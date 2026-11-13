#include "src/bytecode_compiler/storage_compile.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "src/bytecode/opcodes.h"
#include "src/bytecode/panic.h"
#include "src/bytecode/value.h"
#include "src/bytecode_compiler/register_alloc.h"
#include "src/bytecode_compiler/scalar_encoding.h"
#include "src/hir/abi.h"
#include "src/hir/intrinsic_abi.h"
#include "src/intrinsics.h"
#include "src/runtime/aligned_buffer.h"
#include "src/runtime/coroutine.h"
#include "src/runtime/value_layout.h"

namespace cinder::bytecode_compiler {

namespace {

using bytecode::numeric_kind;
using bytecode::opcode;
using bytecode::slot_value;
using hir::op_kind;
using hir::operand_kind;
using hir::pass_class;
using hir::projection_kind;
using semantic::type_id;

template <typename T> using checked = std::expected<T, compile_error>;

[[nodiscard]] auto fail(compile_error_kind kind, const source_span &span,
                        std::string message) -> std::unexpected<compile_error> {
  return std::unexpected(compile_error{
      .kind = kind, .span = span, .message = std::move(message)});
}

[[nodiscard]] auto align_up(size_t value, size_t align) -> size_t {
  return (value + align - 1) / align * align;
}

/// Where a slot's bytes are while the function runs.
struct home {
  enum class kind : uint8_t {
    none,     ///< Zero-sized: no bytes.
    reg,      ///< The value is in `reg`.
    frame,    ///< Bytes in this frame; `reg` holds their address.
    indirect, ///< Bytes in the caller's storage; `reg` holds their address.
    env_block, ///< Bytes in the closure's environment: `reg` holds the block's
               ///< address and `frame_offset` the distance into it.
  };
  kind where = kind::none;
  virtual_reg reg{};
  uint32_t frame_offset = 0;
};

/// Where a place's bytes are, once its projections are followed.
struct location {
  enum class kind : uint8_t {
    none,   ///< Nothing to read or write.
    reg,    ///< A register holds the value.
    memory, ///< Bytes at `reg` + `offset`.
  };
  kind where = kind::none;
  virtual_reg reg{};
  size_t offset = 0;
};

/// A structured region that is open while emitting.
struct open_region {
  enum class kind : uint8_t { conditional, loop, block };
  kind what = kind::block;
  /// `loop`, `block`: the forward jumps that leave it.
  std::vector<size_t> exits;
  /// `loop`: where the back edge returns to.
  size_t header = 0;
  /// `conditional`: the jump taken when the condition is false, the jump over
  /// the `else` side, and whether an `else` was seen.
  size_t false_jump = 0;
  size_t end_jump = 0;
  bool has_else = false;
};

class function_emitter {
public:
  function_emitter(std::string_view entry, const hir::storage_module &module,
                   const hir::storage_function &function,
                   hir::storage_env &env,
                   const std::unordered_map<std::string, uint16_t> &index,
                   const std::unordered_map<std::string, uint32_t> &statics)
      : entry_(entry), module_(module), fn_(function), env_(env),
        index_(index), statics_(statics) {}

  /// The function-table key of a function of this module.
  [[nodiscard]] auto key(std::string_view name) const -> std::string {
    return hir::program_function_name(entry_, module_.name, name);
  }

  /// The function-table key a direct call names.
  [[nodiscard]] auto key(const hir::call_target &callee) const -> std::string {
    return hir::program_callee_name(entry_, module_.name, callee);
  }

  [[nodiscard]] auto compile() -> checked<bytecode::bytecode_function> {
    if (auto ok = prepare(); !ok.has_value()) {
      return std::unexpected(ok.error());
    }
    for (const auto &op : fn_.ops) {
      current_span_ = op.span;
      if (auto ok = emit(op); !ok.has_value()) {
        return std::unexpected(ok.error());
      }
    }
    return finish();
  }

private:
  // ------------------------------------------------------------------
  //  Registers and raw emission
  // ------------------------------------------------------------------

  [[nodiscard]] auto alloc_register() -> virtual_reg {
    return virtual_reg{static_cast<uint32_t>(next_register_++)};
  }

  [[nodiscard]] auto alloc_group(uint32_t count) -> virtual_reg {
    const auto first = alloc_register();
    for (auto i = uint32_t{1}; i < count; ++i) {
      static_cast<void>(alloc_register());
    }
    if (count > 0) {
      groups_.push_back(register_group{.first = first, .count = count});
    }
    return first;
  }

  [[nodiscard]] static auto nth(virtual_reg first, size_t index) -> virtual_reg {
    return virtual_reg{first.id + static_cast<uint32_t>(index)};
  }

  auto emit_op(opcode op) -> void {
    instruction_offsets_.push_back(writer_.current_offset());
    if (!current_span_.empty() &&
        (spans_.empty() || spans_.back().span.start != current_span_.start ||
         spans_.back().span.end != current_span_.end)) {
      spans_.push_back(bytecode::span_entry{
          .offset = static_cast<uint32_t>(writer_.current_offset()),
          .span = current_span_});
    }
    writer_.emit_opcode(op);
  }

  auto emit_register(virtual_reg reg) -> void {
    sites_.push_back(
        register_site{.code_offset = writer_.current_offset(), .reg = reg});
    writer_.emit_u16(0);
  }

  auto emit_move(virtual_reg dst, virtual_reg src) -> void {
    if (dst == src) {
      return;
    }
    emit_op(opcode::op_move);
    emit_register(dst);
    emit_register(src);
  }

  auto emit_const(virtual_reg dst, slot_value value) -> void {
    const auto index = writer_.add_constant(value);
    emit_op(opcode::op_load_const);
    emit_register(dst);
    writer_.emit_u32(index);
  }

  [[nodiscard]] auto emit_load(virtual_reg dst, virtual_reg base, size_t offset,
                               size_t size) -> checked<void> {
    const auto [near, low] = near_base(base, offset);
    emit_op(opcode::op_load_slot);
    emit_register(dst);
    emit_register(near);
    writer_.emit_u16(low);
    writer_.emit_u8(static_cast<uint8_t>(size));
    return {};
  }

  [[nodiscard]] auto emit_store(virtual_reg base, size_t offset, virtual_reg src,
                                size_t size) -> checked<void> {
    const auto [near, low] = near_base(base, offset);
    emit_op(opcode::op_store_slot);
    emit_register(near);
    writer_.emit_u16(low);
    emit_register(src);
    writer_.emit_u8(static_cast<uint8_t>(size));
    return {};
  }

  /// `base + offset` as a register and an offset that fits the 16-bit operand
  /// of a load, store or address instruction. A larger offset moves its high
  /// part into a new register, so a slot or block past 64 KiB is reached at
  /// any byte.
  [[nodiscard]] auto near_base(virtual_reg base, size_t offset)
      -> std::pair<virtual_reg, uint16_t> {
    if (offset <= UINT16_MAX) {
      return {base, static_cast<uint16_t>(offset)};
    }
    const auto high = offset & ~size_t{UINT16_MAX};
    const auto step = alloc_register();
    emit_const(step, slot_value{uint64_t{high}});
    const auto near = alloc_register();
    emit_op(opcode::op_addr_indexed);
    emit_register(near);
    emit_register(base);
    emit_register(step);
    writer_.emit_u8(1);
    return {near, static_cast<uint16_t>(offset - high)};
  }

  /// A register holding `base + offset`.
  [[nodiscard]] auto offset_address(virtual_reg base, size_t offset) -> virtual_reg {
    const auto [near, low] = near_base(base, offset);
    if (low == 0) {
      return near;
    }
    const auto out = alloc_register();
    emit_op(opcode::op_addr_slot);
    emit_register(out);
    emit_register(near);
    writer_.emit_u16(low);
    return out;
  }

  auto emit_jump_back_to(size_t target) -> void {
    emit_op(opcode::op_jump);
    const auto after_operand = static_cast<int64_t>(writer_.current_offset()) + 4;
    writer_.emit_i32(
        static_cast<int32_t>(static_cast<int64_t>(target) - after_operand));
  }

  // ------------------------------------------------------------------
  //  Types
  // ------------------------------------------------------------------

  [[nodiscard]] auto types() const -> const semantic::type_table & {
    return env_.types();
  }

  [[nodiscard]] auto layout_of(type_id type, const source_span &span)
      -> checked<const runtime::value_layout *> {
    const auto found = env_.layouts.layout(type);
    if (!found.has_value()) {
      return fail(compile_error_kind::unsupported_type, span,
                  found.error().message);
    }
    return *found;
  }

  [[nodiscard]] auto class_of(type_id type, const source_span &span)
      -> checked<hir::abi_value> {
    const auto found = hir::classify_value(env_.layouts, type);
    if (!found.has_value()) {
      return fail(compile_error_kind::unsupported_type, span,
                  found.error().message);
    }
    return *found;
  }

  [[nodiscard]] auto kind_of(type_id type, const source_span &span)
      -> checked<numeric_kind> {
    const auto kind = bytecode::numeric_kind_of(types(), types().strip_refinement(type));
    if (!kind.has_value()) {
      return fail(compile_error_kind::unsupported_type, span,
                  std::format("type `{}` has no numeric bytecode "
                              "representation yet — 128-bit floats are not "
                              "supported by the bytecode VM yet",
                              types().display(type)));
    }
    return *kind;
  }

  // ------------------------------------------------------------------
  //  Preparation: where every slot lives
  // ------------------------------------------------------------------

  /// Marks the slots whose own address is taken, which must live in memory.
  auto note_address_taken(const hir::operand &value) -> void {
    if (value.kind == operand_kind::address_of &&
        (value.where.steps.empty() ||
         value.where.steps.front().kind != projection_kind::deref)) {
      taken_.insert(value.where.base);
    }
  }

  [[nodiscard]] auto alloc_frame(size_t size, size_t align) -> checked<uint32_t> {
    frame_bytes_ = align_up(frame_bytes_, std::max<size_t>(align, 1));
    const auto offset = frame_bytes_;
    frame_bytes_ += size;
    if (frame_bytes_ > UINT32_MAX) {
      return fail(compile_error_kind::encoding_limit_exceeded, fn_.slots.empty()
                      ? source_span{}
                      : fn_.slots.front().span,
                  std::format("`{}` needs more than {} bytes of frame storage",
                              fn_.name, UINT32_MAX));
    }
    return static_cast<uint32_t>(offset);
  }

