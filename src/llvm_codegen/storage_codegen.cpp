#include "src/llvm_codegen/storage_codegen.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
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
  std::unordered_map<std::string, llvm::Function *> functions;
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
auto add_result_extension(module_context &mc, llvm::Function &function,
                          type_id result) -> void {
  auto *ty = function.getReturnType();
  if (!ty->isIntegerTy() || ty->getIntegerBitWidth() >= 32) {
    return;
  }
  const auto &types = mc.env.types();
  const auto kind = bytecode::numeric_kind_of(types, types.strip_refinement(result));
  function.addRetAttr(kind.has_value() && bytecode::is_signed_integer(*kind)
                          ? llvm::Attribute::SExt
                          : llvm::Attribute::ZExt);
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
      if (operands[i].role == hir::abi_operand_role::result_address) {
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
    case op_kind::publish_variant:
      return emit_publish(op);
    case op_kind::drop_in_place:
      return fail(codegen_error_kind::unsupported_construct, op.span,
                  "`drop_in_place` has no llvm_codegen form yet");
    case op_kind::call_into:
      return emit_call(op);
    case op_kind::return_value:
      return emit_return(op);
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

  [[nodiscard]] auto emit_call(const hir::storage_op &op) -> checked<void> {
    if (!op.callee.is_direct()) {
      return fail(codegen_error_kind::unsupported_construct, op.span,
                  "a call through a `fn` value has no llvm_codegen form yet");
    }
    if (!mc_.functions.contains(op.callee.name)) {
      if (const auto id = hir::runtime_intrinsic_id(op.callee.name);
          id.has_value()) {
        return emit_intrinsic_call(op, *id);
      }
    }
    if (op.callee.owner_module.has_value() &&
        *op.callee.owner_module != module_.name) {
      return fail(codegen_error_kind::unsupported_construct, op.span,
                  std::format("a call into module `{}` is not compiled from "
                              "the storage form yet",
                              *op.callee.owner_module));
    }
    const auto found = mc_.functions.find(op.callee.name);
    if (found == mc_.functions.end()) {
      return fail(codegen_error_kind::unknown_callee, op.span,
                  std::format("`{}` is not a function of this module",
                              op.callee.name));
    }
    auto *callee = found->second;
    const auto dest = locate(op.dest);
    if (!dest.has_value()) {
      return std::unexpected(dest.error());
    }
    auto args = std::vector<llvm::Value *>{};
    if (op.signature.returns_through_address()) {
      args.push_back(pointer_at(*dest));
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

  /// Calls the runtime's native implementation of intrinsic `id`, after
  /// checking the call's signature against the symbol (`hir::
  /// check_intrinsic_signature`). A descriptor or aggregate argument is the
  /// address of the slot holding it; a scalar crosses at its wire kind; a
  /// result built in caller storage is written through the destination.
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
  std::vector<open_region> regions_;
};

/// Emits `void <prefix><name>(const uint64_t *args, void *result)` for one
/// function; see `k_storage_entry_prefix`.
[[nodiscard]] auto emit_entry_thunk(module_context &mc,
                                    const hir::storage_function &function,
                                    llvm::Function *target) -> checked<void> {
  auto &ctx = mc.ctx;
  auto *ptr_ty = llvm::PointerType::get(ctx, 0);
  auto *thunk = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {ptr_ty, ptr_ty},
                              /*isVarArg=*/false),
      llvm::Function::ExternalLinkage,
      std::string(k_storage_entry_prefix) + function.name, mc.module);
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

} // namespace

auto compile_storage_module(const hir::storage_module &module,
                            hir::storage_env &env,
                            const storage_codegen_options &options)
    -> std::expected<compiled_module, codegen_error> {
  auto result = compiled_module{
      .context = std::make_unique<llvm::LLVMContext>(),
      .module = nullptr,
  };
  result.module = std::make_unique<llvm::Module>(module.name, *result.context);
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
                           .functions = {}};

  // Every signature is declared before any body, so a call finds a real
  // function whether it is forward, recursive, or mutual.
  for (const auto &function : module.functions) {
    const auto span = function.slots.empty() ? source_span{}
                                             : function.slots.front().span;
    const auto type = function_type_for(mc, function.signature, span);
    if (!type.has_value()) {
      return std::unexpected(type.error());
    }
    auto *declared = llvm::Function::Create(
        *type, llvm::Function::ExternalLinkage, function.name, llvm_module);
    if (!function.signature.returns_through_address()) {
      add_result_extension(mc, *declared, function.signature.result.type);
    }
    mc.functions.emplace(function.name, declared);
  }

  for (const auto &function : module.functions) {
    auto *declared = mc.functions.at(function.name);
    if (auto ok = function_emitter(mc, module, function, declared).compile();
        !ok.has_value()) {
      return std::unexpected(ok.error());
    }
  }
  if (options.entry_thunks) {
    for (const auto &function : module.functions) {
      if (auto ok = emit_entry_thunk(mc, function, mc.functions.at(function.name));
          !ok.has_value()) {
        return std::unexpected(ok.error());
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

} // namespace cinder::llvm_codegen
