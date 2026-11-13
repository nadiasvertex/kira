#include "src/llvm_codegen/storage_codegen.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#pragma clang diagnostic ignored "-Wshadow"
#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#pragma clang diagnostic pop

#include "src/bytecode/panic.h"
#include "src/bytecode/value.h"
#include "src/hir/abi.h"
#include "src/hir/intrinsic_abi.h"
#include "src/intrinsics.h"
#include "src/llvm_codegen/scalar_ops.h"
#include "src/runtime/aligned_buffer.h"
#include "src/runtime/coroutine.h"
#include "src/runtime/value_layout.h"

namespace cinder::llvm_codegen {

namespace {

using bytecode::numeric_kind;
using hir::op_kind;
using hir::operand_kind;
using hir::pass_class;
using hir::projection_kind;
using semantic::type_id;

template <typename T> using checked = std::expected<T, codegen_error>;

[[nodiscard]] auto fail(codegen_error_kind kind, const source_span &span,
                        std::string message) -> std::unexpected<codegen_error> {
  return std::unexpected(codegen_error{
      .kind = kind, .span = span, .message = std::move(message)});
}

/// The alignment that `offset` bytes past an `align`-aligned address still
/// has: the lowest set bit of the offset, capped by the base alignment.
[[nodiscard]] auto aligned_at(size_t align, size_t offset) -> size_t {
  if (offset == 0) {
    return align;
  }
  return std::min(align, offset & (~offset + 1));
}

/// The alignment of a closure's environment block: the allocator hands back
/// 8-byte-aligned blocks (`src/runtime/allocator.h`).
constexpr size_t k_environment_align = 8;

/// Where a slot's bytes are while the function runs.
struct home {
  llvm::Value *ptr = nullptr;
  /// The alignment of `ptr` that the function may rely on.
  size_t align = 1;
};

/// Where a place's bytes are, once its projections are followed.
struct location {
  llvm::Value *base = nullptr;
  size_t offset = 0;
  /// The alignment of `base`, not of `base + offset`.
  size_t align = 1;
};

/// A structured region that is open while emitting.
struct open_region {
  enum class kind : uint8_t { conditional, loop, block };
  kind what = kind::block;
  /// `conditional`: the blocks of the two sides and the join.
  llvm::BasicBlock *else_block = nullptr;
  bool has_else = false;
  /// `loop`: where the back edge returns to.
  llvm::BasicBlock *header = nullptr;
  /// `conditional`, `loop`, `block`: where control continues after the region.
  llvm::BasicBlock *after = nullptr;
};

/// What every function in one module compiles against.
struct module_context {
  llvm::LLVMContext &ctx;
  llvm::Module &module;
  hir::storage_env &env;
  llvm::Function *panic_fn;
  /// The program's entry module: its functions are named plainly, any other
  /// module's `module::name` (`hir::program_function_name`).
  std::string entry;
  /// Every function of the program by its program-wide name.
  std::unordered_map<std::string, llvm::Function *> functions;
  /// Every module's statics by name: their bytes, and the alignment the
  /// bytes were given.
  struct static_storage {
    llvm::GlobalVariable *bytes = nullptr;
    size_t align = 1;
  };
  std::unordered_map<std::string, static_storage> statics;
};

[[nodiscard]] auto layout_of(module_context &mc, type_id type,
                             const source_span &span)
    -> checked<const runtime::value_layout *> {
  const auto found = mc.env.layouts.layout(type);
  if (!found.has_value()) {
    return fail(codegen_error_kind::unsupported_type, span,
                found.error().message);
  }
  return *found;
}

[[nodiscard]] auto class_of(module_context &mc, type_id type,
                            const source_span &span) -> checked<hir::abi_value> {
  const auto found = hir::classify_value(mc.env.layouts, type);
  if (!found.has_value()) {
    return fail(codegen_error_kind::unsupported_type, span,
                found.error().message);
  }
  return *found;
}

[[nodiscard]] auto kind_of(module_context &mc, type_id type,
                           const source_span &span) -> checked<numeric_kind> {
  const auto &types = mc.env.types();
  const auto kind =
      bytecode::numeric_kind_of(types, types.strip_refinement(type));
  if (!kind.has_value() || bytecode::is_wide_integer(*kind)) {
    return fail(codegen_error_kind::unsupported_type, span,
                std::format("type `{}` has no numeric llvm_codegen "
                            "representation yet — 128-bit integer and float "
                            "types are not supported yet",
                            types.display(type)));
  }
  return *kind;
}

/// The type a scalar-class value has while it is computed with: `i1` for a
/// `bool`, `ptr` for an address or handle, the numeric type otherwise.
[[nodiscard]] auto value_type(module_context &mc, type_id type,
                              const source_span &span) -> checked<llvm::Type *> {
  const auto value = class_of(mc, type, span);
  if (!value.has_value()) {
    return std::unexpected(value.error());
  }
  if (value->cls == pass_class::address || value->cls == pass_class::handle) {
    return llvm::PointerType::get(mc.ctx, 0);
  }
  if (value->cls != pass_class::scalar) {
    return fail(codegen_error_kind::unsupported_type, span,
                std::format("`{}` is not a scalar value", mc.env.types().display(type)));
  }
  const auto kind = kind_of(mc, type, span);
  if (!kind.has_value()) {
    return std::unexpected(kind.error());
  }
  return llvm_type_for(mc.ctx, *kind);
}

/// The type a scalar-class value has in memory: a `bool` is a byte.
[[nodiscard]] auto memory_type(module_context &mc, type_id type,
                               const source_span &span) -> checked<llvm::Type *> {
  const auto ty = value_type(mc, type, span);
  if (!ty.has_value()) {
    return ty;
  }
  if ((*ty)->isIntegerTy(1)) {
    return llvm::Type::getInt8Ty(mc.ctx);
  }
  return ty;
}

/// The LLVM function type of an internal signature.
[[nodiscard]] auto function_type_for(module_context &mc,
                                     const hir::abi_signature &signature,
                                     const source_span &span)
    -> checked<llvm::FunctionType *> {
  auto params = std::vector<llvm::Type *>{};
  for (const auto &operand : signature.operands()) {
    if (operand.role == hir::abi_operand_role::result_address ||
        operand.role == hir::abi_operand_role::environment ||
        operand.by_address) {
      params.push_back(llvm::PointerType::get(mc.ctx, 0));
      continue;
    }
    const auto ty = value_type(mc, operand.value.type, span);
    if (!ty.has_value()) {
      return std::unexpected(ty.error());
    }
    params.push_back(*ty);
  }
  auto *result = static_cast<llvm::Type *>(llvm::Type::getVoidTy(mc.ctx));
  if (!signature.returns_through_address() &&
      signature.result.cls != pass_class::zero_sized) {
    const auto ty = value_type(mc, signature.result.type, span);
    if (!ty.has_value()) {
      return std::unexpected(ty.error());
    }
    result = *ty;
  }
  return llvm::FunctionType::get(result, params, /*isVarArg=*/false);
}

/// A sub-32-bit integer result carries `signext`/`zeroext`, as clang emits
/// for C: Apple arm64 makes the callee extend such a value, so a native
/// caller may read the whole register.
[[nodiscard]] auto result_extension(module_context &mc, llvm::Type *ty,
                                    type_id result)
    -> std::optional<llvm::Attribute::AttrKind> {
  if (!ty->isIntegerTy() || ty->getIntegerBitWidth() >= 32) {
    return std::nullopt;
  }
  const auto &types = mc.env.types();
  const auto kind = bytecode::numeric_kind_of(types, types.strip_refinement(result));
  return kind.has_value() && bytecode::is_signed_integer(*kind)
             ? llvm::Attribute::SExt
             : llvm::Attribute::ZExt;
}

auto add_result_extension(module_context &mc, llvm::Function &function,
                          type_id result) -> void {
  if (const auto extension = result_extension(mc, function.getReturnType(), result);
      extension.has_value()) {
    function.addRetAttr(*extension);
  }
}

/// A direct call that carries the callee's return attributes, so the call
/// site agrees with the callee.
[[nodiscard]] auto call_function(llvm::IRBuilder<> &builder,
                                 llvm::Function *callee,
                                 llvm::ArrayRef<llvm::Value *> args)
    -> llvm::CallInst * {
  auto *call = builder.CreateCall(callee, args);
  const auto ret_attrs = callee->getAttributes().getRetAttrs();
  if (ret_attrs.hasAttributes()) {
    call->addRetAttrs(llvm::AttrBuilder(callee->getContext(), ret_attrs));
  }
  return call;
}

/// The LLVM type a runtime symbol declares for a parameter or result of
/// `kind` (`src/intrinsics.h`).
[[nodiscard]] auto wire_type(llvm::LLVMContext &ctx, cinder::intrinsic_wire_kind kind)
    -> llvm::Type * {
  switch (kind) {
  case cinder::intrinsic_wire_kind::i32:
    return llvm::Type::getInt32Ty(ctx);
  case cinder::intrinsic_wire_kind::i64:
    return llvm::Type::getInt64Ty(ctx);
  case cinder::intrinsic_wire_kind::f32:
    return llvm::Type::getFloatTy(ctx);
  case cinder::intrinsic_wire_kind::f64:
    return llvm::Type::getDoubleTy(ctx);
  case cinder::intrinsic_wire_kind::ptr:
    break;
  }
  return llvm::PointerType::get(ctx, 0);
}

/// The declaration of the runtime symbol for intrinsic `id`, added to the
/// module the first time a call needs it. An intrinsic that builds its result
/// in caller storage is called through its `_into` symbol, which takes the
/// destination first and returns nothing; every other one returns its result
/// at its wire kind.
[[nodiscard]] auto runtime_function(module_context &mc, size_t id)
    -> llvm::Function * {
  const auto name = cinder::known_intrinsic_names.at(id);
  const auto into = cinder::intrinsic_builds_into(id);
  const auto symbol =
      into ? cinder::intrinsic_into_symbol(name) : "cinder_" + std::string(name);
  if (auto *existing = mc.module.getFunction(symbol); existing != nullptr) {
    return existing;
  }
  auto params = std::vector<llvm::Type *>{};
  if (into) {
    params.push_back(llvm::PointerType::get(mc.ctx, 0));
  }
  for (size_t p = 0; p < cinder::known_intrinsic_arities.at(id); ++p) {
    params.push_back(wire_type(mc.ctx, cinder::known_intrinsic_param_kinds.at(id).at(p)));
  }
  auto *result = into ? llvm::Type::getVoidTy(mc.ctx)
                      : wire_type(mc.ctx, cinder::known_intrinsic_return_kinds.at(id));
  return llvm::Function::Create(llvm::FunctionType::get(result, params, false),
                                llvm::Function::ExternalLinkage, symbol, mc.module);
}

class function_emitter {
public:
  function_emitter(module_context &mc, const hir::storage_module &module,
                   const hir::storage_function &function,
                   llvm::Function *llvm_function)
      : mc_(mc), module_(module), fn_(function), llvm_fn_(llvm_function),
        builder_(mc.ctx), scalars_(builder_, mc.panic_fn) {}