  [[nodiscard]] auto prepare() -> checked<void> {
    const auto operands = fn_.signature.operands();
    if (operands.size() > UINT8_MAX) {
      return fail(compile_error_kind::encoding_limit_exceeded, fn_.slots.empty()
                      ? source_span{}
                      : fn_.slots.front().span,
                  std::format("`{}` takes more than {} operands", fn_.name,
                              UINT8_MAX));
    }
    param_count_ = static_cast<uint16_t>(operands.size());
    next_register_ = operands.size();

    auto operand_of_slot =
        std::vector<std::optional<size_t>>(fn_.slots.size(), std::nullopt);
    for (size_t i = 0; i < operands.size(); ++i) {
      if (operands[i].role == hir::abi_operand_role::result_address) {
        if (fn_.result_slot != hir::k_invalid_storage) {
          operand_of_slot[fn_.result_slot] = i;
        }
      } else if (operands[i].source_index < fn_.param_slots.size()) {
        operand_of_slot[fn_.param_slots[operands[i].source_index]] = i;
      }
    }

    for (const auto &op : fn_.ops) {
      for (const auto &capture : op.captures) {
        if (capture.kind == hir::capture_kind::reference) {
          note_address_taken(hir::operand{.kind = operand_kind::address_of,
                                          .where = capture.source});
        }
      }
      for (const auto &value : op.operands) {
        note_address_taken(value);
      }
      note_address_taken(op.condition);
      for (const auto &arg : op.args) {
        note_address_taken(arg.value);
      }
      if (op.value.has_value()) {
        note_address_taken(*op.value);
      }
    }

    homes_.assign(fn_.slots.size(), home{});
    auto entry_stores = std::vector<hir::storage_id>{};
    auto indirect_captures = std::vector<hir::storage_id>{};
    auto statics = std::vector<hir::storage_id>{};
    for (const auto &slot : fn_.slots) {
      const auto value = class_of(slot.type, slot.span);
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
      auto &here = homes_[slot.id];
      const auto direct = !hir::is_indirect(value->cls);
      const auto zero = value->cls == pass_class::zero_sized;
      const auto taken = taken_.contains(slot.id);
      switch (slot.cls) {
      case hir::storage_class::caller_result:
        if (!operand_of_slot[slot.id].has_value()) {
          return fail(compile_error_kind::internal_error, slot.span,
                      "the result slot has no hidden result operand");
        }
        here = home{.where = home::kind::indirect,
                    .reg = virtual_reg{static_cast<uint32_t>(*operand_of_slot[slot.id])}};
        break;
      case hir::storage_class::parameter:
        if (zero && !taken) {
          break;
        }
        if (zero) {
          if (auto placed = place_anchor(); !placed.has_value()) {
            return std::unexpected(placed.error());
          }
          here = home{.where = home::kind::frame, .frame_offset = *anchor_};
          break;
        }
        if (!operand_of_slot[slot.id].has_value()) {
          return fail(compile_error_kind::internal_error, slot.span,
                      "a parameter slot has no operand");
        }
        if (!direct) {
          here = home{.where = home::kind::indirect,
                      .reg = virtual_reg{static_cast<uint32_t>(*operand_of_slot[slot.id])}};
        } else if (taken) {
          const auto offset = alloc_frame(value->size, value->align);
          if (!offset.has_value()) {
            return std::unexpected(offset.error());
          }
          here = home{.where = home::kind::frame, .frame_offset = *offset};
          entry_stores.push_back(slot.id);
        } else {
          here = home{.where = home::kind::reg,
                      .reg = virtual_reg{static_cast<uint32_t>(*operand_of_slot[slot.id])}};
        }
        break;
      case hir::storage_class::frame_local:
      case hir::storage_class::outgoing_argument:
        if (zero && !taken) {
          break;
        }
        if (zero) {
          if (auto placed = place_anchor(); !placed.has_value()) {
            return std::unexpected(placed.error());
          }
          here = home{.where = home::kind::frame, .frame_offset = *anchor_};
        } else if (direct && !taken) {
          here = home{.where = home::kind::reg, .reg = alloc_register()};
        } else {
          const auto offset = alloc_frame(value->size, value->align);
          if (!offset.has_value()) {
            return std::unexpected(offset.error());
          }
          here = home{.where = home::kind::frame, .frame_offset = *offset};
        }
        break;
      case hir::storage_class::coroutine_frame:
        // A generator's locals are bytes of the state block it was handed,
        // which is its first operand and outlives every suspension.
        if (zero) {
          if (auto placed = place_anchor(); !placed.has_value()) {
            return std::unexpected(placed.error());
          }
          here = home{.where = home::kind::frame, .frame_offset = *anchor_};
        } else {
          here = home{.where = home::kind::env_block,
                      .reg = virtual_reg{0},
                      .frame_offset = static_cast<uint32_t>(slot.env_offset)};
        }
        break;
      case hir::storage_class::closure_environment:
        if (slot.env_indirect) {
          here = home{.where = home::kind::indirect, .reg = alloc_register()};
          indirect_captures.push_back(slot.id);
        } else {
          here = home{.where = home::kind::env_block,
                      .reg = virtual_reg{0},
                      .frame_offset = static_cast<uint32_t>(slot.env_offset)};
        }
        break;
      case hir::storage_class::global:
        // A static lives in the program's static storage, reached through a
        // register set once at entry.
        if (!statics_.contains(slot.name)) {
          return fail(compile_error_kind::internal_error, slot.span,
                      std::format("no module declares the static `{}`",
                                  slot.name));
        }
        here = home{.where = home::kind::indirect, .reg = alloc_register()};
        statics.push_back(slot.id);
        break;
      default:
        return fail(compile_error_kind::unsupported_construct, slot.span,
                    std::format("a `{}` slot is not placed by the bytecode "
                                "VM yet",
                                hir::storage_class_name(slot.cls)));
      }
    }

    // Frame storage is addressed through one register per slot, set up once.
    for (const auto &slot : fn_.slots) {
      auto &here = homes_[slot.id];
      if (here.where != home::kind::frame) {
        continue;
      }
      here.reg = alloc_register();
      emit_op(opcode::op_stack_alloc);
      emit_register(here.reg);
      writer_.emit_u32(here.frame_offset);
    }
    for (const auto id : statics) {
      emit_op(opcode::op_global_addr);
      emit_register(homes_[id].reg);
      writer_.emit_u32(statics_.at(fn_.slots[id].name));
    }
    // A variable captured by reference is reached through the address the
    // environment holds.
    for (const auto id : indirect_captures) {
      current_span_ = fn_.slots[id].span;
      if (auto ok = emit_load(homes_[id].reg, virtual_reg{0},
                              fn_.slots[id].env_offset, 8);
          !ok.has_value()) {
        return ok;
      }
    }
    for (const auto id : entry_stores) {
      const auto &slot = fn_.slots[id];
      const auto size = env_.layouts.size_of(slot.type);
      if (!size.has_value()) {
        return fail(compile_error_kind::unsupported_type, slot.span,
                    size.error().message);
      }
      // The incoming register is the parameter's operand register.
      const auto incoming = std::ranges::find(fn_.param_slots, id);
      const auto source_index = static_cast<size_t>(incoming - fn_.param_slots.begin());
      auto position = size_t{0};
      for (const auto &operand : operands) {
        if (operand.role == hir::abi_operand_role::argument &&
            operand.source_index == source_index) {
          break;
        }
        ++position;
      }
      current_span_ = slot.span;
      if (auto ok = emit_store(homes_[id].reg, 0,
                               virtual_reg{static_cast<uint32_t>(position)}, *size);
          !ok.has_value()) {
        return ok;
      }
    }
    return {};
  }

  /// A small aligned region that gives a zero-sized value an address to be
  /// borrowed at; nothing is ever read or written through it.
  [[nodiscard]] auto place_anchor() -> checked<void> {
    if (anchor_.has_value()) {
      return {};
    }
    const auto offset = alloc_frame(runtime::k_frame_storage_alignment,
                                    runtime::k_frame_storage_alignment);
    if (!offset.has_value()) {
      return std::unexpected(offset.error());
    }
    anchor_ = *offset;
    return {};
  }

  // ------------------------------------------------------------------
  //  Places
  // ------------------------------------------------------------------

  /// Follows `where`'s projections from its slot's home. A dereference loads
  /// the address the value holds, so it may emit code.
  [[nodiscard]] auto locate(const hir::place &where) -> checked<location> {
    const auto &slot = fn_.slots[where.base];
    const auto &here = homes_[where.base];
    auto current = slot.type;
    auto loc = location{};
    auto step = size_t{0};
    switch (here.where) {
    case home::kind::none:
      return location{};
    case home::kind::reg:
      if (where.steps.empty()) {
        return location{.where = location::kind::reg, .reg = here.reg};
      }
      // Only a dereference can follow a value held in a register: the value
      // is the address of what comes next.
      loc = location{.where = location::kind::memory, .reg = here.reg};
      current = env_.pointee(current).value_or(semantic::k_unknown_type);
      step = 1;
      break;
    case home::kind::frame:
    case home::kind::indirect:
      loc = location{.where = location::kind::memory, .reg = here.reg};
      break;
    case home::kind::env_block:
      loc = location{.where = location::kind::memory,
                     .reg = here.reg,
                     .offset = here.frame_offset};
      break;
    }
    for (; step < where.steps.size(); ++step) {
      const auto &projection = where.steps[step];
      if (projection.kind == projection_kind::deref) {
        const auto pointer = alloc_register();
        if (auto ok = emit_load(pointer, loc.reg, loc.offset, 8); !ok.has_value()) {
          return std::unexpected(ok.error());
        }
        loc = location{.where = location::kind::memory, .reg = pointer};
        current = env_.pointee(current).value_or(semantic::k_unknown_type);
        continue;
      }
      const auto layout = layout_of(current, slot.span);
      if (!layout.has_value()) {
        return std::unexpected(layout.error());
      }
      const auto &found = **layout;
      switch (projection.kind) {
      case projection_kind::member:
        loc.offset += found.members[projection.index].offset;
        current = found.members[projection.index].type;
        break;
      case projection_kind::element:
        loc.offset += found.stride * projection.index;
        current = found.element;
        break;
      case projection_kind::payload: {
        const auto *variant = found.find_variant(projection.name);
        loc.offset += variant->payload[projection.index].offset;
        current = variant->payload[projection.index].type;
        break;
      }
      case projection_kind::length:
        loc.offset += runtime::k_descriptor_len_offset;
        current = projection.type;
        break;
      case projection_kind::data:
        loc.offset += runtime::k_descriptor_data_offset;
        current = projection.type;
        break;
      case projection_kind::deref:
        break;
      }
    }
    return loc;
  }