  /// The program-wide name of a function of this module.
  [[nodiscard]] auto key(std::string_view name) const -> std::string {
    return hir::program_function_name(mc_.entry, module_.name, name);
  }

  /// The program-wide name a direct call names.
  [[nodiscard]] auto key(const hir::call_target &callee) const -> std::string {
    return hir::program_callee_name(mc_.entry, module_.name, callee);
  }

  [[nodiscard]] auto compile() -> checked<void> {
    entry_ = llvm::BasicBlock::Create(mc_.ctx, "entry", llvm_fn_);
    builder_.SetInsertPoint(entry_);
    if (auto ok = prepare(); !ok.has_value()) {
      return ok;
    }
    for (const auto &op : fn_.ops) {
      current_span_ = op.span;
      if (auto ok = emit(op); !ok.has_value()) {
        return ok;
      }
    }
    finish();
    return {};
  }

private:
  // ------------------------------------------------------------------
  //  Types and small helpers
  // ------------------------------------------------------------------

  [[nodiscard]] auto types() const -> const semantic::type_table & {
    return mc_.env.types();
  }

  [[nodiscard]] auto i8() -> llvm::Type * { return llvm::Type::getInt8Ty(mc_.ctx); }
  [[nodiscard]] auto i64() -> llvm::Type * { return llvm::Type::getInt64Ty(mc_.ctx); }
  [[nodiscard]] auto ptr_type() -> llvm::Type * {
    return llvm::PointerType::get(mc_.ctx, 0);
  }

  [[nodiscard]] auto constant(llvm::Type *ty, uint64_t value) -> llvm::Constant * {
    return llvm::ConstantInt::get(ty, value);
  }

  [[nodiscard]] auto terminated() const -> bool {
    return builder_.GetInsertBlock()->hasTerminator();
  }

  /// Code after a `return` or `exit` in the same region is unreachable. It
  /// still has to live in a block, so it gets one that nothing branches to.
  auto start_dead_block() -> void {
    auto *dead = llvm::BasicBlock::Create(mc_.ctx, "dead", llvm_fn_);
    builder_.SetInsertPoint(dead);
  }

  /// Ends the current block with a branch to `target` unless it already ended.
  auto branch_to(llvm::BasicBlock *target) -> void {
    if (!terminated()) {
      builder_.CreateBr(target);
    }
  }

  // ------------------------------------------------------------------
  //  Preparation: where every slot lives
  // ------------------------------------------------------------------

  /// Entry-block storage of `size` bytes aligned to `align`. An `alloca` in
  /// the entry block is a fixed frame slot however often the code around it
  /// runs.
  [[nodiscard]] auto frame_bytes(size_t size, size_t align) -> llvm::Value * {
    auto at_entry = llvm::IRBuilder<>(entry_, entry_->begin());
    auto *storage = at_entry.CreateAlloca(
        llvm::ArrayType::get(i8(), std::max<size_t>(size, 1)));
    storage->setAlignment(llvm::Align(std::max<size_t>(align, 1)));
    return storage;
  }

  /// A small aligned region that gives a zero-sized value an address to be
  /// borrowed at; nothing is ever read or written through it.
  [[nodiscard]] auto anchor() -> home {
    if (anchor_ == nullptr) {
      anchor_ = frame_bytes(runtime::k_frame_storage_alignment,
                            runtime::k_frame_storage_alignment);
    }
    return home{.ptr = anchor_, .align = runtime::k_frame_storage_alignment};
  }

  [[nodiscard]] auto prepare() -> checked<void> {
    const auto operands = fn_.signature.operands();
    if (operands.size() != llvm_fn_->arg_size()) {
      return fail(codegen_error_kind::unsupported_construct, current_span_,
                  std::format("`{}` has {} operands but its declaration has {}",
                              fn_.name, operands.size(), llvm_fn_->arg_size()));
    }
    auto operand_of_slot =
        std::vector<std::optional<size_t>>(fn_.slots.size(), std::nullopt);
    for (size_t i = 0; i < operands.size(); ++i) {
      if (operands[i].role == hir::abi_operand_role::environment) {
        environment_ = llvm_fn_->getArg(static_cast<unsigned>(i));
      } else if (operands[i].role == hir::abi_operand_role::result_address) {
        if (fn_.result_slot != hir::k_invalid_storage) {
          operand_of_slot[fn_.result_slot] = i;
        }
      } else if (operands[i].source_index < fn_.param_slots.size()) {
        operand_of_slot[fn_.param_slots[operands[i].source_index]] = i;
      }
    }

    homes_.assign(fn_.slots.size(), home{});
    for (const auto &slot : fn_.slots) {
      const auto value = class_of(mc_, slot.type, slot.span);
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
      auto &here = homes_[slot.id];
      const auto zero = value->cls == pass_class::zero_sized;
      switch (slot.cls) {
      case hir::storage_class::caller_result:
        if (!operand_of_slot[slot.id].has_value()) {
          return fail(codegen_error_kind::unsupported_construct, slot.span,
                      "the result slot has no hidden result operand");
        }
        here = home{.ptr = llvm_fn_->getArg(
                        static_cast<unsigned>(*operand_of_slot[slot.id])),
                    .align = fn_.signature.result.align};
        break;
      case hir::storage_class::parameter: {
        if (zero) {
          here = anchor();
          break;
        }
        if (!operand_of_slot[slot.id].has_value()) {
          return fail(codegen_error_kind::unsupported_construct, slot.span,
                      "a parameter slot has no operand");
        }
        auto *incoming = llvm_fn_->getArg(
            static_cast<unsigned>(*operand_of_slot[slot.id]));
        if (hir::is_indirect(value->cls)) {
          here = home{.ptr = incoming, .align = value->align};
          break;
        }
        here = home{.ptr = frame_bytes(value->size, value->align),
                    .align = value->align};
        if (auto ok = store_scalar(location{.base = here.ptr, .align = here.align},
                                   slot.type, incoming);
            !ok.has_value()) {
          return ok;
        }
        break;
      }
      case hir::storage_class::frame_local:
      case hir::storage_class::outgoing_argument:
        here = zero ? anchor()
                    : home{.ptr = frame_bytes(value->size, value->align),
                           .align = value->align};
        break;
      case hir::storage_class::coroutine_frame: {
        // A generator's locals are bytes of the state block the step receives.
        if (zero) {
          here = anchor();
          break;
        }
        if (environment_ == nullptr) {
          return fail(codegen_error_kind::unsupported_construct, slot.span,
                      "a coroutine slot in a function without a state block");
        }
        here = home{.ptr = slot.env_offset == 0
                               ? environment_
                               : builder_.CreateInBoundsGEP(
                                     i8(), environment_,
                                     {constant(i64(), slot.env_offset)}),
                    .align = aligned_at(k_environment_align, slot.env_offset)};
        break;
      }
      case hir::storage_class::closure_environment: {
        if (environment_ == nullptr) {
          return fail(codegen_error_kind::unsupported_construct, slot.span,
                      "an environment slot in a function without an "
                      "environment operand");
        }
        auto *at = slot.env_offset == 0
                       ? environment_
                       : builder_.CreateInBoundsGEP(
                             i8(), environment_,
                             {constant(i64(), slot.env_offset)});
        if (slot.env_indirect) {
          // Captured by reference: the environment holds the variable's
          // address, and what is reached through it has unknown alignment.
          here = home{.ptr = builder_.CreateAlignedLoad(
                          ptr_type(), at,
                          access_align(location{.base = environment_,
                                                .offset = slot.env_offset,
                                                .align = k_environment_align},
                                       runtime::k_word_size)),
                      .align = 1};
        } else {
          here = home{.ptr = at,
                      .align = aligned_at(k_environment_align, slot.env_offset)};
        }
        break;
      }
      case hir::storage_class::global: {
        // A static is its own LLVM global, which every function reaches.
        const auto found = mc_.statics.find(slot.name);
        if (found == mc_.statics.end()) {
          return fail(codegen_error_kind::unsupported_construct, slot.span,
                      std::format("no module declares the static `{}`",
                                  slot.name));
        }
        here = home{.ptr = found->second.bytes, .align = found->second.align};
        break;
      }
      default:
        return fail(codegen_error_kind::unsupported_construct, slot.span,
                    std::format("a `{}` slot is not placed by llvm_codegen yet",
                                hir::storage_class_name(slot.cls)));
      }
    }
    return {};
  }

  // ------------------------------------------------------------------
  //  Places
  // ------------------------------------------------------------------

  /// The alignment an access of `natural` bytes at `loc` may claim.
  [[nodiscard]] static auto access_align(const location &loc, size_t natural)
      -> llvm::Align {
    return llvm::Align(
        std::max<size_t>(1, std::min(natural, aligned_at(loc.align, loc.offset))));
  }

  [[nodiscard]] auto pointer_at(const location &loc) -> llvm::Value * {
    if (loc.offset == 0) {
      return loc.base;
    }
    return builder_.CreateInBoundsGEP(i8(), loc.base,
                                      {constant(i64(), loc.offset)});
  }

  /// Follows `where`'s projections from its slot's home. A dereference loads
  /// the address the value holds; what is reached through it is accessed with
  /// alignment 1, because the referent's alignment is not inferred from its
  /// type.
  [[nodiscard]] auto locate(const hir::place &where) -> checked<location> {
    const auto &slot = fn_.slots[where.base];
    const auto &here = homes_[where.base];
    auto loc = location{.base = here.ptr, .offset = 0, .align = here.align};
    auto current = slot.type;
    for (const auto &step : where.steps) {
      if (step.kind == projection_kind::deref) {
        auto *pointer = builder_.CreateAlignedLoad(
            ptr_type(), pointer_at(loc), access_align(loc, runtime::k_word_size));
        loc = location{.base = pointer, .offset = 0, .align = 1};
        current = types().entry(types().strip_refinement(current)).result;
        continue;
      }
      const auto layout = layout_of(mc_, current, slot.span);
      if (!layout.has_value()) {
        return std::unexpected(layout.error());
      }
      const auto &found = **layout;
      switch (step.kind) {
      case projection_kind::member:
        loc.offset += found.members[step.index].offset;
        current = found.members[step.index].type;
        break;
      case projection_kind::element:
        loc.offset += found.stride * step.index;
        current = found.element;
        break;
      case projection_kind::payload: {
        const auto *variant = found.find_variant(step.name);
        loc.offset += variant->payload[step.index].offset;
        current = variant->payload[step.index].type;
        break;
      }
      case projection_kind::length:
        loc.offset += runtime::k_descriptor_len_offset;
        current = step.type;
        break;
      case projection_kind::data:
        loc.offset += runtime::k_descriptor_data_offset;
        current = step.type;
        break;
      case projection_kind::deref:
        break;
      }
    }
    return loc;
  }

  /// The scalar-class value stored at `loc`.
  [[nodiscard]] auto load_scalar(const location &loc, type_id type)
      -> checked<llvm::Value *> {
    const auto stored = memory_type(mc_, type, current_span_);
    if (!stored.has_value()) {
      return std::unexpected(stored.error());
    }
    const auto size = mc_.env.layouts.size_of(type);
    if (!size.has_value()) {
      return fail(codegen_error_kind::unsupported_type, current_span_,
                  size.error().message);
    }
    auto *loaded = builder_.CreateAlignedLoad(*stored, pointer_at(loc),
                                              access_align(loc, *size));
    const auto computed = value_type(mc_, type, current_span_);
    if (!computed.has_value()) {
      return std::unexpected(computed.error());
    }
    if ((*computed)->isIntegerTy(1)) {
      return builder_.CreateICmpNE(loaded, constant(i8(), 0));
    }
    return loaded;
  }

  [[nodiscard]] auto store_scalar(const location &loc, type_id type,
                                  llvm::Value *value) -> checked<void> {
    const auto size = mc_.env.layouts.size_of(type);
    if (!size.has_value()) {
      return fail(codegen_error_kind::unsupported_type, current_span_,
                  size.error().message);
    }
    if (value->getType()->isIntegerTy(1)) {
      value = builder_.CreateZExt(value, i8());
    }
    builder_.CreateAlignedStore(value, pointer_at(loc), access_align(loc, *size));
    return {};
  }

  // ------------------------------------------------------------------
  //  Operands
  // ------------------------------------------------------------------

  [[nodiscard]] auto operand_value(const hir::operand &value)
      -> checked<llvm::Value *> {
    switch (value.kind) {
    case operand_kind::constant: {
      if (value.literal == token_kind::kw_unit) {
        return constant(i64(), 0);
      }
      const auto kind = kind_of(mc_, value.type, current_span_);
      if (!kind.has_value()) {
        return std::unexpected(kind.error());
      }
      return scalars_.literal(value.literal, value.text, current_span_, *kind);
    }
    case operand_kind::read: {
      const auto loc = locate(value.where);
      if (!loc.has_value()) {
        return std::unexpected(loc.error());
      }
      const auto cls = class_of(mc_, value.where.type, current_span_);
      if (!cls.has_value()) {
        return std::unexpected(cls.error());
      }
      if (cls->cls == pass_class::zero_sized) {
        return constant(i64(), 0);
      }
      return load_scalar(*loc, value.where.type);
    }
    case operand_kind::address_of: {
      const auto loc = locate(value.where);
      if (!loc.has_value()) {
        return std::unexpected(loc.error());
      }
      return pointer_at(*loc);
    }
    }
    return fail(codegen_error_kind::unsupported_construct, current_span_,
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
      builder_.CreateUnreachable();
      start_dead_block();
      return {};
    case op_kind::string_literal:
      return emit_string_literal(op);
    case op_kind::if_begin:
      return emit_if_begin(op);
    case op_kind::else_begin:
      return emit_else();
    case op_kind::if_end:
      return emit_if_end();
    case op_kind::loop_begin:
      return emit_loop_begin();
    case op_kind::loop_end:
      return emit_loop_end();
    case op_kind::block_begin:
      return emit_block_begin();
    case op_kind::block_end:
      return emit_block_end();
    case op_kind::exit:
      return emit_exit(op);
    }
    return fail(codegen_error_kind::unsupported_construct, op.span,
                "an operation of an unknown kind");
  }

  [[nodiscard]] auto emit_compute(const hir::storage_op &op) -> checked<void> {
    const auto dest_class = class_of(mc_, op.dest.type, op.span);
    if (!dest_class.has_value()) {
      return std::unexpected(dest_class.error());
    }
    if (dest_class->cls == pass_class::zero_sized) {
      return {};
    }
    auto computed = checked<llvm::Value *>{nullptr};
    switch (op.computation) {
    case hir::compute_kind::constant: {
      const auto literal = hir::operand{.kind = operand_kind::constant,
                                        .type = op.dest.type,
                                        .literal = op.literal,
                                        .text = op.text};
      computed = operand_value(literal);
      break;
    }
    case hir::compute_kind::copy:
      computed = operand_value(op.operands[0]);
      break;
    case hir::compute_kind::binary:
      computed = emit_binary(op);
      break;
    case hir::compute_kind::unary:
      computed = emit_unary(op);
      break;
    case hir::compute_kind::cast:
      computed = emit_cast(op);
      break;
    case hir::compute_kind::pointer_offset:
      computed = emit_pointer_offset(op);
      break;
    case hir::compute_kind::checked_index:
    case hir::compute_kind::checked_end:
      computed = emit_bounds_check(op);
      break;
    }
    if (!computed.has_value()) {
      return std::unexpected(computed.error());
    }
    const auto dest = locate(op.dest);
    if (!dest.has_value()) {
      return std::unexpected(dest.error());
    }
    return store_scalar(*dest, op.dest.type, *computed);
  }

  [[nodiscard]] auto emit_binary(const hir::storage_op &op)
      -> checked<llvm::Value *> {
    const auto kind = kind_of(mc_, op.operands[0].type, op.span);
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
    return scalars_.binary(op.binary, *kind, *lhs, *rhs, op.span);
  }

  [[nodiscard]] auto emit_unary(const hir::storage_op &op)
      -> checked<llvm::Value *> {
    const auto &value = op.operands[0];
    if (op.unary == ast::unary_op::logical_not) {
      const auto src = operand_value(value);
      if (!src.has_value()) {
        return std::unexpected(src.error());
      }
      return builder_.CreateNot(*src);
    }
    if (op.unary != ast::unary_op::neg && op.unary != ast::unary_op::bit_not) {
      return fail(codegen_error_kind::unsupported_construct, op.span,
                  std::format("operator `{}` needs a reference model "
                              "llvm_codegen does not have yet",
                              ast::unary_op_name(op.unary)));
    }
    const auto kind = kind_of(mc_, value.type, op.span);
    if (!kind.has_value()) {
      return std::unexpected(kind.error());
    }
    if (op.unary == ast::unary_op::neg && value.kind == operand_kind::constant &&
        value.literal == token_kind::int_lit) {
      // `-128` as `int8`: the literal's magnitude alone does not fit the
      // type, so the negation is folded here instead of trapping at run time.
      return scalars_.negated_literal(value.text, op.span, *kind);
    }
    const auto src = operand_value(value);
    if (!src.has_value()) {
      return std::unexpected(src.error());
    }
    if (op.unary == ast::unary_op::bit_not) {
      return builder_.CreateNot(*src);
    }
    return scalars_.negate(*kind, *src);
  }

  [[nodiscard]] auto emit_cast(const hir::storage_op &op)
      -> checked<llvm::Value *> {
    const auto from = kind_of(mc_, op.operands[0].type, op.span);
    if (!from.has_value()) {
      return std::unexpected(from.error());
    }
    const auto to = kind_of(mc_, op.dest.type, op.span);
    if (!to.has_value()) {
      return std::unexpected(to.error());
    }
    const auto src = operand_value(op.operands[0]);
    if (!src.has_value()) {
      return std::unexpected(src.error());
    }
    return scalars_.cast(*from, *to, *src);
  }

  /// `p + i * stride` bytes. The index is widened to 64 bits by its own
  /// type; the arithmetic wraps, because the machine layer owns the bounds.
  [[nodiscard]] auto emit_pointer_offset(const hir::storage_op &op)
      -> checked<llvm::Value *> {
    const auto base = operand_value(op.operands[0]);
    if (!base.has_value()) {
      return std::unexpected(base.error());
    }
    const auto kind = kind_of(mc_, op.operands[1].type, op.span);
    if (!kind.has_value()) {
      return std::unexpected(kind.error());
    }
    auto count = operand_value(op.operands[1]);
    if (!count.has_value()) {
      return std::unexpected(count.error());
    }
    auto *index = *count;
    if (*kind != numeric_kind::u64) {
      index = scalars_.cast(*kind, numeric_kind::u64, index);
    }
    auto *bytes = builder_.CreateMul(index, constant(i64(), op.stride));
    return builder_.CreateGEP(i8(), *base, {bytes});
  }