  [[nodiscard]] auto size_of(type_id type, const source_span &span)
      -> checked<size_t> {
    const auto found = env_.layouts.size_of(type);
    if (!found.has_value()) {
      return fail(compile_error_kind::unsupported_type, span,
                  found.error().message);
    }
    return *found;
  }

  /// A register holding the address of the bytes at `loc`.
  [[nodiscard]] auto address_of(const location &loc) -> checked<virtual_reg> {
    if (loc.where != location::kind::memory) {
      return fail(compile_error_kind::internal_error, current_span_,
                  "the address of a value that lives in a register was "
                  "taken");
    }
    return offset_address(loc.reg, loc.offset);
  }

  /// The scalar-class value at `loc`, in `into` when a register is needed.
  [[nodiscard]] auto read_location(const location &loc, type_id type,
                                   std::optional<virtual_reg> into)
      -> checked<virtual_reg> {
    if (loc.where == location::kind::reg) {
      return loc.reg;
    }
    if (loc.where != location::kind::memory) {
      return fail(compile_error_kind::internal_error, current_span_,
                  "a value was read from storage that holds none");
    }
    const auto size = size_of(type, current_span_);
    if (!size.has_value()) {
      return std::unexpected(size.error());
    }
    const auto dst = into.value_or(alloc_register());
    if (auto ok = emit_load(dst, loc.reg, loc.offset, *size); !ok.has_value()) {
      return std::unexpected(ok.error());
    }
    // A load zero-extends; a signed integer is held sign-extended in its
    // register, so restore that for the instructions that read it whole.
    const auto kind = bytecode::numeric_kind_of(types(), types().strip_refinement(type));
    if (kind.has_value() && bytecode::is_signed_integer(*kind) &&
        bytecode::bit_width(*kind) < 64) {
      emit_op(opcode::op_cast);
      emit_register(dst);
      emit_register(dst);
      writer_.emit_numeric_kind(*kind);
      writer_.emit_numeric_kind(*kind);
    }
    return dst;
  }

  [[nodiscard]] auto write_location(const location &loc, type_id type,
                                    virtual_reg src) -> checked<void> {
    if (loc.where == location::kind::reg) {
      emit_move(loc.reg, src);
      return {};
    }
    if (loc.where != location::kind::memory) {
      return {};
    }
    const auto size = size_of(type, current_span_);
    if (!size.has_value()) {
      return std::unexpected(size.error());
    }
    return emit_store(loc.reg, loc.offset, src, *size);
  }

  // ------------------------------------------------------------------
  //  Operands
  // ------------------------------------------------------------------

  /// The register holding a scalar operand's value, built in `into` when it
  /// needs a register of its own.
  [[nodiscard]] auto operand_value(const hir::operand &value,
                                   std::optional<virtual_reg> into = std::nullopt)
      -> checked<virtual_reg> {
    switch (value.kind) {
    case operand_kind::constant: {
      const auto dst = into.value_or(alloc_register());
      if (value.literal == token_kind::kw_unit) {
        emit_const(dst, slot_value{uint64_t{0}});
        return dst;
      }
      const auto kind = kind_of(value.type, current_span_);
      if (!kind.has_value()) {
        return std::unexpected(kind.error());
      }
      const auto encoded =
          encode_literal(value.literal, value.text, current_span_, *kind);
      if (!encoded.has_value()) {
        return std::unexpected(encoded.error());
      }
      emit_const(dst, *encoded);
      return dst;
    }
    case operand_kind::read: {
      const auto loc = locate(value.where);
      if (!loc.has_value()) {
        return std::unexpected(loc.error());
      }
      return read_location(*loc, value.where.type, into);
    }
    case operand_kind::address_of: {
      const auto loc = locate(value.where);
      if (!loc.has_value()) {
        return std::unexpected(loc.error());
      }
      const auto address = address_of(*loc);
      if (!address.has_value()) {
        return std::unexpected(address.error());
      }
      if (into.has_value()) {
        emit_move(*into, *address);
        return *into;
      }
      return *address;
    }
    }
    return fail(compile_error_kind::internal_error, current_span_,
                "an operand of an unknown kind");
  }

  // ------------------------------------------------------------------
  //  Operations
  // ------------------------------------------------------------------

  [[nodiscard]] auto emit(const hir::storage_op &op) -> checked<void> {
    switch (op.kind) {
    case op_kind::compute:
      return emit_compute(op);
    case op_kind::init_empty:
    case op_kind::storage_dead:
      return {};
    case op_kind::copy_into:
    case op_kind::move_into:
      return emit_transfer(op);
    case op_kind::replicate:
      return emit_replicate(op);
    case op_kind::publish_variant:
      return emit_publish(op);
    case op_kind::test_variant:
      return emit_test_variant(op);
    case op_kind::drop_in_place:
      return emit_drop(op);
    case op_kind::forget:
      return {}; // The value's drops run through a raw pointer instead.
    case op_kind::zero_fill:
      return emit_zero_fill(op);
    case op_kind::make_closure:
      return emit_make_closure(op);
    case op_kind::release_environment:
      return emit_release_environment(op);
    case op_kind::make_generator:
      return emit_make_generator(op);
    case op_kind::generator_resume:
      return emit_generator_resume(op);
    case op_kind::generator_take:
      return emit_generator_take(op);
    case op_kind::coroutine_has_item:
      return emit_coroutine_has_item(op);
    case op_kind::coroutine_dispatch:
      return emit_coroutine_dispatch(op);
    case op_kind::coroutine_suspend:
      return emit_coroutine_suspend(op);
    case op_kind::coroutine_await:
      return emit_coroutine_await(op);
    case op_kind::task_block_on_group:
      return emit_task_block_on_group(op);
    case op_kind::coroutine_cancelled:
      return emit_coroutine_cancelled(op);
    case op_kind::coroutine_finish:
      return emit_coroutine_finish(op);
    case op_kind::call_into:
      return emit_call(op);
    case op_kind::return_value:
      return emit_return(op);
    case op_kind::unreachable:
      // The call before it does not return; a panic here is the backstop.
      emit_op(opcode::op_panic);
      return {};
    case op_kind::check:
      return emit_check(op);
    case op_kind::string_literal:
      return emit_string_literal(op);
    case op_kind::if_begin:
      return emit_if_begin(op);
    case op_kind::else_begin:
      return emit_else();
    case op_kind::if_end:
      return emit_if_end();
    case op_kind::loop_begin:
      regions_.push_back(open_region{.what = open_region::kind::loop,
                                     .header = writer_.current_offset()});
      return {};
    case op_kind::loop_end:
      return emit_loop_end();
    case op_kind::block_begin:
      regions_.push_back(open_region{.what = open_region::kind::block});
      return {};
    case op_kind::block_end:
      return emit_block_end();
    case op_kind::exit:
      return emit_exit(op);
    }
    return fail(compile_error_kind::internal_error, op.span,
                "an operation of an unknown kind");
  }

  [[nodiscard]] auto emit_compute(const hir::storage_op &op) -> checked<void> {
    const auto dest = locate(op.dest);
    if (!dest.has_value()) {
      return std::unexpected(dest.error());
    }
    const auto target = dest->where == location::kind::reg ? dest->reg
                                                          : alloc_register();
    switch (op.computation) {
    case hir::compute_kind::constant: {
      hir::operand literal{.kind = operand_kind::constant,
                           .type = op.dest.type,
                           .literal = op.literal,
                           .text = op.text};
      if (auto ok = operand_value(literal, target); !ok.has_value()) {
        return std::unexpected(ok.error());
      }
      break;
    }
    case hir::compute_kind::copy: {
      const auto value = operand_value(op.operands[0], target);
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
      emit_move(target, *value);
      break;
    }
    case hir::compute_kind::binary: {
      if (auto ok = emit_binary(op, target); !ok.has_value()) {
        return ok;
      }
      break;
    }
    case hir::compute_kind::unary: {
      if (auto ok = emit_unary(op, target); !ok.has_value()) {
        return ok;
      }
      break;
    }
    case hir::compute_kind::pointer_offset: {
      if (auto ok = emit_pointer_offset(op, target); !ok.has_value()) {
        return ok;
      }
      break;
    }
    case hir::compute_kind::checked_index:
    case hir::compute_kind::checked_end: {
      if (auto ok = emit_bounds_check(op, target); !ok.has_value()) {
        return ok;
      }
      break;
    }
    case hir::compute_kind::cast: {
      const auto from = kind_of(op.operands[0].type, op.span);
      const auto to = kind_of(op.dest.type, op.span);
      if (!from.has_value()) {
        return std::unexpected(from.error());
      }
      if (!to.has_value()) {
        return std::unexpected(to.error());
      }
      const auto value = operand_value(op.operands[0]);
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
      emit_op(opcode::op_cast);
      emit_register(target);
      emit_register(*value);
      writer_.emit_numeric_kind(*from);
      writer_.emit_numeric_kind(*to);
      break;
    }
    }
    if (dest->where == location::kind::memory) {
      return write_location(*dest, op.dest.type, target);
    }
    return {};
  }

  /// `p + i * stride` bytes. `op_addr_indexed` scales by a one-byte element
  /// size, so a larger stride is multiplied out first; the arithmetic wraps,
  /// because the machine layer owns the bounds.
  [[nodiscard]] auto emit_pointer_offset(const hir::storage_op &op,
                                         virtual_reg target) -> checked<void> {
    const auto base = operand_value(op.operands[0]);
    if (!base.has_value()) {
      return std::unexpected(base.error());
    }
    const auto count = operand_value(op.operands[1]);
    if (!count.has_value()) {
      return std::unexpected(count.error());
    }
    auto index = *count;
    auto scale = op.stride;
    if (scale > UINT8_MAX) {
      const auto factor = alloc_register();
      emit_const(factor, slot_value{uint64_t{op.stride}});
      index = alloc_register();
      emit_op(opcode::op_mul);
      emit_register(index);
      emit_register(*count);
      emit_register(factor);
      writer_.emit_numeric_kind(numeric_kind::u64);
      scale = 1;
    }
    emit_op(opcode::op_addr_indexed);
    emit_register(target);
    emit_register(*base);
    emit_register(index);
    writer_.emit_u8(static_cast<uint8_t>(scale));
    return {};
  }