  /// `checked_index` panics when the value is at or past the limit,
  /// `checked_end` when it is past it; otherwise the value is the result.
  [[nodiscard]] auto emit_bounds_check(const hir::storage_op &op)
      -> checked<llvm::Value *> {
    const auto value = operand_value(op.operands[0]);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    const auto limit = operand_value(op.operands[1]);
    if (!limit.has_value()) {
      return std::unexpected(limit.error());
    }
    auto *outside = op.computation == hir::compute_kind::checked_index
                        ? builder_.CreateICmpUGE(*value, *limit)
                        : builder_.CreateICmpUGT(*value, *limit);
    scalars_.guard_panic(outside, bytecode::panic_reason::index_out_of_bounds);
    return *value;
  }

  [[nodiscard]] auto emit_transfer(const hir::storage_op &op) -> checked<void> {
    const auto value = class_of(mc_, op.dest.type, op.span);
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
      const auto held = load_scalar(*from, op.source.type);
      if (!held.has_value()) {
        return std::unexpected(held.error());
      }
      return store_scalar(*to, op.dest.type, *held);
    }
    // The two places may overlap, so this is a move rather than a copy.
    builder_.CreateMemMove(pointer_at(*to), access_align(*to, value->align),
                           pointer_at(*from), access_align(*from, value->align),
                           constant(i64(), value->size));
    return {};
  }

  /// Copies element 0 over the rest of the array by doubling the written
  /// prefix: about log2(count) copies, each of a constant size and offset.
  [[nodiscard]] auto emit_zero_fill(const hir::storage_op &op) -> checked<void> {
    const auto layout = layout_of(mc_, op.dest.type, op.span);
    if (!layout.has_value()) {
      return std::unexpected(layout.error());
    }
    if ((*layout)->size == 0) {
      return {};
    }
    const auto to = locate(op.dest);
    if (!to.has_value()) {
      return std::unexpected(to.error());
    }
    builder_.CreateMemSet(pointer_at(*to),
                          llvm::ConstantInt::get(i8(), 0),
                          constant(i64(), (*layout)->size),
                          access_align(*to, (*layout)->align));
    return {};
  }

  [[nodiscard]] auto emit_replicate(const hir::storage_op &op) -> checked<void> {
    const auto layout = layout_of(mc_, op.dest.type, op.span);
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
    const auto align = (*layout)->align;
    for (auto filled = size_t{1}; filled < count;) {
      const auto chunk = std::min(filled, count - filled);
      auto next = *to;
      next.offset += filled * stride;
      builder_.CreateMemCpy(pointer_at(next), access_align(next, align),
                            pointer_at(*to), access_align(*to, align),
                            constant(i64(), chunk * stride));
      filled += chunk;
    }
    return {};
  }

  [[nodiscard]] auto emit_publish(const hir::storage_op &op) -> checked<void> {
    const auto layout = layout_of(mc_, op.dest.type, op.span);
    if (!layout.has_value()) {
      return std::unexpected(layout.error());
    }
    const auto *variant = (*layout)->find_variant(op.variant);
    if (variant == nullptr) {
      return fail(codegen_error_kind::unsupported_construct, op.span,
                  std::format("`{}` has no variant `{}`",
                              types().display(op.dest.type), op.variant));
    }
    auto dest = locate(op.dest);
    if (!dest.has_value()) {
      return std::unexpected(dest.error());
    }
    dest->offset += (*layout)->tag_offset;
    builder_.CreateAlignedStore(
        constant(i64(), static_cast<uint64_t>(variant->tag)), pointer_at(*dest),
        access_align(*dest, runtime::k_sum_tag_size));
    return {};
  }

  /// Compares the tag of the sum at `source` with the tag of `variant`.
  [[nodiscard]] auto emit_test_variant(const hir::storage_op &op)
      -> checked<void> {
    const auto layout = layout_of(mc_, op.source.type, op.span);
    if (!layout.has_value()) {
      return std::unexpected(layout.error());
    }
    const auto *variant = (*layout)->find_variant(op.variant);
    if (variant == nullptr) {
      return fail(codegen_error_kind::unsupported_construct, op.span,
                  std::format("`{}` has no variant `{}`",
                              types().display(op.source.type), op.variant));
    }
    auto source = locate(op.source);
    if (!source.has_value()) {
      return std::unexpected(source.error());
    }
    const auto dest = locate(op.dest);
    if (!dest.has_value()) {
      return std::unexpected(dest.error());
    }
    source->offset += (*layout)->tag_offset;
    auto *tag = builder_.CreateAlignedLoad(
        i64(), pointer_at(*source),
        access_align(*source, runtime::k_sum_tag_size));
    auto *same = builder_.CreateICmpEQ(
        tag, constant(i64(), static_cast<uint64_t>(variant->tag)));
    return store_scalar(*dest, op.dest.type, same);
  }

  [[nodiscard]] auto emit_call(const hir::storage_op &op) -> checked<void> {
    if (!op.callee.is_direct()) {
      return emit_call_function(op);
    }
    const auto target = key(op.callee);
    if (!mc_.functions.contains(target)) {
      if (const auto which = cinder::inline_intrinsic_of(op.callee.name);
          which.has_value()) {
        return emit_inline_intrinsic(op, *which);
      }
      if (const auto id = hir::runtime_intrinsic_id(op.callee.name);
          id.has_value()) {
        return emit_intrinsic_call(op, *id);
      }
    }
    const auto found = mc_.functions.find(target);
    if (found == mc_.functions.end()) {
      return fail(codegen_error_kind::unknown_callee, op.span,
                  std::format("`{}` is not a function of this program", target));
    }
    auto *callee = found->second;
    const auto dest = locate(op.dest);
    if (!dest.has_value()) {
      return std::unexpected(dest.error());
    }
    auto args = std::vector<llvm::Value *>{};
    if (auto ok = call_arguments(op, *dest, args); !ok.has_value()) {
      return ok;
    }
    if (args.size() != callee->arg_size()) {
      return fail(codegen_error_kind::unsupported_construct, op.span,
                  std::format("a call to `{}` passes {} operands but the "
                              "function takes {}",
                              op.callee.name, args.size(), callee->arg_size()));
    }
    auto *call = call_function(builder_, callee, args);
    const auto direct_result = !op.signature.returns_through_address() &&
                               op.signature.result.cls != pass_class::zero_sized;
    if (direct_result) {
      return store_scalar(*dest, op.dest.type, call);
    }
    return {};
  }

  /// The operands of a call, in the order its signature lists them, after
  /// any hidden operand the caller has already placed in `args`.
  [[nodiscard]] auto call_arguments(const hir::storage_op &op,
                                    const location &dest,
                                    std::vector<llvm::Value *> &args)
      -> checked<void> {
    if (op.signature.returns_through_address()) {
      args.push_back(pointer_at(dest));
    }
    for (const auto &arg : op.args) {
      if (arg.cls == pass_class::zero_sized) {
        continue;
      }
      if (hir::is_indirect(arg.cls)) {
        const auto where = locate(arg.slot);
        if (!where.has_value()) {
          return std::unexpected(where.error());
        }
        args.push_back(pointer_at(*where));
      } else {
        const auto value = operand_value(arg.value);
        if (!value.has_value()) {
          return std::unexpected(value.error());
        }
        args.push_back(*value);
      }
    }
    return {};
  }

  /// A call through a `fn` value `{entry, env, glue}`: the entry address and
  /// the environment are read from the value's bytes, and the environment is
  /// the callee's first operand.
  [[nodiscard]] auto emit_call_function(const hir::storage_op &op)
      -> checked<void> {
    auto closure = locate(*op.callee.callable);
    if (!closure.has_value()) {
      return std::unexpected(closure.error());
    }
    auto *entry = builder_.CreateAlignedLoad(
        ptr_type(), pointer_at(*closure),
        access_align(*closure, runtime::k_word_size));
    closure->offset += runtime::k_word_size;
    auto *environment = builder_.CreateAlignedLoad(
        ptr_type(), pointer_at(*closure),
        access_align(*closure, runtime::k_word_size));
    const auto dest = locate(op.dest);
    if (!dest.has_value()) {
      return std::unexpected(dest.error());
    }
    auto signature = op.signature;
    signature.takes_environment = true;
    const auto type = function_type_for(mc_, signature, op.span);
    if (!type.has_value()) {
      return std::unexpected(type.error());
    }
    auto args = std::vector<llvm::Value *>{environment};
    if (auto ok = call_arguments(op, *dest, args); !ok.has_value()) {
      return ok;
    }
    auto *call = builder_.CreateCall(*type, entry, args);
    const auto direct_result = !op.signature.returns_through_address() &&
                               op.signature.result.cls != pass_class::zero_sized;
    if (!direct_result) {
      return {};
    }
    if (const auto extension = result_extension(
            mc_, (*type)->getReturnType(), op.signature.result.type);
        extension.has_value()) {
      call->addRetAttr(*extension);
    }
    return store_scalar(*dest, op.dest.type, call);
  }

  /// Builds a `fn` value `{entry, env, glue}` in its destination. A non-empty
  /// environment is one heap block that every capture is stored into.
  [[nodiscard]] auto emit_make_closure(const hir::storage_op &op)
      -> checked<void> {
    const auto entry = mc_.functions.find(key(op.entry));
    if (entry == mc_.functions.end()) {
      return fail(codegen_error_kind::unknown_callee, op.span,
                  std::format("`{}` is not a function of this module", op.entry));
    }
    llvm::Value *glue = llvm::ConstantPointerNull::get(
        llvm::PointerType::get(mc_.ctx, 0));
    if (!op.glue.empty()) {
      const auto found = mc_.functions.find(key(op.glue));
      if (found == mc_.functions.end()) {
        return fail(codegen_error_kind::unknown_callee, op.span,
                    std::format("`{}` is not a function of this module", op.glue));
      }
      glue = found->second;
    }
    llvm::Value *environment = llvm::ConstantPointerNull::get(
        llvm::PointerType::get(mc_.ctx, 0));
    if (op.env_size != 0) {
      const auto id = hir::runtime_intrinsic_id("rt_alloc");
      if (!id.has_value()) {
        return fail(codegen_error_kind::unsupported_construct, op.span,
                    "the runtime has no `rt_alloc`");
      }
      environment = builder_.CreateCall(runtime_function(mc_, *id),
                                        {constant(i64(), op.env_size)});
      for (const auto &capture : op.captures) {
        if (auto ok = emit_capture(environment, capture); !ok.has_value()) {
          return ok;
        }
      }
    }
    auto dest = locate(op.dest);
    if (!dest.has_value()) {
      return std::unexpected(dest.error());
    }
    for (llvm::Value *word : {static_cast<llvm::Value *>(entry->second),
                              environment, glue}) {
      builder_.CreateAlignedStore(word, pointer_at(*dest),
                                  access_align(*dest, runtime::k_word_size));
      dest->offset += runtime::k_word_size;
    }
    return {};
  }

  /// Stores one captured variable into the environment block.
  [[nodiscard]] auto emit_capture(llvm::Value *environment,
                                  const hir::closure_capture &capture)
      -> checked<void> {
    const auto source = locate(capture.source);
    if (!source.has_value()) {
      return std::unexpected(source.error());
    }
    const auto into = location{.base = environment,
                               .offset = capture.offset,
                               .align = k_environment_align};
    if (capture.kind == hir::capture_kind::reference) {
      builder_.CreateAlignedStore(pointer_at(*source), pointer_at(into),
                                  access_align(into, runtime::k_word_size));
      return {};
    }
    const auto value = class_of(mc_, capture.source.type, current_span_);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    if (value->cls == pass_class::zero_sized) {
      return {};
    }
    if (!hir::is_indirect(value->cls)) {
      const auto held = load_scalar(*source, capture.source.type);
      if (!held.has_value()) {
        return std::unexpected(held.error());
      }
      return store_scalar(into, capture.source.type, *held);
    }
    builder_.CreateMemCpy(pointer_at(into), access_align(into, value->align),
                          pointer_at(*source), access_align(*source, value->align),
                          constant(i64(), value->size));
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
      return fail(codegen_error_kind::unsupported_construct, op.span,
                  std::format("`drop_in_place` of `{}` has no llvm_codegen "
                              "form yet",
                              types().display(op.dest.type)));
    }
    auto closure = locate(op.dest);
    if (!closure.has_value()) {
      return std::unexpected(closure.error());
    }
    closure->offset += runtime::k_word_size;
    auto *environment = builder_.CreateAlignedLoad(
        ptr_type(), pointer_at(*closure),
        access_align(*closure, runtime::k_word_size));
    closure->offset += runtime::k_word_size;
    auto *glue = builder_.CreateAlignedLoad(
        ptr_type(), pointer_at(*closure),
        access_align(*closure, runtime::k_word_size));
    auto *run = llvm::BasicBlock::Create(mc_.ctx, "glue", llvm_fn_);
    auto *after = llvm::BasicBlock::Create(mc_.ctx, "glue.done", llvm_fn_);
    builder_.CreateCondBr(builder_.CreateIsNotNull(glue), run, after);
    builder_.SetInsertPoint(run);
    auto *glue_type = llvm::FunctionType::get(llvm::Type::getVoidTy(mc_.ctx),
                                              {ptr_type()}, /*isVarArg=*/false);
    builder_.CreateCall(glue_type, glue, {environment});
    builder_.CreateBr(after);
    builder_.SetInsertPoint(after);
    return {};
  }

  // ------------------------------------------------------------------
  //  Generators
  //
  //  A coroutine is the runtime's eight-word object (`coroutine.h`). A step
  //  function runs against the state block the object points at; the value
  //  word holds the address of the item the producer last yielded, which lies
  //  in that block.
  // ------------------------------------------------------------------

  /// Whether `type` is a coroutine handle: a generator or a task.
  [[nodiscard]] auto is_generator(type_id type) const -> bool {
    const auto &entry = types().entry(types().strip_refinement(type));
    return entry.kind == semantic::type_kind::builtin_generic_kind &&
           (entry.name == "generator" || entry.name == "task");
  }

  [[nodiscard]] auto read_place(const hir::place &where) -> checked<llvm::Value *> {
    return operand_value(hir::operand{
        .kind = operand_kind::read, .where = where, .type = where.type});
  }

  [[nodiscard]] auto word_address(llvm::Value *object, size_t slot)
      -> llvm::Value * {
    return builder_.CreateInBoundsGEP(
        i8(), object, {constant(i64(), slot * runtime::k_word_size)});
  }

  [[nodiscard]] auto load_word(llvm::Type *type, llvm::Value *object, size_t slot)
      -> llvm::Value * {
    return builder_.CreateAlignedLoad(type, word_address(object, slot),
                                      llvm::Align(runtime::k_word_size));
  }

  auto store_word(llvm::Value *object, size_t slot, llvm::Value *value) -> void {
    builder_.CreateAlignedStore(value, word_address(object, slot),
                                llvm::Align(runtime::k_word_size));
  }

  [[nodiscard]] auto emit_make_generator(const hir::storage_op &op)
      -> checked<void> {
    namespace co = runtime::coroutine;
    const auto entry = mc_.functions.find(key(op.entry));
    if (entry == mc_.functions.end()) {
      return fail(codegen_error_kind::unknown_callee, op.span,
                  std::format("`{}` is not a function of this module", op.entry));
    }
    const auto alloc = hir::runtime_intrinsic_id("rt_alloc");
    if (!alloc.has_value()) {
      return fail(codegen_error_kind::unsupported_construct, op.span,
                  "the runtime has no `rt_alloc`");
    }
    llvm::Value *state = llvm::ConstantPointerNull::get(
        llvm::PointerType::get(mc_.ctx, 0));
    if (op.env_size != 0) {
      state = builder_.CreateCall(runtime_function(mc_, *alloc),
                                  {constant(i64(), op.env_size)});
      for (const auto &capture : op.captures) {
        if (auto ok = emit_capture(state, capture); !ok.has_value()) {
          return ok;
        }
      }
    }
    auto *object = builder_.CreateCall(
        runtime_function(mc_, *alloc),
        {constant(i64(), co::k_slot_count * runtime::k_word_size)});
    store_word(object, co::k_step_function_slot, entry->second);
    store_word(object, co::k_state_ptr_slot, state);
    store_word(object, co::k_resume_index_slot, constant(i64(), 0));
    store_word(object, co::k_status_slot,
               constant(i64(), co::raw(co::status::suspended)));
    store_word(object, co::k_record_slot, constant(i64(), 0));
    store_word(object, co::k_value_slot, constant(i64(), 0));
    store_word(object, co::k_value_slot + 1, constant(i64(), 0));
    store_word(object, co::k_state_bytes_slot, constant(i64(), op.env_size));
    const auto dest = locate(op.dest);
    if (!dest.has_value()) {
      return std::unexpected(dest.error());
    }
    builder_.CreateAlignedStore(object, pointer_at(*dest),
                                access_align(*dest, runtime::k_word_size));
    return {};
  }

  /// Calls the step function of the coroutine at `object` with the state
  /// block and resume index it holds.
  [[nodiscard]] auto call_step(llvm::Value *object) -> llvm::Value * {
    namespace co = runtime::coroutine;
    auto *step = load_word(ptr_type(), object, co::k_step_function_slot);
    auto *state = load_word(ptr_type(), object, co::k_state_ptr_slot);
    auto *resume = load_word(i64(), object, co::k_resume_index_slot);
    auto *type = llvm::FunctionType::get(i64(), {ptr_type(), i64(), ptr_type()},
                                         /*isVarArg=*/false);
    return builder_.CreateCall(type, step, {state, resume, object});
  }