  /// A contract the checker could not discharge: `op_panic_if` panics when
  /// its register is true, so the condition is negated first, and the panic
  /// names which promise broke.
  [[nodiscard]] auto emit_check(const hir::storage_op &op) -> checked<void> {
    const auto condition = operand_value(op.condition);
    if (!condition.has_value()) {
      return std::unexpected(condition.error());
    }
    const auto violated = alloc_register();
    emit_op(opcode::op_not_bool);
    emit_register(violated);
    emit_register(*condition);
    emit_op(opcode::op_panic_if);
    emit_register(violated);
    writer_.emit_u8(static_cast<uint8_t>(contract_panic_reason(op.contract)));
    return {};
  }

  [[nodiscard]] static auto contract_panic_reason(hir::check_kind kind) noexcept
      -> bytecode::panic_reason {
    switch (kind) {
    case hir::check_kind::precondition:
      return bytecode::panic_reason::precondition_violated;
    case hir::check_kind::postcondition:
      return bytecode::panic_reason::postcondition_violated;
    case hir::check_kind::invariant:
      return bytecode::panic_reason::invariant_violated;
    }
    return bytecode::panic_reason::explicit_panic;
  }

  /// `checked_index` panics when the value is at or past the limit,
  /// `checked_end` when it is past it; otherwise the value is the result.
  [[nodiscard]] auto emit_bounds_check(const hir::storage_op &op,
                                       virtual_reg target) -> checked<void> {
    const auto value = operand_value(op.operands[0]);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    const auto limit = operand_value(op.operands[1]);
    if (!limit.has_value()) {
      return std::unexpected(limit.error());
    }
    const auto outside = alloc_register();
    emit_op(op.computation == hir::compute_kind::checked_index ? opcode::op_ge
                                                               : opcode::op_gt);
    emit_register(outside);
    emit_register(*value);
    emit_register(*limit);
    writer_.emit_numeric_kind(numeric_kind::u64);
    emit_op(opcode::op_panic_if);
    emit_register(outside);
    writer_.emit_u8(
        static_cast<uint8_t>(bytecode::panic_reason::index_out_of_bounds));
    emit_move(target, *value);
    return {};
  }

  [[nodiscard]] auto emit_binary(const hir::storage_op &op, virtual_reg target)
      -> checked<void> {
    const auto code = binary_opcode_for(op.binary);
    if (!code.has_value()) {
      return fail(compile_error_kind::unsupported_construct, op.span,
                  std::format("operator `{}` is not supported by the bytecode "
                              "compiler yet",
                              ast::binary_op_name(op.binary)));
    }
    const auto kind = kind_of(op.operands[0].type, op.span);
    if (!kind.has_value()) {
      return std::unexpected(kind.error());
    }
    const auto lhs = operand_value(op.operands[0]);
    if (!lhs.has_value()) {
      return std::unexpected(lhs.error());
    }
    const auto rhs = operand_value(op.operands[1]);
    if (!rhs.has_value()) {
      return std::unexpected(rhs.error());
    }
    emit_op(*code);
    emit_register(target);
    emit_register(*lhs);
    emit_register(*rhs);
    writer_.emit_numeric_kind(*kind);
    return {};
  }

  [[nodiscard]] auto emit_unary(const hir::storage_op &op, virtual_reg target)
      -> checked<void> {
    const auto &value = op.operands[0];
    if (op.unary == ast::unary_op::logical_not) {
      const auto src = operand_value(value);
      if (!src.has_value()) {
        return std::unexpected(src.error());
      }
      emit_op(opcode::op_not_bool);
      emit_register(target);
      emit_register(*src);
      return {};
    }
    if (op.unary != ast::unary_op::neg && op.unary != ast::unary_op::bit_not) {
      return fail(compile_error_kind::unsupported_construct, op.span,
                  std::format("operator `{}` needs a reference model this "
                              "compiler does not have yet",
                              ast::unary_op_name(op.unary)));
    }
    const auto kind = kind_of(value.type, op.span);
    if (!kind.has_value()) {
      return std::unexpected(kind.error());
    }
    if (op.unary == ast::unary_op::neg && value.kind == operand_kind::constant &&
        value.literal == token_kind::int_lit) {
      // `-128` as `int8`: negate the literal's magnitude here, since the
      // magnitude alone does not fit and would trip the run-time guard.
      const auto magnitude = encode_literal(value.literal, value.text, op.span, *kind);
      if (!magnitude.has_value()) {
        return std::unexpected(magnitude.error());
      }
      emit_const(target, negate_literal(*magnitude, *kind));
      return {};
    }
    const auto src = operand_value(value);
    if (!src.has_value()) {
      return std::unexpected(src.error());
    }
    emit_op(op.unary == ast::unary_op::neg ? opcode::op_neg : opcode::op_bitnot);
    emit_register(target);
    emit_register(*src);
    writer_.emit_numeric_kind(*kind);
    return {};
  }

  [[nodiscard]] auto emit_zero_fill(const hir::storage_op &op) -> checked<void> {
    const auto value = class_of(op.dest.type, op.span);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    if (value->size == 0) {
      return {};
    }
    const auto to = locate(op.dest);
    if (!to.has_value()) {
      return std::unexpected(to.error());
    }
    const auto address = address_of(*to);
    if (!address.has_value()) {
      return std::unexpected(address.error());
    }
    emit_op(opcode::op_zero_bytes);
    emit_register(*address);
    writer_.emit_u32(static_cast<uint32_t>(value->size));
    return {};
  }

  [[nodiscard]] auto emit_transfer(const hir::storage_op &op) -> checked<void> {
    const auto value = class_of(op.dest.type, op.span);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    if (value->cls == pass_class::zero_sized) {
      return {};
    }
    const auto from = locate(op.source);
    if (!from.has_value()) {
      return std::unexpected(from.error());
    }
    const auto to = locate(op.dest);
    if (!to.has_value()) {
      return std::unexpected(to.error());
    }
    if (!hir::is_indirect(value->cls)) {
      const auto held = read_location(*from, op.source.type, std::nullopt);
      if (!held.has_value()) {
        return std::unexpected(held.error());
      }
      return write_location(*to, op.dest.type, *held);
    }
    const auto source_address = address_of(*from);
    if (!source_address.has_value()) {
      return std::unexpected(source_address.error());
    }
    const auto dest_address = address_of(*to);
    if (!dest_address.has_value()) {
      return std::unexpected(dest_address.error());
    }
    emit_op(opcode::op_copy_bytes);
    emit_register(*dest_address);
    emit_register(*source_address);
    writer_.emit_u32(static_cast<uint32_t>(value->size));
    return {};
  }

  /// Copies element 0 over the rest of the array by doubling the written
  /// prefix: about log2(count) copies, each of a constant size and offset.
  [[nodiscard]] auto emit_replicate(const hir::storage_op &op) -> checked<void> {
    const auto layout = layout_of(op.dest.type, op.span);
    if (!layout.has_value()) {
      return std::unexpected(layout.error());
    }
    const auto count = static_cast<size_t>((*layout)->count);
    const auto stride = (*layout)->stride;
    if (count < 2 || stride == 0) {
      return {};
    }
    const auto to = locate(op.dest);
    if (!to.has_value()) {
      return std::unexpected(to.error());
    }
    if (to->where != location::kind::memory) {
      return fail(compile_error_kind::internal_error, op.span,
                  "an array of more than one element outside memory");
    }
    const auto first = address_of(*to);
    if (!first.has_value()) {
      return std::unexpected(first.error());
    }
    for (auto filled = size_t{1}; filled < count;) {
      const auto chunk = std::min(filled, count - filled);
      auto next = *to;
      next.offset += filled * stride;
      const auto at = address_of(next);
      if (!at.has_value()) {
        return std::unexpected(at.error());
      }
      emit_op(opcode::op_copy_bytes);
      emit_register(*at);
      emit_register(*first);
      writer_.emit_u32(static_cast<uint32_t>(chunk * stride));
      filled += chunk;
    }
    return {};
  }

  [[nodiscard]] auto emit_publish(const hir::storage_op &op) -> checked<void> {
    const auto layout = layout_of(op.dest.type, op.span);
    if (!layout.has_value()) {
      return std::unexpected(layout.error());
    }
    const auto *variant = (*layout)->find_variant(op.variant);
    if (variant == nullptr) {
      return fail(compile_error_kind::internal_error, op.span,
                  std::format("`{}` has no variant `{}`",
                              types().display(op.dest.type), op.variant));
    }
    const auto dest = locate(op.dest);
    if (!dest.has_value()) {
      return std::unexpected(dest.error());
    }
    const auto tag = alloc_register();
    emit_const(tag, slot_value{static_cast<uint64_t>(variant->tag)});
    return emit_store(dest->reg, dest->offset + (*layout)->tag_offset, tag,
                      runtime::k_sum_tag_size);
  }

  /// Compares the tag of the sum at `source` with the tag of `variant`.
  [[nodiscard]] auto emit_test_variant(const hir::storage_op &op)
      -> checked<void> {
    const auto layout = layout_of(op.source.type, op.span);
    if (!layout.has_value()) {
      return std::unexpected(layout.error());
    }
    const auto *variant = (*layout)->find_variant(op.variant);
    if (variant == nullptr) {
      return fail(compile_error_kind::internal_error, op.span,
                  std::format("`{}` has no variant `{}`",
                              types().display(op.source.type), op.variant));
    }
    const auto source = locate(op.source);
    if (!source.has_value()) {
      return std::unexpected(source.error());
    }
    const auto dest = locate(op.dest);
    if (!dest.has_value()) {
      return std::unexpected(dest.error());
    }
    if (source->where != location::kind::memory) {
      return fail(compile_error_kind::internal_error, op.span,
                  "a sum was not in addressable storage");
    }
    const auto tag = alloc_register();
    if (auto ok = emit_load(tag, source->reg,
                            source->offset + (*layout)->tag_offset,
                            runtime::k_sum_tag_size);
        !ok.has_value()) {
      return ok;
    }
    const auto expected = alloc_register();
    emit_const(expected, slot_value{static_cast<uint64_t>(variant->tag)});
    const auto target = dest->where == location::kind::reg ? dest->reg
                                                          : alloc_register();
    emit_op(opcode::op_eq);
    emit_register(target);
    emit_register(tag);
    emit_register(expected);
    writer_.emit_numeric_kind(bytecode::numeric_kind::u64);
    if (dest->where == location::kind::memory) {
      return write_location(*dest, op.dest.type, target);
    }
    return {};
  }