  /// Runs the producer until it suspends or ends; the status is the result.
  /// A finished one is not run again: its status is read back.
  [[nodiscard]] auto emit_generator_resume(const hir::storage_op &op)
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
    auto *current = load_word(i64(), *object, co::k_status_slot);
    auto *runnable = builder_.CreateICmpEQ(
        current, constant(i64(), co::raw(co::status::suspended)));
    auto *from = builder_.GetInsertBlock();
    auto *call_block = llvm::BasicBlock::Create(mc_.ctx, "gen.call", llvm_fn_);
    auto *join = llvm::BasicBlock::Create(mc_.ctx, "gen.resumed", llvm_fn_);
    builder_.CreateCondBr(runnable, call_block, join);
    builder_.SetInsertPoint(call_block);
    auto *stepped = call_step(*object);
    builder_.CreateBr(join);
    builder_.SetInsertPoint(join);
    auto *status = builder_.CreatePHI(i64(), 2);
    status->addIncoming(current, from);
    status->addIncoming(stepped, call_block);
    return store_scalar(*dest, op.dest.type, status);
  }

  /// Moves the item the producer yielded out of its state block.
  [[nodiscard]] auto emit_generator_take(const hir::storage_op &op)
      -> checked<void> {
    namespace co = runtime::coroutine;
    const auto value = class_of(mc_, op.dest.type, op.span);
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
    auto *item = load_word(ptr_type(), *object, co::k_value_slot);
    // The item is now the taker's: the coroutine no longer holds one.
    store_word(*object, co::k_value_slot, constant(i64(), 0));
    auto dest = locate(op.dest);
    if (!dest.has_value()) {
      return std::unexpected(dest.error());
    }
    const auto from = location{.base = item, .offset = 0, .align = 1};
    if (!hir::is_indirect(value->cls)) {
      const auto held = load_scalar(from, op.dest.type);
      if (!held.has_value()) {
        return std::unexpected(held.error());
      }
      return store_scalar(*dest, op.dest.type, *held);
    }
    builder_.CreateMemCpy(pointer_at(*dest), access_align(*dest, value->align),
                          item, llvm::Align(1), constant(i64(), value->size));
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
    auto *start = llvm::BasicBlock::Create(mc_.ctx, "start", llvm_fn_);
    auto *table = builder_.CreateSwitch(*resume, start, op.depth);
    for (size_t point = 1; point <= op.depth; ++point) {
      auto *block = llvm::BasicBlock::Create(
          mc_.ctx, std::format("resume.{}", point), llvm_fn_);
      resume_blocks_.push_back(block);
      table->addCase(llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i64()),
                                            point),
                     block);
    }
    builder_.SetInsertPoint(start);
    return {};
  }

  /// Hands the item to the resumer and returns; the code after this operation
  /// is where the dispatch resumes.
  [[nodiscard]] auto emit_coroutine_suspend(const hir::storage_op &op)
      -> checked<void> {
    namespace co = runtime::coroutine;
    if (op.depth == 0 || op.depth > resume_blocks_.size()) {
      return fail(codegen_error_kind::unsupported_construct, op.span,
                  "a suspension point the dispatch does not name");
    }
    const auto object = read_place(op.dest);
    if (!object.has_value()) {
      return std::unexpected(object.error());
    }
    const auto item = class_of(mc_, op.source.type, op.span);
    if (!item.has_value()) {
      return std::unexpected(item.error());
    }
    llvm::Value *address = llvm::ConstantPointerNull::get(
        llvm::PointerType::get(mc_.ctx, 0));
    if (item->cls != pass_class::zero_sized) {
      const auto where = locate(op.source);
      if (!where.has_value()) {
        return std::unexpected(where.error());
      }
      address = pointer_at(*where);
    }
    store_word(*object, co::k_value_slot, address);
    store_word(*object, co::k_resume_index_slot, constant(i64(), op.depth));
    builder_.CreateRet(constant(i64(), co::raw(co::status::suspended)));
    builder_.SetInsertPoint(resume_blocks_[op.depth - 1]);
    return {};
  }

  /// A task waits for another. This is its own resume point: the dispatch
  /// lands here, the step polls the awaited task, and while it has not
  /// finished the step returns `pending` so the scheduler runs something else
  /// and wakes it later.
  [[nodiscard]] auto emit_coroutine_await(const hir::storage_op &op)
      -> checked<void> {
    namespace co = runtime::coroutine;
    if (op.depth == 0 || op.depth > resume_blocks_.size()) {
      return fail(codegen_error_kind::unsupported_construct, op.span,
                  "a suspension point the dispatch does not name");
    }
    const auto name = op.waits == hir::wait_mode::one
                          ? "rt_task_await_poll"
                          : op.waits == hir::wait_mode::crew
                                ? "rt_crew_join_poll"
                          : op.waits == hir::wait_mode::all
                                ? "rt_task_await_all_poll"
                                : "rt_task_await_any_poll";
    const auto poll = hir::runtime_intrinsic_id(name);
    if (!poll.has_value()) {
      return fail(codegen_error_kind::unsupported_construct, op.span,
                  std::string{"the runtime has no `"} + name + "`");
    }
    auto *landing = resume_blocks_[op.depth - 1];
    builder_.CreateBr(landing);
    builder_.SetInsertPoint(landing);
    const auto object = read_place(op.dest);
    if (!object.has_value()) {
      return std::unexpected(object.error());
    }
    auto arguments = group_arguments(op);
    if (!arguments.has_value()) {
      return std::unexpected(arguments.error());
    }
    arguments->insert(arguments->begin(), *object);
    store_word(*object, co::k_resume_index_slot, constant(i64(), op.depth));
    auto *finished =
        builder_.CreateCall(runtime_function(mc_, *poll), *arguments);
    auto *waiting = llvm::BasicBlock::Create(mc_.ctx, "await.pending", llvm_fn_);
    auto *ready = llvm::BasicBlock::Create(mc_.ctx, "await.ready", llvm_fn_);
    builder_.CreateCondBr(
        builder_.CreateICmpNE(finished, constant(finished->getType(), 0)),
        ready, waiting);
    builder_.SetInsertPoint(waiting);
    builder_.CreateRet(constant(i64(), co::raw(co::status::pending)));
    builder_.SetInsertPoint(ready);
    return {};
  }

  /// What the runtime is given for the awaited thing in `op.source`: the
  /// task's handle, or a group's address and member count (the runtime reads
  /// a group's handles in place).
  [[nodiscard]] auto group_arguments(const hir::storage_op &op)
      -> checked<std::vector<llvm::Value *>> {
    if (op.waits == hir::wait_mode::one || op.waits == hir::wait_mode::crew) {
      const auto awaited = read_place(op.source);
      if (!awaited.has_value()) {
        return std::unexpected(awaited.error());
      }
      return std::vector<llvm::Value *>{*awaited};
    }
    const auto where = locate(op.source);
    if (!where.has_value()) {
      return std::unexpected(where.error());
    }
    const auto &group =
        types().entry(types().strip_refinement(op.source.type));
    return std::vector<llvm::Value *>{pointer_at(*where),
                                      constant(i64(), group.args.size())};
  }

  /// `await par` / `await race` outside a task: the thread blocks.
  [[nodiscard]] auto emit_task_block_on_group(const hir::storage_op &op)
      -> checked<void> {
    const auto name = op.waits == hir::wait_mode::all ? "rt_task_block_on_all"
                                                      : "rt_task_block_on_any";
    const auto id = hir::runtime_intrinsic_id(name);
    if (!id.has_value()) {
      return fail(codegen_error_kind::unsupported_construct, op.span,
                  std::string{"the runtime has no `"} + name + "`");
    }
    const auto arguments = group_arguments(op);
    if (!arguments.has_value()) {
      return std::unexpected(arguments.error());
    }
    builder_.CreateCall(runtime_function(mc_, *id), *arguments);
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
    auto *item = load_word(i64(), *object, co::k_value_slot);
    auto *held = builder_.CreateICmpNE(item, constant(i64(), 0));
    return store_scalar(*dest, op.dest.type, held);
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
    auto *status = load_word(i64(), *object, co::k_status_slot);
    auto *asked = builder_.CreateICmpEQ(
        status, constant(i64(), co::raw(co::status::cancel_requested)));
    return store_scalar(*dest, op.dest.type, asked);
  }

  [[nodiscard]] auto emit_coroutine_finish(const hir::storage_op &op)
      -> checked<void> {
    namespace co = runtime::coroutine;
    const auto object = read_place(op.source);
    if (!object.has_value()) {
      return std::unexpected(object.error());
    }
    store_word(*object, co::k_status_slot,
               constant(i64(), co::raw(co::status::done)));
    return {};
  }

  /// Dropping a generator: one still suspended is unwound once (by the
  /// scheduler if it owns the task, otherwise by resuming it here with a
  /// cancel request), then the coroutine is released.
  [[nodiscard]] auto emit_generator_drop(const hir::storage_op &op)
      -> checked<void> {
    namespace co = runtime::coroutine;
    const auto drop = hir::runtime_intrinsic_id("rt_task_drop");
    const auto release = hir::runtime_intrinsic_id("rt_task_release");
    if (!drop.has_value() || !release.has_value()) {
      return fail(codegen_error_kind::unsupported_construct, op.span,
                  "the runtime has no `rt_task_drop` or `rt_task_release`");
    }
    const auto object = read_place(op.dest);
    if (!object.has_value()) {
      return std::unexpected(object.error());
    }
    auto *running = builder_.CreateICmpEQ(
        load_word(i64(), *object, co::k_status_slot),
        constant(i64(), co::raw(co::status::suspended)));
    auto *cancel = llvm::BasicBlock::Create(mc_.ctx, "gen.cancel", llvm_fn_);
    auto *inline_block =
        llvm::BasicBlock::Create(mc_.ctx, "gen.cancel.inline", llvm_fn_);
    auto *done = llvm::BasicBlock::Create(mc_.ctx, "gen.cancel.done", llvm_fn_);
    builder_.CreateCondBr(running, cancel, done);
    builder_.SetInsertPoint(cancel);
    auto *handed_over =
        builder_.CreateCall(runtime_function(mc_, *drop), {*object});
    builder_.CreateCondBr(
        builder_.CreateICmpNE(handed_over, constant(handed_over->getType(), 0)),
        done, inline_block);
    builder_.SetInsertPoint(inline_block);
    store_word(*object, co::k_status_slot,
               constant(i64(), co::raw(co::status::cancel_requested)));
    static_cast<void>(call_step(*object));
    builder_.CreateBr(done);
    builder_.SetInsertPoint(done);
    builder_.CreateCall(runtime_function(mc_, *release), {*object});
    return {};
  }

  /// Frees the environment block this function received.
  [[nodiscard]] auto emit_release_environment(const hir::storage_op &op)
      -> checked<void> {
    const auto id = hir::runtime_intrinsic_id("rt_free");
    if (!id.has_value() || environment_ == nullptr) {
      return fail(codegen_error_kind::unsupported_construct, op.span,
                  "`release_env` needs the runtime's `rt_free` and an "
                  "environment operand");
    }
    builder_.CreateCall(runtime_function(mc_, *id),
                        {environment_, constant(i64(), op.env_size)});
    return {};
  }

  /// Calls the runtime's native implementation of intrinsic `id`, after
  /// checking the call's signature against the symbol (`hir::
  /// check_intrinsic_signature`). A descriptor or aggregate argument is the
  /// address of the slot holding it; a scalar crosses at its wire kind; a
  /// result built in caller storage is written through the destination.
  /// An intrinsic that is one instruction (`cinder::inline_intrinsic`): an
  /// acquire-release atomic add or subtract of a `usize` at a raw pointer,
  /// giving the old value.
  [[nodiscard]] auto emit_inline_intrinsic(const hir::storage_op &op,
                                           cinder::inline_intrinsic which)
      -> checked<void> {
    if (op.args.size() != 2) {
      return fail(codegen_error_kind::unsupported_construct, op.span,
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
    const auto rmw = which == cinder::inline_intrinsic::atomic_fetch_add
                         ? llvm::AtomicRMWInst::Add
                         : llvm::AtomicRMWInst::Sub;
    auto *old = builder_.CreateAtomicRMW(rmw, *ptr, *amount, llvm::MaybeAlign(8),
                                         llvm::AtomicOrdering::AcquireRelease);
    return store_scalar(*dest, op.dest.type, old);
  }

  [[nodiscard]] auto emit_intrinsic_call(const hir::storage_op &op, size_t id)
      -> checked<void> {
    if (const auto problem = hir::check_intrinsic_signature(op.signature, id);
        problem.has_value()) {
      return fail(codegen_error_kind::unsupported_construct, op.span, *problem);
    }
    const auto dest = locate(op.dest);
    if (!dest.has_value()) {
      return std::unexpected(dest.error());
    }
    auto args = std::vector<llvm::Value *>{};
    if (op.signature.returns_through_address()) {
      args.push_back(pointer_at(*dest));
    }
    auto position = size_t{0};
    for (const auto &arg : op.args) {
      if (arg.cls == pass_class::zero_sized) {
        continue;
      }
      if (hir::is_indirect(arg.cls)) {
        const auto where = locate(arg.slot);
        if (!where.has_value()) {
          return std::unexpected(where.error());
        }
        args.push_back(pointer_at(*where));
      } else {
        const auto value = operand_value(arg.value);
        if (!value.has_value()) {
          return std::unexpected(value.error());
        }
        const auto wire = cinder::known_intrinsic_param_kinds.at(id).at(position);
        const auto converted = to_wire(
            *value, op.signature.params.at(arg.source_index).type, wire, op.span);
        if (!converted.has_value()) {
          return std::unexpected(converted.error());
        }
        args.push_back(*converted);
      }
      ++position;
    }
    auto *call = builder_.CreateCall(runtime_function(mc_, id), args);
    if (cinder::intrinsic_builds_into(id) ||
        op.signature.result.cls == pass_class::zero_sized) {
      return {};
    }
    const auto result = from_wire(call, op.dest.type,
                                  cinder::known_intrinsic_return_kinds.at(id), op.span);
    if (!result.has_value()) {
      return std::unexpected(result.error());
    }
    return store_scalar(*dest, op.dest.type, *result);
  }

  /// A scalar argument at the wire kind the runtime symbol declares:
  /// integers extend from their own signedness, floats pass as they are, and
  /// an address or handle is already a pointer.
  [[nodiscard]] auto to_wire(llvm::Value *value, type_id type,
                             cinder::intrinsic_wire_kind wire,
                             const source_span &span) -> checked<llvm::Value *> {
    using wire_kind = cinder::intrinsic_wire_kind;
    if (wire == wire_kind::ptr || wire == wire_kind::f32 ||
        wire == wire_kind::f64) {
      return value;
    }
    const auto kind = kind_of(mc_, type, span);
    if (!kind.has_value()) {
      return std::unexpected(kind.error());
    }
    const auto is_signed = bytecode::is_signed_integer(*kind);
    const auto target = wire == wire_kind::i32
                            ? (is_signed ? numeric_kind::i32 : numeric_kind::u32)
                            : (is_signed ? numeric_kind::i64 : numeric_kind::u64);
    return scalars_.cast(*kind, target, value);
  }

  /// A scalar result from the wire kind the runtime symbol returns, as the
  /// declared Cinder type: a `bool` is the number compared with zero, other
  /// integers convert from the wire width, and a pointer stays a pointer.
  [[nodiscard]] auto from_wire(llvm::Value *value, type_id type,
                               cinder::intrinsic_wire_kind wire,
                               const source_span &span) -> checked<llvm::Value *> {
    using wire_kind = cinder::intrinsic_wire_kind;
    if (wire == wire_kind::ptr || wire == wire_kind::f32 ||
        wire == wire_kind::f64) {
      return value;
    }
    const auto kind = kind_of(mc_, type, span);
    if (!kind.has_value()) {
      return std::unexpected(kind.error());
    }
    if (*kind == numeric_kind::boolean) {
      return builder_.CreateICmpNE(value, llvm::ConstantInt::get(value->getType(), 0));
    }
    const auto is_signed = bytecode::is_signed_integer(*kind);
    const auto source = wire == wire_kind::i32
                            ? (is_signed ? numeric_kind::i32 : numeric_kind::u32)
                            : (is_signed ? numeric_kind::i64 : numeric_kind::u64);
    return scalars_.cast(source, *kind, value);
  }

  /// A `str` descriptor `{len, data}` with `data` a private global holding
  /// the literal's bytes.
  [[nodiscard]] auto emit_string_literal(const hir::storage_op &op)
      -> checked<void> {
    const auto dest = locate(op.dest);
    if (!dest.has_value()) {
      return std::unexpected(dest.error());
    }
    auto *bytes = builder_.CreateGlobalString(op.text, "str.lit");
    builder_.CreateAlignedStore(constant(i64(), op.text.size()),
                                pointer_at(*dest), access_align(*dest, 8));
    auto data = *dest;
    data.offset += 8;
    builder_.CreateAlignedStore(bytes, pointer_at(data), access_align(data, 8));
    return {};
  }

  [[nodiscard]] auto emit_return(const hir::storage_op &op) -> checked<void> {
    if (!op.value.has_value()) {
      builder_.CreateRetVoid();
    } else {
      const auto value = operand_value(*op.value);
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
      builder_.CreateRet(*value);
    }
    start_dead_block();
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
    auto *then_block = llvm::BasicBlock::Create(mc_.ctx, "if.then", llvm_fn_);
    auto region = open_region{.what = open_region::kind::conditional};
    region.else_block = llvm::BasicBlock::Create(mc_.ctx, "if.else", llvm_fn_);
    region.after = llvm::BasicBlock::Create(mc_.ctx, "if.end", llvm_fn_);
    builder_.CreateCondBr(*condition, then_block, region.else_block);
    builder_.SetInsertPoint(then_block);
    regions_.push_back(region);
    return {};
  }

  [[nodiscard]] auto emit_else() -> checked<void> {
    auto &region = regions_.back();
    branch_to(region.after);
    builder_.SetInsertPoint(region.else_block);
    region.has_else = true;
    return {};
  }

  [[nodiscard]] auto emit_if_end() -> checked<void> {
    const auto region = regions_.back();
    regions_.pop_back();
    branch_to(region.after);
    if (!region.has_else) {
      // The false edge goes straight to the join.
      builder_.SetInsertPoint(region.else_block);
      builder_.CreateBr(region.after);
    }
    builder_.SetInsertPoint(region.after);
    return {};
  }

  [[nodiscard]] auto emit_loop_begin() -> checked<void> {
    auto region = open_region{.what = open_region::kind::loop};
    region.header = llvm::BasicBlock::Create(mc_.ctx, "loop", llvm_fn_);
    region.after = llvm::BasicBlock::Create(mc_.ctx, "loop.end", llvm_fn_);
    branch_to(region.header);
    builder_.SetInsertPoint(region.header);
    regions_.push_back(region);
    return {};
  }

  [[nodiscard]] auto emit_loop_end() -> checked<void> {
    const auto region = regions_.back();
    regions_.pop_back();
    branch_to(region.header);
    builder_.SetInsertPoint(region.after);
    return {};
  }

  [[nodiscard]] auto emit_block_begin() -> checked<void> {
    auto region = open_region{.what = open_region::kind::block};
    region.after = llvm::BasicBlock::Create(mc_.ctx, "block.end", llvm_fn_);
    regions_.push_back(region);
    return {};
  }

  [[nodiscard]] auto emit_block_end() -> checked<void> {
    const auto region = regions_.back();
    regions_.pop_back();
    branch_to(region.after);
    builder_.SetInsertPoint(region.after);
    return {};
  }

  [[nodiscard]] auto emit_exit(const hir::storage_op &op) -> checked<void> {
    auto remaining = op.depth;
    for (auto it = regions_.rbegin(); it != regions_.rend(); ++it) {
      if (it->what == open_region::kind::conditional) {
        continue;
      }
      if (remaining-- == 0) {
        builder_.CreateBr(it->after);
        start_dead_block();
        return {};
      }
    }
    return fail(codegen_error_kind::unsupported_construct, op.span,
                "an `exit` names a loop or block that is not open");
  }

  // ------------------------------------------------------------------
  //  Finishing
  // ------------------------------------------------------------------

  /// Gives every block a terminator. The verifier guarantees each path
  /// returns, so a block still open here is one nothing reaches.
  auto finish() -> void {
    for (auto &block : *llvm_fn_) {
      if (block.hasTerminator()) {
        continue;
      }
      builder_.SetInsertPoint(&block);
      if (llvm_fn_->getReturnType()->isVoidTy()) {
        builder_.CreateRetVoid();
      } else {
        builder_.CreateUnreachable();
      }
    }
  }

  module_context &mc_;
  const hir::storage_module &module_;
  const hir::storage_function &fn_;
  llvm::Function *llvm_fn_;
  llvm::IRBuilder<> builder_;
  scalar_builder scalars_;
  llvm::BasicBlock *entry_ = nullptr;
  source_span current_span_{};

  std::vector<home> homes_;
  llvm::Value *anchor_ = nullptr;
  /// The hidden environment argument of a closure body or drop glue.
  llvm::Value *environment_ = nullptr;
  std::vector<open_region> regions_;
  /// The blocks a step's dispatch jumps to, one per suspension point.
  std::vector<llvm::BasicBlock *> resume_blocks_;
};

/// Emits `void <prefix><name>(const uint64_t *args, void *result)` for one
/// function; see `k_storage_entry_prefix`.
[[nodiscard]] auto emit_entry_thunk(module_context &mc,
                                    const hir::storage_function &function,
                                    std::string_view program_name,
                                    llvm::Function *target) -> checked<void> {
  auto &ctx = mc.ctx;
  auto *ptr_ty = llvm::PointerType::get(ctx, 0);
  auto *thunk = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {ptr_ty, ptr_ty},
                              /*isVarArg=*/false),
      llvm::Function::ExternalLinkage,
      std::string(k_storage_entry_prefix) + std::string(program_name),
      mc.module);
  auto builder = llvm::IRBuilder<>(ctx);
  builder.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", thunk));
  auto *args = thunk->getArg(0);
  auto *result = thunk->getArg(1);
  const auto span = function.slots.empty() ? source_span{}
                                           : function.slots.front().span;

  auto call_args = std::vector<llvm::Value *>{};
  if (function.signature.returns_through_address()) {
    call_args.push_back(result);
  }
  auto slot_index = uint64_t{0};
  for (const auto &param : function.signature.params) {
    if (param.cls == pass_class::zero_sized) {
      continue;
    }
    auto *slot_address = builder.CreateInBoundsGEP(
        llvm::Type::getInt8Ty(ctx), args,
        {llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), slot_index * 8)});
    ++slot_index;
    if (hir::is_indirect(param.cls) || param.cls == pass_class::address ||
        param.cls == pass_class::handle) {
      call_args.push_back(builder.CreateAlignedLoad(ptr_ty, slot_address,
                                                    llvm::Align(8)));
      continue;
    }
    const auto stored = memory_type(mc, param.type, span);
    if (!stored.has_value()) {
      return std::unexpected(stored.error());
    }
    llvm::Value *loaded =
        builder.CreateAlignedLoad(*stored, slot_address, llvm::Align(8));
    // A `bool` is a byte in memory and an `i1` as an argument.
    const auto *wanted =
        target->getArg(static_cast<unsigned>(call_args.size()))->getType();
    if (wanted->isIntegerTy(1)) {
      loaded = builder.CreateICmpNE(loaded, llvm::ConstantInt::get(*stored, 0));
    }
    call_args.push_back(loaded);
  }
  auto *call = call_function(builder, target, call_args);
  const auto direct_result = !function.signature.returns_through_address() &&
                             function.signature.result.cls != pass_class::zero_sized;
  if (direct_result) {
    llvm::Value *value = call;
    if (value->getType()->isIntegerTy(1)) {
      value = builder.CreateZExt(value, llvm::Type::getInt8Ty(ctx));
    }
    builder.CreateAlignedStore(value, result, llvm::Align(1));
  }
  builder.CreateRetVoid();
  return {};
}

/// Registers one load-time constructor that runs each module's static init,
/// in module order, through `llvm.global_ctors`: the C runtime runs it
/// before `main`, and `jit_module::create` runs it in the JIT.
[[nodiscard]] auto emit_static_ctor(
    module_context &mc, std::span<const hir::storage_module *const> modules)
    -> checked<void> {
  auto inits = std::vector<llvm::Function *>{};
  for (const auto *module : modules) {
    if (module->globals.empty()) {
      continue;
    }
    const auto found = mc.functions.find(hir::program_function_name(
        mc.entry, module->name, hir::k_static_init_name));
    if (found == mc.functions.end() || found->second->arg_size() != 0) {
      return fail(codegen_error_kind::unsupported_construct, source_span{},
                  std::format("module `{}` declares statics but has no static "
                              "init function",
                              module->name));
    }
    inits.push_back(found->second);
  }
  if (inits.empty()) {
    return {};
  }
  auto &ctx = mc.ctx;
  auto *ctor = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), /*isVarArg=*/false),
      llvm::Function::InternalLinkage, "__cinder_storage_static_init",
      mc.module);
  auto builder = llvm::IRBuilder<>(llvm::BasicBlock::Create(ctx, "entry", ctor));
  for (auto *init : inits) {
    builder.CreateCall(init, {});
  }
  builder.CreateRetVoid();

  // `{ i32 priority, ptr fn, ptr data }`; the lowest priority, since nothing
  // else depends on running first.
  auto *ptr_ty = llvm::PointerType::get(ctx, 0);
  auto *entry_ty = llvm::StructType::get(
      ctx, {llvm::Type::getInt32Ty(ctx), ptr_ty, ptr_ty});
  auto *array_ty = llvm::ArrayType::get(entry_ty, 1);
  auto *entry = llvm::ConstantStruct::get(
      entry_ty, {llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 65535),
                 ctor, llvm::ConstantPointerNull::get(ptr_ty)});
  new llvm::GlobalVariable(mc.module, array_ty, /*isConstant=*/false,
                           llvm::GlobalValue::AppendingLinkage,
                           llvm::ConstantArray::get(array_ty, {entry}),
                           "llvm.global_ctors");
  return {};
}

} // namespace