  [[nodiscard]] auto emit_call(const hir::storage_op &op) -> checked<void> {
    if (!op.callee.is_direct()) {
      return emit_call_function(op);
    }
    const auto target = key(op.callee);
    if (!index_.contains(target)) {
      if (const auto which = cinder::inline_intrinsic_of(op.callee.name);
          which.has_value()) {
        return emit_inline_intrinsic(op, *which);
      }
      if (const auto id = hir::runtime_intrinsic_id(op.callee.name);
          id.has_value()) {
        return emit_intrinsic_call(op, *id);
      }
    }
    const auto found = index_.find(target);
    if (found == index_.end()) {
      return fail(compile_error_kind::unknown_callee, op.span,
                  std::format("`{}` is not a function of this program", target));
    }
    const auto operands = op.signature.operands();
    const auto block = fill_call_block(op, operands.size());
    if (!block.has_value()) {
      return std::unexpected(block.error());
    }
    if (op.tail_call) {
      // The callee takes over this frame; its return is this function's
      // (`hir::is_safe_tail_call`). The `return_value` after it is not reached.
      emit_op(opcode::op_tail_call);
      writer_.emit_u16(found->second);
      emit_register(*block);
      writer_.emit_u8(static_cast<uint8_t>(operands.size()));
      return {};
    }
    const auto dest = locate(op.dest);
    if (!dest.has_value()) {
      return std::unexpected(dest.error());
    }
    // A direct result comes back in a register: straight into the destination
    // when that is one, otherwise through a temporary stored afterwards.
    const auto direct_result =
        !op.signature.returns_through_address() &&
        op.signature.result.cls != pass_class::zero_sized;
    const auto result =
        direct_result && dest->where == location::kind::reg ? dest->reg
                                                            : alloc_register();
    emit_op(opcode::op_call);
    emit_register(result);
    writer_.emit_u16(found->second);
    emit_register(*block);
    writer_.emit_u8(static_cast<uint8_t>(operands.size()));
    if (direct_result && dest->where == location::kind::memory) {
      return write_location(*dest, op.dest.type, result);
    }
    return {};
  }

  /// A call through a `fn` value: its entry and environment are read from the
  /// value's bytes, and the environment is the callee's first operand.
  [[nodiscard]] auto emit_call_function(const hir::storage_op &op)
      -> checked<void> {
    const auto closure = locate(*op.callee.callable);
    if (!closure.has_value()) {
      return std::unexpected(closure.error());
    }
    if (closure->where != location::kind::memory) {
      return fail(compile_error_kind::internal_error, op.span,
                  "a `fn` value does not live in memory");
    }
    const auto entry = alloc_register();
    if (auto ok = emit_load(entry, closure->reg, closure->offset, 8);
        !ok.has_value()) {
      return ok;
    }
    const auto environment = alloc_register();
    if (auto ok = emit_load(environment, closure->reg, closure->offset + 8, 8);
        !ok.has_value()) {
      return ok;
    }
    const auto operand_count = op.signature.operands().size() + 1;
    const auto block = fill_call_block(op, operand_count, environment);
    if (!block.has_value()) {
      return std::unexpected(block.error());
    }
    const auto dest = locate(op.dest);
    if (!dest.has_value()) {
      return std::unexpected(dest.error());
    }
    const auto direct_result =
        !op.signature.returns_through_address() &&
        op.signature.result.cls != pass_class::zero_sized;
    const auto result =
        direct_result && dest->where == location::kind::reg ? dest->reg
                                                            : alloc_register();
    emit_op(opcode::op_call_function);
    emit_register(result);
    emit_register(entry);
    emit_register(*block);
    writer_.emit_u8(static_cast<uint8_t>(operand_count));
    if (direct_result && dest->where == location::kind::memory) {
      return write_location(*dest, op.dest.type, result);
    }
    return {};
  }

  /// Builds a `fn` value `{entry, env, glue}` in its destination. A non-empty
  /// environment is one heap block that every capture is stored into.
  [[nodiscard]] auto emit_make_closure(const hir::storage_op &op)
      -> checked<void> {
    const auto entry = index_.find(key(op.entry));
    if (entry == index_.end()) {
      return fail(compile_error_kind::unknown_callee, op.span,
                  std::format("`{}` is not a function of this module", op.entry));
    }
    auto glue_word = uint64_t{0};
    if (!op.glue.empty()) {
      const auto glue = index_.find(key(op.glue));
      if (glue == index_.end()) {
        return fail(compile_error_kind::unknown_callee, op.span,
                    std::format("`{}` is not a function of this module", op.glue));
      }
      glue_word = uint64_t{glue->second} + 1;
    }
    const auto environment = alloc_register();
    if (op.env_size != 0) {
      emit_op(opcode::op_alloc);
      emit_register(environment);
      writer_.emit_u64(op.env_size);
      for (const auto &capture : op.captures) {
        if (auto ok = emit_capture(environment, capture); !ok.has_value()) {
          return ok;
        }
      }
    } else {
      emit_const(environment, slot_value{uint64_t{0}});
    }
    const auto dest = locate(op.dest);
    if (!dest.has_value()) {
      return std::unexpected(dest.error());
    }
    if (dest->where != location::kind::memory) {
      return fail(compile_error_kind::internal_error, op.span,
                  "a `fn` value does not live in memory");
    }
    const auto word = alloc_register();
    emit_const(word, slot_value{static_cast<uint64_t>(entry->second)});
    if (auto ok = emit_store(dest->reg, dest->offset, word, 8); !ok.has_value()) {
      return ok;
    }
    if (auto ok = emit_store(dest->reg, dest->offset + 8, environment, 8);
        !ok.has_value()) {
      return ok;
    }
    emit_const(word, slot_value{glue_word});
    return emit_store(dest->reg, dest->offset + 16, word, 8);
  }

  /// Stores one captured variable into the environment block at `environment`.
  [[nodiscard]] auto emit_capture(virtual_reg environment,
                                  const hir::closure_capture &capture)
      -> checked<void> {
    const auto source = locate(capture.source);
    if (!source.has_value()) {
      return std::unexpected(source.error());
    }
    if (capture.kind == hir::capture_kind::reference) {
      const auto address = address_of(*source);
      if (!address.has_value()) {
        return std::unexpected(address.error());
      }
      return emit_store(environment, capture.offset, *address, 8);
    }
    const auto value = class_of(capture.source.type, current_span_);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    if (!hir::is_indirect(value->cls)) {
      const auto held = read_location(*source, capture.source.type, std::nullopt);
      if (!held.has_value()) {
        return std::unexpected(held.error());
      }
      return emit_store(environment, capture.offset, *held, value->size);
    }
    const auto from = address_of(*source);
    if (!from.has_value()) {
      return std::unexpected(from.error());
    }
    const auto to = offset_address(environment, capture.offset);
    emit_op(opcode::op_copy_bytes);
    emit_register(to);
    emit_register(*from);
    writer_.emit_u32(static_cast<uint32_t>(value->size));
    return {};
  }

  /// Ends a `fn` value: runs its drop glue, which drops what the environment
  /// owns and frees the block. Any other drop is still ordinary code.
  [[nodiscard]] auto emit_drop(const hir::storage_op &op) -> checked<void> {
    if (is_generator(op.dest.type)) {
      return emit_generator_drop(op);
    }
    if (types().entry(types().strip_refinement(op.dest.type)).kind !=
        semantic::type_kind::fn_kind) {
      return fail(compile_error_kind::unsupported_construct, op.span,
                  std::format("`drop_in_place` of `{}` has no bytecode form yet",
                              types().display(op.dest.type)));
    }
    const auto where = locate(op.dest);
    if (!where.has_value()) {
      return std::unexpected(where.error());
    }
    const auto address = address_of(*where);
    if (!address.has_value()) {
      return std::unexpected(address.error());
    }
    emit_op(opcode::op_drop_function_value);
    emit_register(alloc_register());
    emit_register(*address);
    return {};
  }

  // ------------------------------------------------------------------
  //  Generators
  //
  //  A coroutine is the runtime's eight-word object (`coroutine.h`). A step
  //  function runs against the state block the object points at; the value
  //  word holds the address of the item the producer last yielded, which lies
  //  in that block, so an item of any size needs no extra room in the object.
  // ------------------------------------------------------------------

  /// Whether `type` is a coroutine handle: a generator or a task.
  [[nodiscard]] auto is_generator(type_id type) const -> bool {
    const auto &entry = types().entry(types().strip_refinement(type));
    return entry.kind == semantic::type_kind::builtin_generic_kind &&
           (entry.name == "generator" || entry.name == "task");
  }

  /// The register holding the pointer of the coroutine (or the scalar) at
  /// `where`.
  [[nodiscard]] auto read_place(const hir::place &where) -> checked<virtual_reg> {
    return operand_value(hir::operand{
        .kind = operand_kind::read, .where = where, .type = where.type});
  }

  [[nodiscard]] static constexpr auto word_at(size_t slot) -> size_t {
    return slot * runtime::k_word_size;
  }

  /// Stores the constant `value` into word `slot` of the coroutine in `object`.
  [[nodiscard]] auto store_word(virtual_reg object, size_t slot, uint64_t value)
      -> checked<void> {
    const auto word = alloc_register();
    emit_const(word, slot_value{value});
    return emit_store(object, word_at(slot), word, runtime::k_word_size);
  }