auto compile_storage_module(std::span<const hir::storage_module *const> modules,
                            hir::storage_env &env,
                            const storage_codegen_options &options)
    -> std::expected<compiled_module, codegen_error> {
  if (modules.empty()) {
    return fail(codegen_error_kind::unsupported_construct, source_span{},
                "a program needs an entry module");
  }
  const auto &entry = modules.front()->name;
  auto result = compiled_module{
      .context = std::make_unique<llvm::LLVMContext>(),
      .module = nullptr,
  };
  result.module = std::make_unique<llvm::Module>(entry, *result.context);
  auto &ctx = *result.context;
  auto &llvm_module = *result.module;

  auto *panic_fn = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(ctx),
                              {llvm::Type::getInt8Ty(ctx)},
                              /*isVarArg=*/false),
      llvm::Function::ExternalLinkage, kPanicSymbolName, llvm_module);
  panic_fn->setDoesNotReturn();

  auto mc = module_context{.ctx = ctx,
                           .module = llvm_module,
                           .env = env,
                           .panic_fn = panic_fn,
                           .entry = entry,
                           .functions = {}};
  const auto name_of = [&](const hir::storage_module &module,
                           const hir::storage_function &function) {
    return hir::program_function_name(entry, module.name, function.name);
  };

  // Each static is zeroed bytes at its type's alignment, built at load time.
  for (const auto *module : modules) {
    for (const auto &global : module->globals) {
      const auto layout = layout_of(mc, global.type, global.span);
      if (!layout.has_value()) {
        return std::unexpected(layout.error());
      }
      const auto align = std::max<size_t>((*layout)->align, 1);
      auto *bytes_type = llvm::ArrayType::get(
          llvm::Type::getInt8Ty(ctx), std::max<uint64_t>((*layout)->size, 1));
      auto *bytes = new llvm::GlobalVariable(
          llvm_module, bytes_type, /*isConstant=*/false,
          llvm::GlobalValue::InternalLinkage,
          llvm::ConstantAggregateZero::get(bytes_type), "static." + global.name);
      bytes->setAlignment(llvm::Align(align));
      mc.statics.emplace(global.name, module_context::static_storage{
                                          .bytes = bytes, .align = align});
    }
  }

  // Every signature of every module is declared before any body, so a call
  // finds a real function whether it is forward, recursive, mutual, or into
  // another module.
  for (const auto *module : modules) {
    for (const auto &function : module->functions) {
      const auto span = function.slots.empty() ? source_span{}
                                               : function.slots.front().span;
      const auto type = function_type_for(mc, function.signature, span);
      if (!type.has_value()) {
        return std::unexpected(type.error());
      }
      auto *declared =
          llvm::Function::Create(*type, llvm::Function::ExternalLinkage,
                                 name_of(*module, function), llvm_module);
      if (!function.signature.returns_through_address()) {
        add_result_extension(mc, *declared, function.signature.result.type);
      }
      mc.functions.emplace(name_of(*module, function), declared);
    }
  }

  for (const auto *module : modules) {
    for (const auto &function : module->functions) {
      auto *declared = mc.functions.at(name_of(*module, function));
      if (auto ok = function_emitter(mc, *module, function, declared).compile();
          !ok.has_value()) {
        auto error = ok.error();
        error.message = std::format("in `{}`: {}", name_of(*module, function),
                                    error.message);
        return std::unexpected(std::move(error));
      }
    }
  }
  if (auto ok = emit_static_ctor(mc, modules); !ok.has_value()) {
    return std::unexpected(ok.error());
  }
  if (options.entry_thunks) {
    for (const auto *module : modules) {
      for (const auto &function : module->functions) {
        if (function.signature.takes_environment) {
          continue; // reached only through a `fn` value
        }
        const auto name = name_of(*module, function);
        if (auto ok = emit_entry_thunk(mc, function, name, mc.functions.at(name));
            !ok.has_value()) {
          return std::unexpected(ok.error());
        }
      }
    }
  }

  for (auto &function : llvm_module) {
    if (!function.isDeclaration()) {
      function.addFnAttr("probe-stack", "inline-asm");
      // A panic unwinds through these frames to the host.
      function.setUWTableKind(llvm::UWTableKind::Default);
    }
  }

  auto verify_message = std::string{};
  auto verify_stream = llvm::raw_string_ostream(verify_message);
  if (llvm::verifyModule(llvm_module, &verify_stream)) {
    return fail(codegen_error_kind::unsupported_construct, source_span{},
                std::format("llvm_codegen produced a module that failed "
                            "verification (this is a codegen bug, not a "
                            "source error): {}",
                            verify_message));
  }
  return result;
}

auto compile_storage_module(const hir::storage_module &module,
                            hir::storage_env &env,
                            const storage_codegen_options &options)
    -> std::expected<compiled_module, codegen_error> {
  const auto *module_ptr = &module;
  return compile_storage_module(
      std::span<const hir::storage_module *const>(&module_ptr, 1), env, options);
}

} // namespace cinder::llvm_codegen