  [[nodiscard]] auto emit_make_generator(const hir::storage_op &op)
      -> checked<void> {
    namespace co = runtime::coroutine;
    const auto entry = index_.find(key(op.entry));
    if (entry == index_.end()) {
      return fail(compile_error_kind::unknown_callee, op.span,
                  std::format("`{}` is not a function of this module", op.entry));
    }
    const auto state = alloc_register();
    if (op.env_size != 0) {
      emit_op(opcode::op_alloc);
      emit_register(state);
      writer_.emit_u64(op.env_size);
      for (const auto &capture : op.captures) {
        if (auto ok = emit_capture(state, capture); !ok.has_value()) {
          return ok;
        }
      }
    } else {
      emit_const(state, slot_value{uint64_t{0}});
    }
    const auto object = alloc_register();
    emit_op(opcode::op_alloc);
    emit_register(object);
    writer_.emit_u64(co::k_slot_count * runtime::k_word_size);
    for (const auto &[slot, value] :
         {std::pair{co::k_step_function_slot, uint64_t{entry->second}},
          std::pair{co::k_resume_index_slot, uint64_t{0}},
          std::pair{co::k_status_slot, co::raw(co::status::suspended)},
          std::pair{co::k_record_slot, uint64_t{0}},
          std::pair{co::k_value_slot, uint64_t{0}},
          std::pair{co::k_value_slot + 1, uint64_t{0}},
          std::pair{co::k_state_bytes_slot, uint64_t{op.env_size}}}) {
      if (auto ok = store_word(object, slot, value); !ok.has_value()) {
        return ok;
      }
    }
    if (auto ok = emit_store(object, word_at(co::k_state_ptr_slot), state,
                             runtime::k_word_size);
        !ok.has_value()) {
      return ok;
    }
    const auto dest = locate(op.dest);
    if (!dest.has_value()) {
      return std::unexpected(dest.error());
    }
    return write_location(*dest, op.dest.type, object);
  }

  /// Runs the producer until it suspends or ends; the status is the result.
  [[nodiscard]] auto emit_generator_resume(const hir::storage_op &op)
      -> checked<void> {
    const auto object = read_place(op.source);
    if (!object.has_value()) {
      return std::unexpected(object.error());
    }
    const auto dest = locate(op.dest);
    if (!dest.has_value()) {
      return std::unexpected(dest.error());
    }
    const auto target = dest->where == location::kind::reg ? dest->reg
                                                          : alloc_register();
    emit_op(opcode::op_coroutine_resume);
    emit_register(target);
    emit_register(*object);
    if (dest->where == location::kind::memory) {
      return write_location(*dest, op.dest.type, target);
    }
    return {};
  }

  /// Moves the item the producer yielded out of its state block.
  [[nodiscard]] auto emit_generator_take(const hir::storage_op &op)
      -> checked<void> {
    namespace co = runtime::coroutine;
    const auto value = class_of(op.dest.type, op.span);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    if (value->cls == pass_class::zero_sized) {
      return {};
    }
    const auto object = read_place(op.source);
    if (!object.has_value()) {
      return std::unexpected(object.error());
    }
    const auto item = alloc_register();
    if (auto ok = emit_load(item, *object, word_at(co::k_value_slot),
                            runtime::k_word_size);
        !ok.has_value()) {
      return ok;
    }
    // A take makes the item the taker's: the coroutine no longer holds one.
    // A shallow read of a `copy` item leaves it there.
    if (!op.shallow) {
      const auto none = alloc_register();
      emit_const(none, slot_value{uint64_t{0}});
      if (auto ok = emit_store(*object, word_at(co::k_value_slot), none,
                               runtime::k_word_size);
          !ok.has_value()) {
        return ok;
      }
    }
    const auto dest = locate(op.dest);
    if (!dest.has_value()) {
      return std::unexpected(dest.error());
    }
    if (!hir::is_indirect(value->cls)) {
      const auto held = read_location(
          location{.where = location::kind::memory, .reg = item},
          op.dest.type, std::nullopt);
      if (!held.has_value()) {
        return std::unexpected(held.error());
      }
      return write_location(*dest, op.dest.type, *held);
    }
    const auto to = address_of(*dest);
    if (!to.has_value()) {
      return std::unexpected(to.error());
    }
    emit_op(opcode::op_copy_bytes);
    emit_register(*to);
    emit_register(item);
    writer_.emit_u32(static_cast<uint32_t>(value->size));
    return {};
  }

  /// First in a step: jumps to the point after the suspension `resume` names.
  /// Zero starts the body.
  [[nodiscard]] auto emit_coroutine_dispatch(const hir::storage_op &op)
      -> checked<void> {
    const auto resume = read_place(op.source);
    if (!resume.has_value()) {
      return std::unexpected(resume.error());
    }
    for (size_t point = 1; point <= op.depth; ++point) {
      const auto wanted = alloc_register();
      emit_const(wanted, slot_value{static_cast<uint64_t>(point)});
      const auto same = alloc_register();
      emit_op(opcode::op_eq);
      emit_register(same);
      emit_register(*resume);
      emit_register(wanted);
      writer_.emit_numeric_kind(bytecode::numeric_kind::u64);
      emit_op(opcode::op_jump_if_true);
      emit_register(same);
      resume_jumps_.push_back(writer_.emit_jump_placeholder());
    }
    return {};
  }

  /// Hands the item to the resumer and returns; the code after this operation
  /// is where the dispatch resumes.
  [[nodiscard]] auto emit_coroutine_suspend(const hir::storage_op &op)
      -> checked<void> {
    namespace co = runtime::coroutine;
    const auto object = read_place(op.dest);
    if (!object.has_value()) {
      return std::unexpected(object.error());
    }
    const auto item = locate(op.source);
    if (!item.has_value()) {
      return std::unexpected(item.error());
    }
    auto address = alloc_register();
    if (item->where == location::kind::memory) {
      const auto found = address_of(*item);
      if (!found.has_value()) {
        return std::unexpected(found.error());
      }
      address = *found;
    } else {
      emit_const(address, slot_value{uint64_t{0}}); // A zero-sized item.
    }
    if (auto ok = emit_store(*object, word_at(co::k_value_slot), address,
                             runtime::k_word_size);
        !ok.has_value()) {
      return ok;
    }
    if (auto ok = store_word(*object, co::k_resume_index_slot, op.depth);
        !ok.has_value()) {
      return ok;
    }
    const auto status = alloc_register();
    emit_const(status, slot_value{co::raw(co::status::suspended)});
    emit_op(opcode::op_return_value);
    emit_register(status);
    if (op.depth == 0 || op.depth > resume_jumps_.size()) {
      return fail(compile_error_kind::internal_error, op.span,
                  "a suspension point the dispatch does not name");
    }
    writer_.patch_jump_to_here(resume_jumps_[op.depth - 1]);
    return {};
  }

  /// A task waits for another. This is its own resume point: the dispatch
  /// lands here, the step polls the awaited task, and while it has not
  /// finished the step returns `pending` so the scheduler runs something else
  /// and wakes it later.
  [[nodiscard]] auto emit_coroutine_await(const hir::storage_op &op)
      -> checked<void> {
    namespace co = runtime::coroutine;
    if (op.depth == 0 || op.depth > resume_jumps_.size()) {
      return fail(compile_error_kind::internal_error, op.span,
                  "a suspension point the dispatch does not name");
    }
    writer_.patch_jump_to_here(resume_jumps_[op.depth - 1]);
    const auto object = read_place(op.dest);
    if (!object.has_value()) {
      return std::unexpected(object.error());
    }
    if (auto ok = store_word(*object, co::k_resume_index_slot, op.depth);
        !ok.has_value()) {
      return ok;
    }
    // One task is polled by its handle. A group is a tuple of handles that
    // the runtime reads in place, so it gets the tuple's address and the
    // number of members.
    const auto name = op.waits == hir::wait_mode::one
                          ? "rt_task_await_poll"
                          : op.waits == hir::wait_mode::crew
                                ? "rt_crew_join_poll"
                          : op.waits == hir::wait_mode::all
                                ? "rt_task_await_all_poll"
                                : "rt_task_await_any_poll";
    const auto id = hir::runtime_intrinsic_id(name);
    if (!id.has_value()) {
      return fail(compile_error_kind::internal_error, op.span,
                  std::string{"the runtime has no `"} + name + "`");
    }
    const auto arity = is_handle_wait(op.waits) ? uint8_t{2} : uint8_t{3};
    const auto block = alloc_group(arity);
    emit_move(block, *object);
    if (auto ok = emit_group_operand(op, block, 1); !ok.has_value()) {
      return ok;
    }
    const auto finished = alloc_register();
    emit_op(opcode::op_call_intrinsic);
    emit_register(finished);
    writer_.emit_u8(static_cast<uint8_t>(*id));
    emit_register(block);
    writer_.emit_u8(arity);
    emit_op(opcode::op_jump_if_true);
    emit_register(finished);
    const auto ready = writer_.emit_jump_placeholder();
    const auto status = alloc_register();
    emit_const(status, slot_value{co::raw(co::status::pending)});
    emit_op(opcode::op_return_value);
    emit_register(status);
    writer_.patch_jump_to_here(ready);
    return {};
  }

  /// A wait on one handle (a task or a crew) rather than on a tuple of tasks.
  [[nodiscard]] static auto is_handle_wait(hir::wait_mode mode) -> bool {
    return mode == hir::wait_mode::one || mode == hir::wait_mode::crew;
  }

  /// Fills the argument registers after `first` for the awaited thing in
  /// `op.source`: the task's handle, or a group's address and member count.
  [[nodiscard]] auto emit_group_operand(const hir::storage_op &op,
                                        virtual_reg block, size_t first)
      -> checked<void> {
    if (is_handle_wait(op.waits)) {
      const auto awaited = read_place(op.source);
      if (!awaited.has_value()) {
        return std::unexpected(awaited.error());
      }
      emit_move(nth(block, first), *awaited);
      return {};
    }
    const auto where = locate(op.source);
    if (!where.has_value()) {
      return std::unexpected(where.error());
    }
    const auto address = address_of(*where);
    if (!address.has_value()) {
      return std::unexpected(address.error());
    }
    emit_move(nth(block, first), *address);
    const auto &group =
        env_.types().entry(env_.types().strip_refinement(op.source.type));
    emit_const(nth(block, first + 1),
               slot_value{static_cast<uint64_t>(group.args.size())});
    return {};
  }

  /// `await par` / `await race` outside a task: the thread blocks.
  [[nodiscard]] auto emit_task_block_on_group(const hir::storage_op &op)
      -> checked<void> {
    const auto name = op.waits == hir::wait_mode::all ? "rt_task_block_on_all"
                                                      : "rt_task_block_on_any";
    const auto id = hir::runtime_intrinsic_id(name);
    if (!id.has_value()) {
      return fail(compile_error_kind::internal_error, op.span,
                  std::string{"the runtime has no `"} + name + "`");
    }
    const auto block = alloc_group(2);
    if (auto ok = emit_group_operand(op, block, 0); !ok.has_value()) {
      return ok;
    }
    emit_op(opcode::op_call_intrinsic);
    emit_register(alloc_register());
    writer_.emit_u8(static_cast<uint8_t>(*id));
    emit_register(block);
    writer_.emit_u8(2);
    return {};
  }

  /// `has_item dest = source`: whether the value word holds an item address.
  [[nodiscard]] auto emit_coroutine_has_item(const hir::storage_op &op)
      -> checked<void> {
    namespace co = runtime::coroutine;
    const auto object = read_place(op.source);
    if (!object.has_value()) {
      return std::unexpected(object.error());
    }
    const auto dest = locate(op.dest);
    if (!dest.has_value()) {
      return std::unexpected(dest.error());
    }
    const auto item = alloc_register();
    if (auto ok = emit_load(item, *object, word_at(co::k_value_slot),
                            runtime::k_word_size);
        !ok.has_value()) {
      return ok;
    }
    const auto none = alloc_register();
    emit_const(none, slot_value{uint64_t{0}});
    const auto target = dest->where == location::kind::reg ? dest->reg
                                                          : alloc_register();
    emit_op(opcode::op_ne);
    emit_register(target);
    emit_register(item);
    emit_register(none);
    writer_.emit_numeric_kind(bytecode::numeric_kind::u64);
    if (dest->where == location::kind::memory) {
      return write_location(*dest, op.dest.type, target);
    }
    return {};
  }

  [[nodiscard]] auto emit_coroutine_cancelled(const hir::storage_op &op)
      -> checked<void> {
    namespace co = runtime::coroutine;
    const auto object = read_place(op.source);
    if (!object.has_value()) {
      return std::unexpected(object.error());
    }
    const auto dest = locate(op.dest);
    if (!dest.has_value()) {
      return std::unexpected(dest.error());
    }
    const auto status = alloc_register();
    if (auto ok = emit_load(status, *object, word_at(co::k_status_slot),
                            runtime::k_word_size);
        !ok.has_value()) {
      return ok;
    }
    const auto wanted = alloc_register();
    emit_const(wanted, slot_value{co::raw(co::status::cancel_requested)});
    const auto target = dest->where == location::kind::reg ? dest->reg
                                                          : alloc_register();
    emit_op(opcode::op_eq);
    emit_register(target);
    emit_register(status);
    emit_register(wanted);
    writer_.emit_numeric_kind(bytecode::numeric_kind::u64);
    if (dest->where == location::kind::memory) {
      return write_location(*dest, op.dest.type, target);
    }
    return {};
  }

  [[nodiscard]] auto emit_coroutine_finish(const hir::storage_op &op)
      -> checked<void> {
    namespace co = runtime::coroutine;
    const auto object = read_place(op.source);
    if (!object.has_value()) {
      return std::unexpected(object.error());
    }
    return store_word(*object, co::k_status_slot, co::raw(co::status::done));
  }

  /// Dropping a generator: one still suspended is resumed once to unwind its
  /// state, then the coroutine is released.
  [[nodiscard]] auto emit_generator_drop(const hir::storage_op &op)
      -> checked<void> {
    const auto object = read_place(op.dest);
    if (!object.has_value()) {
      return std::unexpected(object.error());
    }
    emit_op(opcode::op_coroutine_cancel);
    emit_register(alloc_register());
    emit_register(*object);
    const auto id = hir::runtime_intrinsic_id("rt_task_release");
    if (!id.has_value()) {
      return fail(compile_error_kind::internal_error, op.span,
                  "the runtime has no `rt_task_release`");
    }
    const auto block = alloc_group(1);
    emit_move(block, *object);
    emit_op(opcode::op_call_intrinsic);
    emit_register(alloc_register());
    writer_.emit_u8(static_cast<uint8_t>(*id));
    emit_register(block);
    writer_.emit_u8(1);
    return {};
  }

  /// Frees the environment block this function received.
  [[nodiscard]] auto emit_release_environment(const hir::storage_op &op)
      -> checked<void> {
    const auto id = hir::runtime_intrinsic_id("rt_free");
    if (!id.has_value()) {
      return fail(compile_error_kind::internal_error, op.span,
                  "the runtime has no `rt_free`");
    }
    const auto block = alloc_group(2);
    emit_move(block, virtual_reg{0});
    emit_const(nth(block, 1), slot_value{static_cast<uint64_t>(op.env_size)});
    emit_op(opcode::op_call_intrinsic);
    emit_register(alloc_register());
    writer_.emit_u8(static_cast<uint8_t>(*id));
    emit_register(block);
    writer_.emit_u8(2);
    return {};
  }

  /// An intrinsic the VM runs as one instruction (`cinder::inline_intrinsic`):
  /// an atomic add or subtract of a `usize` at a raw pointer, giving the old
  /// value.
  [[nodiscard]] auto emit_inline_intrinsic(const hir::storage_op &op,
                                           cinder::inline_intrinsic which)
      -> checked<void> {
    if (op.args.size() != 2) {
      return fail(compile_error_kind::internal_error, op.span,
                  "an atomic intrinsic takes a pointer and an amount");
    }
    const auto ptr = operand_value(op.args[0].value);
    if (!ptr.has_value()) {
      return std::unexpected(ptr.error());
    }
    const auto amount = operand_value(op.args[1].value);
    if (!amount.has_value()) {
      return std::unexpected(amount.error());
    }
    const auto dest = locate(op.dest);
    if (!dest.has_value()) {
      return std::unexpected(dest.error());
    }
    const auto result =
        dest->where == location::kind::reg ? dest->reg : alloc_register();
    emit_op(which == cinder::inline_intrinsic::atomic_fetch_add
                ? opcode::op_atomic_fetch_add
                : opcode::op_atomic_fetch_sub);
    emit_register(result);
    emit_register(*ptr);
    emit_register(*amount);
    if (dest->where == location::kind::memory) {
      return write_location(*dest, op.dest.type, result);
    }
    return {};
  }

  /// Calls the runtime's native implementation of intrinsic `id`. The call's
  /// signature is checked against the symbol first (`hir::
  /// check_intrinsic_signature`). A result built in caller storage uses
  /// `op_call_intrinsic_into`, whose first argument register is the
  /// destination address; any other result comes back in a register.
  [[nodiscard]] auto emit_intrinsic_call(const hir::storage_op &op, size_t id)
      -> checked<void> {
    if (const auto problem = hir::check_intrinsic_signature(op.signature, id);
        problem.has_value()) {
      return fail(compile_error_kind::internal_error, op.span, *problem);
    }
    const auto operand_count = op.signature.operands().size();
    const auto block = fill_call_block(op, operand_count);
    if (!block.has_value()) {
      return std::unexpected(block.error());
    }
    if (cinder::intrinsic_builds_into(id)) {
      emit_op(opcode::op_call_intrinsic_into);
      writer_.emit_u8(static_cast<uint8_t>(id));
      emit_register(*block);
      writer_.emit_u8(static_cast<uint8_t>(operand_count));
      return {};
    }
    const auto dest = locate(op.dest);
    if (!dest.has_value()) {
      return std::unexpected(dest.error());
    }
    const auto direct_result =
        op.signature.result.cls != pass_class::zero_sized;
    const auto result =
        direct_result && dest->where == location::kind::reg ? dest->reg
                                                            : alloc_register();
    emit_op(opcode::op_call_intrinsic);
    emit_register(result);
    writer_.emit_u8(static_cast<uint8_t>(id));
    emit_register(*block);
    writer_.emit_u8(static_cast<uint8_t>(operand_count));
    if (direct_result && dest->where == location::kind::memory) {
      return write_location(*dest, op.dest.type, result);
    }
    return {};
  }

  /// Reserves `count` consecutive registers and fills them with the call's
  /// operands in signature order: the destination address when the result is
  /// built in caller storage, then each argument (a scalar or address in its
  /// register, a descriptor or aggregate as the address of the slot holding
  /// it).
  [[nodiscard]] auto fill_call_block(
      const hir::storage_op &op, size_t count,
      std::optional<virtual_reg> environment = std::nullopt)
      -> checked<virtual_reg> {
    if (count > UINT8_MAX) {
      return fail(compile_error_kind::encoding_limit_exceeded, op.span,
                  std::format("a call passes more than {} operands", UINT8_MAX));
    }
    const auto block = count == 0
                           ? alloc_register()
                           : alloc_group(static_cast<uint32_t>(count));
    auto position = size_t{0};
    if (environment.has_value()) {
      emit_move(nth(block, position++), *environment);
    }
    if (op.signature.returns_through_address()) {
      const auto dest = locate(op.dest);
      if (!dest.has_value()) {
        return std::unexpected(dest.error());
      }
      const auto address = address_of(*dest);
      if (!address.has_value()) {
        return std::unexpected(address.error());
      }
      emit_move(nth(block, position++), *address);
    }
    for (const auto &arg : op.args) {
      if (arg.cls == pass_class::zero_sized) {
        continue;
      }
      const auto slot = nth(block, position++);
      if (hir::is_indirect(arg.cls)) {
        const auto where = locate(arg.slot);
        if (!where.has_value()) {
          return std::unexpected(where.error());
        }
        const auto address = address_of(*where);
        if (!address.has_value()) {
          return std::unexpected(address.error());
        }
        emit_move(slot, *address);
      } else {
        const auto value = operand_value(arg.value, slot);
        if (!value.has_value()) {
          return std::unexpected(value.error());
        }
        emit_move(slot, *value);
      }
    }
    return block;
  }

  /// A `str` descriptor `{len, data}` in the frame, with `data` the address
  /// of the literal's bytes in the function's constant table.
  [[nodiscard]] auto emit_string_literal(const hir::storage_op &op)
      -> checked<void> {
    const auto dest = locate(op.dest);
    if (!dest.has_value()) {
      return std::unexpected(dest.error());
    }
    if (dest->where != location::kind::memory) {
      return fail(compile_error_kind::internal_error, op.span,
                  "a `str` descriptor does not live in memory");
    }
    const auto len = alloc_register();
    emit_const(len, slot_value{static_cast<uint64_t>(op.text.size())});
    if (auto ok = emit_store(dest->reg, dest->offset, len, 8); !ok.has_value()) {
      return ok;
    }
    const auto data = alloc_register();
    emit_op(opcode::op_str_const_data);
    emit_register(data);
    writer_.emit_u32(writer_.add_string_constant(op.text));
    return emit_store(dest->reg, dest->offset + 8, data, 8);
  }

  [[nodiscard]] auto emit_return(const hir::storage_op &op) -> checked<void> {
    if (!op.value.has_value()) {
      emit_op(opcode::op_return_unit);
      return {};
    }
    const auto value = operand_value(*op.value);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    emit_op(opcode::op_return_value);
    emit_register(*value);
    return {};
  }

  // ------------------------------------------------------------------
  //  Control flow
  // ------------------------------------------------------------------

  [[nodiscard]] auto emit_if_begin(const hir::storage_op &op) -> checked<void> {
    const auto condition = operand_value(op.condition);
    if (!condition.has_value()) {
      return std::unexpected(condition.error());
    }
    emit_op(opcode::op_jump_if_false);
    emit_register(*condition);
    auto region = open_region{.what = open_region::kind::conditional};
    region.false_jump = writer_.emit_jump_placeholder();
    regions_.push_back(std::move(region));
    return {};
  }

  [[nodiscard]] auto emit_else() -> checked<void> {
    auto &region = regions_.back();
    emit_op(opcode::op_jump);
    region.end_jump = writer_.emit_jump_placeholder();
    writer_.patch_jump_to_here(region.false_jump);
    region.has_else = true;
    return {};
  }

  [[nodiscard]] auto emit_if_end() -> checked<void> {
    const auto &region = regions_.back();
    writer_.patch_jump_to_here(region.has_else ? region.end_jump
                                               : region.false_jump);
    regions_.pop_back();
    return {};
  }

  [[nodiscard]] auto emit_loop_end() -> checked<void> {
    auto region = std::move(regions_.back());
    regions_.pop_back();
    loops_.push_back(loop_range{.header = region.header,
                                .back_edge = writer_.current_offset()});
    emit_jump_back_to(region.header);
    for (const auto exit : region.exits) {
      writer_.patch_jump_to_here(exit);
    }
    return {};
  }

  [[nodiscard]] auto emit_block_end() -> checked<void> {
    const auto region = std::move(regions_.back());
    regions_.pop_back();
    for (const auto exit : region.exits) {
      writer_.patch_jump_to_here(exit);
    }
    return {};
  }

  [[nodiscard]] auto emit_exit(const hir::storage_op &op) -> checked<void> {
    auto remaining = op.depth;
    for (auto it = regions_.rbegin(); it != regions_.rend(); ++it) {
      if (it->what == open_region::kind::conditional) {
        continue;
      }
      if (remaining-- == 0) {
        emit_op(opcode::op_jump);
        it->exits.push_back(writer_.emit_jump_placeholder());
        return {};
      }
    }
    return fail(compile_error_kind::internal_error, op.span,
                "an `exit` names a loop or block that is not open");
  }

  // ------------------------------------------------------------------
  //  Finishing
  // ------------------------------------------------------------------

  [[nodiscard]] auto finish() -> checked<bytecode::bytecode_function> {
    const auto span = fn_.slots.empty() ? source_span{} : fn_.slots.front().span;
    const auto input = allocation_input{
        .virtual_count = static_cast<uint32_t>(next_register_),
        .pinned_prefix = param_count_,
        .instruction_offsets = std::move(instruction_offsets_),
        .sites = std::move(sites_),
        .loops = std::move(loops_),
        .groups = std::move(groups_),
        .address_taken = {},
    };
    const auto allocated = allocate_registers(input);
    if (!allocated.has_value()) {
      return fail(compile_error_kind::encoding_limit_exceeded, span,
                  std::format("`{}` keeps more than {} values alive at the "
                              "same moment, more than one bytecode frame can "
                              "hold; split it into smaller functions",
                              fn_.name, k_physical_register_count));
    }
    auto function = std::move(writer_).finish(key(fn_.name), param_count_,
                                              allocated->register_count);
    for (const auto &site : input.sites) {
      const auto physical = allocated->assignment[site.reg.id];
      function.code[site.code_offset] = static_cast<uint8_t>(physical & 0xFF);
      function.code[site.code_offset + 1] =
          static_cast<uint8_t>((physical >> 8) & 0xFF);
    }
    function.stack_byte_size = static_cast<uint32_t>(frame_bytes_);
    function.source_module = module_.name;
    function.source_function = fn_.name;
    function.spans = std::move(spans_);

    const auto transport = [](const hir::abi_value &value) {
      return bytecode::value_transport{
          .size = static_cast<uint32_t>(value.size),
          .align = static_cast<uint32_t>(value.align),
          .by_address = hir::is_indirect(value.cls)};
    };
    auto abi = bytecode::function_abi{};
    abi.returns_through_address = fn_.signature.returns_through_address();
    abi.result = transport(fn_.signature.result);
    for (const auto &param : fn_.signature.params) {
      abi.params.push_back(transport(param));
    }
    function.abi = std::move(abi);

    if (auto layout = check_operand_layout(function, input.instruction_offsets, span);
        !layout.has_value()) {
      return std::unexpected(layout.error());
    }
    return function;
  }

  /// The operand-layout table in opcodes.h and the code this emitted must
  /// agree on every instruction's size; see the HIR compiler's equivalent.
  [[nodiscard]] static auto
  check_operand_layout(const bytecode::bytecode_function &function,
                       const std::vector<size_t> &recorded,
                       const source_span &span) -> checked<void> {
    auto offset = size_t{0};
    for (size_t index = 0; index < recorded.size(); ++index) {
      if (offset != recorded[index]) {
        return fail(compile_error_kind::internal_error, span,
                    std::format("internal compiler error: in `{}`, "
                                "instruction {} was emitted at byte {} but "
                                "`operands_of` puts it at byte {}",
                                function.name, index, recorded[index], offset));
      }
      offset += bytecode::instruction_size(
          static_cast<opcode>(function.code[offset]));
    }
    if (offset != function.code.size()) {
      return fail(compile_error_kind::internal_error, span,
                  std::format("internal compiler error: in `{}`, the code "
                              "buffer ends at byte {} but its instructions "
                              "end at byte {}",
                              function.name, function.code.size(), offset));
    }
    return {};
  }

  std::string_view entry_;
  const hir::storage_module &module_;
  const hir::storage_function &fn_;
  hir::storage_env &env_;
  /// Each static's offset in the program's static storage.
  const std::unordered_map<std::string, uint32_t> &statics_;
  const std::unordered_map<std::string, uint16_t> &index_;

  bytecode::chunk_writer writer_;
  std::vector<size_t> instruction_offsets_;
  std::vector<register_site> sites_;
  std::vector<loop_range> loops_;
  std::vector<register_group> groups_;
  std::vector<bytecode::span_entry> spans_;
  source_span current_span_{};
  size_t next_register_ = 0;
  uint16_t param_count_ = 0;

  std::vector<home> homes_;
  std::unordered_set<hir::storage_id> taken_;
  size_t frame_bytes_ = 0;
  /// A step only: the jump to each resume point, in order, patched where the
  /// suspension that follows it is emitted.
  std::vector<size_t> resume_jumps_;
  std::optional<uint32_t> anchor_;
  std::vector<open_region> regions_;
};

} // namespace

auto compile_storage_module(std::span<const hir::storage_module *const> modules,
                            hir::storage_env &env)
    -> std::expected<bytecode::bytecode_module, compile_error> {
  if (modules.empty()) {
    return std::unexpected(compile_error{
        .kind = compile_error_kind::internal_error,
        .span = {},
        .message = "a program needs an entry module"});
  }
  const auto &entry = modules.front()->name;
  auto total = size_t{0};
  for (const auto *module : modules) {
    total += module->functions.size();
  }
  if (total > UINT16_MAX) {
    return std::unexpected(compile_error{
        .kind = compile_error_kind::encoding_limit_exceeded,
        .span = {},
        .message = "a program has more functions than a call can name"});
  }
  // One flat table over every module, keyed as `program_function_name` says,
  // so a call anywhere resolves to a fixed index.
  auto index = std::unordered_map<std::string, uint16_t>{};
  auto next = uint16_t{0};
  for (const auto *module : modules) {
    for (const auto &function : module->functions) {
      index.emplace(hir::program_function_name(entry, module->name, function.name),
                    next++);
    }
  }
  auto out = bytecode::bytecode_module{.module_name = entry};
  // Every module's statics share one block, each at its type's alignment.
  auto statics = std::unordered_map<std::string, uint32_t>{};
  auto static_bytes = size_t{0};
  for (const auto *module : modules) {
    for (const auto &global : module->globals) {
      const auto layout = env.layouts.layout(global.type);
      if (!layout.has_value()) {
        return std::unexpected(compile_error{
            .kind = compile_error_kind::unsupported_type,
            .span = global.span,
            .message = layout.error().message});
      }
      const auto align = std::max<size_t>((*layout)->align, 1);
      static_bytes = (static_bytes + align - 1) / align * align;
      if (static_bytes + (*layout)->size > UINT32_MAX) {
        return std::unexpected(compile_error{
            .kind = compile_error_kind::encoding_limit_exceeded,
            .span = global.span,
            .message = "the program's statics need more bytes than an "
                       "operand can address"});
      }
      statics.emplace(global.name, static_cast<uint32_t>(static_bytes));
      static_bytes += (*layout)->size;
    }
    if (!module->globals.empty()) {
      out.static_initializers.push_back(index.at(hir::program_function_name(
          entry, module->name, hir::k_static_init_name)));
    }
  }
  out.global_byte_size = static_bytes;
  out.functions.reserve(total);
  for (const auto *module : modules) {
    for (const auto &function : module->functions) {
      auto compiled =
          function_emitter(entry, *module, function, env, index, statics)
              .compile();
      if (!compiled.has_value()) {
        auto error = compiled.error();
        error.message = std::format("in `{}.{}`: {}", module->name, function.name,
                                    error.message);
        return std::unexpected(std::move(error));
      }
      out.functions.push_back(std::move(*compiled));
    }
  }
  return out;
}

auto compile_storage_module(const hir::storage_module &module,
                            hir::storage_env &env)
    -> std::expected<bytecode::bytecode_module, compile_error> {
  const auto *module_ptr = &module;
  return compile_storage_module(
      std::span<const hir::storage_module *const>(&module_ptr, 1), env);
}

} // namespace cinder::bytecode_compiler
