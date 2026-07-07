#include "src/llvm_codegen/codegen.h"

#include <charconv>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <llvm/ADT/APInt.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include "src/bytecode/panic.h"
#include "src/bytecode/value.h"
#include "src/hir/ids.h"
#include "src/k-parser/ast.h"
#include "src/k-parser/token.h"
#include "src/llvm_codegen/runtime.h"

namespace kira::llvm_codegen {

namespace {

using bytecode::bit_width;
using bytecode::is_float;
using bytecode::is_signed_integer;
using bytecode::numeric_kind;
using bytecode::numeric_kind_of;
using bytecode::panic_reason;
using hir::hir_node;
using hir::hir_node_kind;
using semantic::type_id;
using semantic::type_table;

// ==========================================================================
//  Literal decoding — duplicated from src/bytecode_compiler/compile.cpp
//  rather than shared: both are small, self-contained, and this mirrors
//  that file's own precedent of not exporting check.cpp's literal parser
//  across an unrelated module boundary for one caller (see its top comment).
// ==========================================================================

[[nodiscard]] auto parse_uint_literal(std::string_view text)
    -> std::optional<uint64_t> {
  auto cleaned = std::string{};
  cleaned.reserve(text.size());
  for (const auto ch : text) {
    if (ch != '_') {
      cleaned.push_back(ch);
    }
  }
  auto base = 10;
  auto digits = std::string_view(cleaned);
  if (digits.size() > 2 && digits[0] == '0') {
    if (digits[1] == 'x' || digits[1] == 'X') {
      base = 16;
      digits.remove_prefix(2);
    } else if (digits[1] == 'o' || digits[1] == 'O') {
      base = 8;
      digits.remove_prefix(2);
    } else if (digits[1] == 'b' || digits[1] == 'B') {
      base = 2;
      digits.remove_prefix(2);
    }
  }
  auto value = uint64_t{0};
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,bugprone-suspicious-stringview-data-usage)
  const char *digits_end = digits.data() + digits.size();
  const auto result = std::from_chars(digits.data(), digits_end, value, base);
  if (result.ec != std::errc{} || result.ptr != digits_end) {
    return std::nullopt;
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,bugprone-suspicious-stringview-data-usage)
  return value;
}

[[nodiscard]] auto parse_float_literal(std::string_view text)
    -> std::optional<double> {
  auto cleaned = std::string{};
  cleaned.reserve(text.size());
  for (const auto ch : text) {
    if (ch != '_') {
      cleaned.push_back(ch);
    }
  }
  auto value = double{0};
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const char *end = cleaned.data() + cleaned.size();
  const auto result = std::from_chars(cleaned.data(), end, value);
  if (result.ec != std::errc{} || result.ptr != end) {
    return std::nullopt;
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  return value;
}

[[nodiscard]] auto decode_utf8_scalar(std::string_view text, size_t &pos)
    -> std::optional<uint32_t> {
  if (pos >= text.size()) {
    return std::nullopt;
  }
  const auto first = static_cast<unsigned char>(text[pos]);
  if ((first & 0x80) == 0) {
    ++pos;
    return static_cast<uint32_t>(first);
  }
  auto extra_bytes = 0;
  auto value = uint32_t{0};
  if ((first & 0xE0) == 0xC0) {
    extra_bytes = 1;
    value = static_cast<uint32_t>(first & 0x1F);
  } else if ((first & 0xF0) == 0xE0) {
    extra_bytes = 2;
    value = static_cast<uint32_t>(first & 0x0F);
  } else if ((first & 0xF8) == 0xF0) {
    extra_bytes = 3;
    value = static_cast<uint32_t>(first & 0x07);
  } else {
    return std::nullopt;
  }
  ++pos;
  for (auto i = 0; i < extra_bytes; ++i) {
    if (pos >= text.size()) {
      return std::nullopt;
    }
    const auto cont = static_cast<unsigned char>(text[pos]);
    value = (value << 6) | static_cast<uint32_t>(cont & 0x3F);
    ++pos;
  }
  return value;
}

[[nodiscard]] auto decode_char_literal(std::string_view text)
    -> std::optional<uint32_t> {
  if (text.size() < 2 || text.front() != '\'' || text.back() != '\'') {
    return std::nullopt;
  }
  const auto inner = text.substr(1, text.size() - 2);
  if (inner.empty()) {
    return std::nullopt;
  }
  if (inner.front() != '\\') {
    auto pos = size_t{0};
    return decode_utf8_scalar(inner, pos);
  }
  if (inner.size() < 2) {
    return std::nullopt;
  }
  switch (inner[1]) {
  case 'n':
    return static_cast<uint32_t>('\n');
  case 't':
    return static_cast<uint32_t>('\t');
  case 'r':
    return static_cast<uint32_t>('\r');
  case '"':
    return static_cast<uint32_t>('"');
  case '\'':
    return static_cast<uint32_t>('\'');
  case '\\':
    return static_cast<uint32_t>('\\');
  case '{':
    return static_cast<uint32_t>('{');
  case '}':
    return static_cast<uint32_t>('}');
  case 'u': {
    if (inner.size() < 5 || inner[2] != '{' || inner.back() != '}') {
      return std::nullopt;
    }
    const auto hex = inner.substr(3, inner.size() - 4);
    auto value = uint32_t{0};
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const char *hex_end = hex.data() + hex.size();
    const auto result = std::from_chars(hex.data(), hex_end, value, 16);
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (result.ec != std::errc{} || result.ptr != hex_end) {
      return std::nullopt;
    }
    return value;
  }
  default:
    return std::nullopt;
  }
}

// ==========================================================================
//  numeric_kind <-> llvm::Type.
// ==========================================================================

[[nodiscard]] auto llvm_type_for(llvm::LLVMContext &ctx, numeric_kind kind)
    -> llvm::Type * {
  switch (kind) {
  case numeric_kind::boolean:
    return llvm::Type::getInt1Ty(ctx);
  case numeric_kind::f32:
    return llvm::Type::getFloatTy(ctx);
  case numeric_kind::f64:
    return llvm::Type::getDoubleTy(ctx);
  default:
    // Every remaining kind (i8/i16/i32/i64/u8/u16/u32/u64/character) is a
    // plain integer at its own bit width — `bit_width` already special-
    // cases `character` to 32 (widened Unicode scalar) the same way
    // `bytecode::slot_value` does.
    return llvm::Type::getIntNTy(ctx, static_cast<unsigned>(bit_width(kind)));
  }
}

// ==========================================================================
//  module_compiler — declares every function's signature up front (so
//  direct/recursive/mutually-recursive calls always find a real
//  llvm::Function*), then compiles each body.
// ==========================================================================

class function_compiler {
public:
  function_compiler(
      llvm::LLVMContext &ctx, const type_table &types,
      const std::unordered_map<std::string, llvm::Function *> &functions,
      llvm::Function *panic_fn)
      : ctx_(ctx), types_(types), functions_(functions), panic_fn_(panic_fn),
        builder_(ctx) {}

  [[nodiscard]] auto compile(const hir::hir_function &fn,
                             llvm::Function *llvm_fn)
      -> std::expected<void, codegen_error> {
    current_fn_ = llvm_fn;
    return_is_unit_ = types_.is_unit(fn.return_type);
    if (!return_is_unit_) {
      auto kind = numeric_kind_for(fn.return_type, fn.span);
      if (!kind.has_value()) {
        return std::unexpected(kind.error());
      }
      return_llvm_type_ = llvm_type_for(ctx_, *kind);
    }

    auto *entry = llvm::BasicBlock::Create(ctx_, "entry", llvm_fn);
    builder_.SetInsertPoint(entry);
    // Every local (parameter or `let`) gets an alloca inserted here, at the
    // very front of the entry block, regardless of where in the function
    // body it's lexically declared — the usual "alloca marker" trick (also
    // how unoptimized Clang output is shaped): an alloca that executes
    // repeatedly (e.g. one lexically inside a `while` body) must not be
    // emitted at that repeated program point, or it dynamically grows the
    // stack once per loop iteration instead of naming one fixed frame slot.
    alloca_marker_ = builder_.CreateAlloca(llvm::Type::getInt1Ty(ctx_), nullptr,
                                           "alloca.marker");

    for (size_t i = 0; i < fn.params.size(); ++i) {
      const auto &param = fn.params[i];
      auto kind = numeric_kind_for(param.type, fn.span);
      if (!kind.has_value()) {
        return std::unexpected(kind.error());
      }
      auto *alloca =
          create_local_alloca(llvm_type_for(ctx_, *kind), param.name);
      builder_.CreateStore(llvm_fn->getArg(static_cast<unsigned>(i)), alloca);
      locals_.emplace(param.symbol, alloca);
    }

    const auto &stmts = fn.body->stmts;
    auto terminated = false;
    for (size_t i = 0; i + 1 < stmts.size() && !terminated; ++i) {
      auto result = compile_stmt(*stmts[i]);
      if (!result.has_value()) {
        return std::unexpected(result.error());
      }
      terminated = *result;
    }

    if (!terminated) {
      if (stmts.empty()) {
        builder_.CreateRetVoid();
      } else {
        const auto &last = *stmts.back();
        if (last.kind == hir_node_kind::hir_expr_stmt) {
          const auto &expr_stmt =
              dynamic_cast<const hir::hir_expr_stmt &>(last);
          if (return_is_unit_) {
            auto value = compile_expr(*expr_stmt.expr);
            if (!value.has_value()) {
              return std::unexpected(value.error());
            }
            builder_.CreateRetVoid();
          } else {
            auto value = compile_expr(*expr_stmt.expr);
            if (!value.has_value()) {
              return std::unexpected(value.error());
            }
            builder_.CreateRet(*value);
          }
        } else {
          auto result = compile_stmt(last);
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          if (!*result) {
            builder_.CreateRetVoid();
          }
        }
      }
    }

    alloca_marker_->eraseFromParent();
    return {};
  }

private:
  // ------------------------------------------------------------------
  //  Locals and types.
  // ------------------------------------------------------------------

  [[nodiscard]] auto create_local_alloca(llvm::Type *ty,
                                         const std::string &name)
      -> llvm::AllocaInst * {
    auto tmp = llvm::IRBuilder<>(alloca_marker_);
    return tmp.CreateAlloca(ty, nullptr, name);
  }

  [[nodiscard]] auto lookup_local(hir::symbol_id symbol) const
      -> llvm::AllocaInst * {
    const auto found = locals_.find(symbol);
    return found == locals_.end() ? nullptr : found->second;
  }

  [[nodiscard]] auto numeric_kind_for(type_id id, source_span span)
      -> std::expected<numeric_kind, codegen_error> {
    const auto kind = numeric_kind_of(types_, id);
    if (!kind.has_value()) {
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_type,
          .span = span,
          .message = std::format(
              "type `{}` has no scalar llvm_codegen representation yet — "
              "non-scalar types and 128-bit widths are not supported until "
              "spec/codegen-design.md increment 6 (heap types) lands",
              types_.display(id))});
    }
    return *kind;
  }

  // ------------------------------------------------------------------
  //  Panics — every checked-arithmetic guard funnels through this, so a
  //  panic always looks like: compute a bool "should panic" condition,
  //  branch to a block that calls the runtime and terminates with
  //  `unreachable`, otherwise fall through to a fresh block that carries on
  //  — mirroring `bytecode::vm`'s `throw panic_error(...)` sites exactly
  //  (same closed `panic_reason` set, same trigger conditions), just
  //  reified as control flow instead of a C++ exception.
  // ------------------------------------------------------------------

  auto guard_panic(llvm::Value *panic_if_true, panic_reason reason) -> void {
    auto *panic_bb = llvm::BasicBlock::Create(ctx_, "panic", current_fn_);
    auto *ok_bb = llvm::BasicBlock::Create(ctx_, "ok", current_fn_);
    builder_.CreateCondBr(panic_if_true, panic_bb, ok_bb);
    builder_.SetInsertPoint(panic_bb);
    builder_.CreateCall(panic_fn_,
                        {llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_),
                                                static_cast<uint8_t>(reason))});
    builder_.CreateUnreachable();
    builder_.SetInsertPoint(ok_bb);
  }

  // ------------------------------------------------------------------
  //  Expressions — return the computed SSA value directly (no destination-
  //  register threading the way `bytecode_compiler` needs; LLVM's own SSA
  //  form makes that unnecessary).
  // ------------------------------------------------------------------

  [[nodiscard]] auto compile_expr(const hir::hir_expr &expr)
      -> std::expected<llvm::Value *, codegen_error> {
    switch (expr.kind) {
    case hir_node_kind::hir_literal:
      return compile_literal(dynamic_cast<const hir::hir_literal &>(expr));
    case hir_node_kind::hir_local_ref: {
      const auto &ref = dynamic_cast<const hir::hir_local_ref &>(expr);
      auto *alloca = lookup_local(ref.symbol);
      if (alloca == nullptr) {
        return std::unexpected(codegen_error{
            .kind = codegen_error_kind::unsupported_construct,
            .span = expr.span,
            .message = std::format(
                "reference to `{}` is not a local binding — a bare function "
                "value used outside of call position is not supported yet",
                ref.name)});
      }
      return builder_.CreateLoad(alloca->getAllocatedType(), alloca, ref.name);
    }
    case hir_node_kind::hir_binary:
      return compile_binary(dynamic_cast<const hir::hir_binary &>(expr));
    case hir_node_kind::hir_unary:
      return compile_unary(dynamic_cast<const hir::hir_unary &>(expr));
    case hir_node_kind::hir_cast:
      return compile_cast(dynamic_cast<const hir::hir_cast &>(expr));
    case hir_node_kind::hir_call:
      return compile_call(dynamic_cast<const hir::hir_call &>(expr));
    case hir_node_kind::hir_if: {
      const auto &node = dynamic_cast<const hir::hir_if &>(expr);
      auto kind = numeric_kind_for(expr.type, expr.span);
      if (!kind.has_value()) {
        return std::unexpected(kind.error());
      }
      auto *result =
          create_local_alloca(llvm_type_for(ctx_, *kind), "if.result");
      auto terminated = compile_if(node, result);
      if (!terminated.has_value()) {
        return std::unexpected(terminated.error());
      }
      // Scope limitation, documented rather than silently assumed: this
      // spike does not thread "never" (all-branches-diverge) typing through
      // expression position the way `hir_if`-as-statement can. Every stress
      // corpus program keeps an if-expression's branches value-producing,
      // not diverging, so `result` is always written before this load runs.
      return builder_.CreateLoad(result->getAllocatedType(), result,
                                 "if.value");
    }
    default:
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = expr.span,
          .message =
              "this expression form is outside llvm_codegen's scalar/"
              "control-flow scope (spec/codegen-design.md increment 3) — "
              "match, tuples, structs, arrays, lambdas, field/index access, "
              "and sum-type variants all need a heap-value representation "
              "increment 6 hasn't landed yet"});
    }
  }

  [[nodiscard]] auto compile_literal(const hir::hir_literal &lit)
      -> std::expected<llvm::Value *, codegen_error> {
    auto kind = numeric_kind_for(lit.type, lit.span);
    if (!kind.has_value()) {
      return std::unexpected(kind.error());
    }
    auto *ty = llvm_type_for(ctx_, *kind);
    switch (lit.lit_kind) {
    case token_kind::kw_true:
      return llvm::ConstantInt::get(ty, 1);
    case token_kind::kw_false:
      return llvm::ConstantInt::get(ty, 0);
    case token_kind::int_lit: {
      const auto value = parse_uint_literal(lit.value);
      if (!value.has_value()) {
        return std::unexpected(codegen_error{
            .kind = codegen_error_kind::unsupported_construct,
            .span = lit.span,
            .message = std::format("could not parse integer literal `{}`",
                                   lit.value)});
      }
      if (is_float(*kind)) {
        return llvm::ConstantFP::get(ty, static_cast<double>(*value));
      }
      return llvm::ConstantInt::get(ty, *value);
    }
    case token_kind::float_lit: {
      const auto value = parse_float_literal(lit.value);
      if (!value.has_value()) {
        return std::unexpected(codegen_error{
            .kind = codegen_error_kind::unsupported_construct,
            .span = lit.span,
            .message =
                std::format("could not parse float literal `{}`", lit.value)});
      }
      return llvm::ConstantFP::get(ty, *value);
    }
    case token_kind::char_lit: {
      const auto value = decode_char_literal(lit.value);
      if (!value.has_value()) {
        return std::unexpected(codegen_error{
            .kind = codegen_error_kind::unsupported_construct,
            .span = lit.span,
            .message = std::format("could not decode character literal `{}`",
                                   lit.value)});
      }
      return llvm::ConstantInt::get(ty, *value);
    }
    default:
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = lit.span,
          .message = "string literals and other non-scalar literal kinds are "
                     "not supported yet (heap types land in "
                     "spec/codegen-design.md increment 6)"});
    }
  }

  [[nodiscard]] auto compile_binary(const hir::hir_binary &bin)
      -> std::expected<llvm::Value *, codegen_error> {
    if (bin.op == ast::binary_op::And || bin.op == ast::binary_op::Or) {
      return compile_short_circuit(bin);
    }

    auto kind = numeric_kind_for(bin.lhs->type, bin.span);
    if (!kind.has_value()) {
      return std::unexpected(kind.error());
    }
    auto lhs = compile_expr(*bin.lhs);
    if (!lhs.has_value()) {
      return std::unexpected(lhs.error());
    }
    auto rhs = compile_expr(*bin.rhs);
    if (!rhs.has_value()) {
      return std::unexpected(rhs.error());
    }
    return compile_binary_op(bin, *kind, *lhs, *rhs);
  }

  [[nodiscard]] auto compile_short_circuit(const hir::hir_binary &bin)
      -> std::expected<llvm::Value *, codegen_error> {
    auto *result =
        create_local_alloca(llvm::Type::getInt1Ty(ctx_), "sc.result");
    auto lhs = compile_expr(*bin.lhs);
    if (!lhs.has_value()) {
      return std::unexpected(lhs.error());
    }
    builder_.CreateStore(*lhs, result);

    auto *rhs_bb = llvm::BasicBlock::Create(ctx_, "sc.rhs", current_fn_);
    auto *merge_bb = llvm::BasicBlock::Create(ctx_, "sc.end", current_fn_);
    if (bin.op == ast::binary_op::And) {
      builder_.CreateCondBr(*lhs, rhs_bb, merge_bb);
    } else {
      builder_.CreateCondBr(*lhs, merge_bb, rhs_bb);
    }

    builder_.SetInsertPoint(rhs_bb);
    auto rhs = compile_expr(*bin.rhs);
    if (!rhs.has_value()) {
      return std::unexpected(rhs.error());
    }
    builder_.CreateStore(*rhs, result);
    builder_.CreateBr(merge_bb);

    builder_.SetInsertPoint(merge_bb);
    return builder_.CreateLoad(llvm::Type::getInt1Ty(ctx_), result, "sc.value");
  }

  [[nodiscard]] auto compile_binary_op(const hir::hir_binary &bin,
                                       numeric_kind kind, llvm::Value *lhs,
                                       llvm::Value *rhs)
      -> std::expected<llvm::Value *, codegen_error> {
    using ast::binary_op;
    const auto bits = bit_width(kind);
    const auto signed_kind = is_signed_integer(kind);

    switch (bin.op) {
    case binary_op::EqEq:
      return is_float(kind) ? builder_.CreateFCmpOEQ(lhs, rhs)
                            : builder_.CreateICmpEQ(lhs, rhs);
    case binary_op::BangEq:
      return is_float(kind) ? builder_.CreateFCmpONE(lhs, rhs)
                            : builder_.CreateICmpNE(lhs, rhs);
    case binary_op::Lt:
      if (is_float(kind)) {
        return builder_.CreateFCmpOLT(lhs, rhs);
      }
      return signed_kind ? builder_.CreateICmpSLT(lhs, rhs)
                         : builder_.CreateICmpULT(lhs, rhs);
    case binary_op::LtEq:
      if (is_float(kind)) {
        return builder_.CreateFCmpOLE(lhs, rhs);
      }
      return signed_kind ? builder_.CreateICmpSLE(lhs, rhs)
                         : builder_.CreateICmpULE(lhs, rhs);
    case binary_op::Gt:
      if (is_float(kind)) {
        return builder_.CreateFCmpOGT(lhs, rhs);
      }
      return signed_kind ? builder_.CreateICmpSGT(lhs, rhs)
                         : builder_.CreateICmpUGT(lhs, rhs);
    case binary_op::GtEq:
      if (is_float(kind)) {
        return builder_.CreateFCmpOGE(lhs, rhs);
      }
      return signed_kind ? builder_.CreateICmpSGE(lhs, rhs)
                         : builder_.CreateICmpUGE(lhs, rhs);
    case binary_op::Add:
      if (is_float(kind)) {
        return builder_.CreateFAdd(lhs, rhs);
      }
      return checked_arith(signed_kind ? llvm::Intrinsic::sadd_with_overflow
                                       : llvm::Intrinsic::uadd_with_overflow,
                           lhs, rhs);
    case binary_op::Sub:
      if (is_float(kind)) {
        return builder_.CreateFSub(lhs, rhs);
      }
      return checked_arith(signed_kind ? llvm::Intrinsic::ssub_with_overflow
                                       : llvm::Intrinsic::usub_with_overflow,
                           lhs, rhs);
    case binary_op::Mul:
      if (is_float(kind)) {
        return builder_.CreateFMul(lhs, rhs);
      }
      return checked_arith(signed_kind ? llvm::Intrinsic::smul_with_overflow
                                       : llvm::Intrinsic::umul_with_overflow,
                           lhs, rhs);
    case binary_op::Div:
      if (is_float(kind)) {
        return builder_.CreateFDiv(lhs, rhs);
      }
      return checked_div(signed_kind, bits, lhs, rhs, /*is_mod=*/false);
    case binary_op::Mod:
      if (is_float(kind)) {
        return builder_.CreateFRem(lhs, rhs);
      }
      return checked_div(signed_kind, bits, lhs, rhs, /*is_mod=*/true);
    case binary_op::AddWrap:
      return builder_.CreateAdd(lhs, rhs);
    case binary_op::SubWrap:
      return builder_.CreateSub(lhs, rhs);
    case binary_op::MulWrap:
      return builder_.CreateMul(lhs, rhs);
    case binary_op::AddSat:
      return builder_.CreateBinaryIntrinsic(
          signed_kind ? llvm::Intrinsic::sadd_sat : llvm::Intrinsic::uadd_sat,
          lhs, rhs);
    case binary_op::SubSat:
      return builder_.CreateBinaryIntrinsic(
          signed_kind ? llvm::Intrinsic::ssub_sat : llvm::Intrinsic::usub_sat,
          lhs, rhs);
    case binary_op::Shl:
    case binary_op::Shr:
      return checked_shift(bin.op == binary_op::Shl, signed_kind, bits, lhs,
                           rhs);
    case binary_op::BitAnd:
      return builder_.CreateAnd(lhs, rhs);
    case binary_op::BitOr:
      return builder_.CreateOr(lhs, rhs);
    case binary_op::BitXor:
      return builder_.CreateXor(lhs, rhs);
    default:
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = bin.span,
          .message = std::format("operator `{}` is not supported by "
                                 "llvm_codegen yet",
                                 ast::binary_op_name(bin.op))});
    }
  }

  [[nodiscard]] auto checked_arith(llvm::Intrinsic::ID id, llvm::Value *lhs,
                                   llvm::Value *rhs) -> llvm::Value * {
    auto *pair = builder_.CreateBinaryIntrinsic(id, lhs, rhs);
    auto *value = builder_.CreateExtractValue(pair, 0);
    auto *overflowed = builder_.CreateExtractValue(pair, 1);
    guard_panic(overflowed, panic_reason::integer_overflow);
    return value;
  }

  [[nodiscard]] auto checked_div(bool is_signed, int bits, llvm::Value *lhs,
                                 llvm::Value *rhs, bool is_mod)
      -> llvm::Value * {
    auto *ty = lhs->getType();
    auto *zero = llvm::ConstantInt::get(ty, 0);
    guard_panic(builder_.CreateICmpEQ(rhs, zero),
                panic_reason::integer_divide_by_zero);
    if (is_signed) {
      auto *min_value = llvm::ConstantInt::get(
          ty, llvm::APInt::getSignedMinValue(static_cast<unsigned>(bits)));
      auto *neg_one = llvm::ConstantInt::get(ty, static_cast<uint64_t>(-1),
                                             /*isSigned=*/true);
      auto *is_min = builder_.CreateICmpEQ(lhs, min_value);
      auto *is_neg_one = builder_.CreateICmpEQ(rhs, neg_one);
      guard_panic(builder_.CreateAnd(is_min, is_neg_one),
                  panic_reason::integer_overflow);
      return is_mod ? builder_.CreateSRem(lhs, rhs)
                    : builder_.CreateSDiv(lhs, rhs);
    }
    return is_mod ? builder_.CreateURem(lhs, rhs)
                  : builder_.CreateUDiv(lhs, rhs);
  }

  [[nodiscard]] auto checked_shift(bool is_shl, bool is_signed, int bits,
                                   llvm::Value *lhs, llvm::Value *rhs)
      -> llvm::Value * {
    auto *bits_const =
        llvm::ConstantInt::get(rhs->getType(), static_cast<uint64_t>(bits));
    guard_panic(builder_.CreateICmpUGE(rhs, bits_const),
                panic_reason::integer_overflow);
    if (is_shl) {
      return builder_.CreateShl(lhs, rhs);
    }
    return is_signed ? builder_.CreateAShr(lhs, rhs)
                     : builder_.CreateLShr(lhs, rhs);
  }

  [[nodiscard]] auto compile_unary(const hir::hir_unary &un)
      -> std::expected<llvm::Value *, codegen_error> {
    if (un.op == ast::unary_op::Not) {
      auto src = compile_expr(*un.operand);
      if (!src.has_value()) {
        return std::unexpected(src.error());
      }
      return builder_.CreateNot(*src);
    }
    if (un.op != ast::unary_op::Neg && un.op != ast::unary_op::BitNot) {
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = un.span,
          .message =
              std::format("operator `{}` needs a reference/pointer model "
                          "llvm_codegen doesn't have yet",
                          ast::unary_op_name(un.op))});
    }
    auto kind = numeric_kind_for(un.operand->type, un.span);
    if (!kind.has_value()) {
      return std::unexpected(kind.error());
    }
    auto src = compile_expr(*un.operand);
    if (!src.has_value()) {
      return std::unexpected(src.error());
    }
    if (un.op == ast::unary_op::BitNot) {
      return builder_.CreateNot(*src);
    }
    // Neg: float negates plainly; signed integer negation panics exactly
    // when negating the type's minimum value (mirrors `vm.cpp`'s
    // `checked_neg_signed` — the checker guarantees `Neg` never reaches an
    // unsigned operand, matching `exec_neg`'s own precondition comment).
    if (is_float(*kind)) {
      return builder_.CreateFNeg(*src);
    }
    const auto bits = bit_width(*kind);
    auto *min_value = llvm::ConstantInt::get(
        (*src)->getType(),
        llvm::APInt::getSignedMinValue(static_cast<unsigned>(bits)));
    guard_panic(builder_.CreateICmpEQ(*src, min_value),
                panic_reason::integer_overflow);
    return builder_.CreateNeg(*src);
  }

  [[nodiscard]] auto compile_cast(const hir::hir_cast &cast)
      -> std::expected<llvm::Value *, codegen_error> {
    auto from_kind = numeric_kind_for(cast.operand->type, cast.span);
    if (!from_kind.has_value()) {
      return std::unexpected(from_kind.error());
    }
    auto to_kind = numeric_kind_for(cast.type, cast.span);
    if (!to_kind.has_value()) {
      return std::unexpected(to_kind.error());
    }
    auto src = compile_expr(*cast.operand);
    if (!src.has_value()) {
      return std::unexpected(src.error());
    }
    auto *to_ty = llvm_type_for(ctx_, *to_kind);
    const auto from_float = is_float(*from_kind);
    const auto to_float = is_float(*to_kind);

    if (from_float && to_float) {
      return *to_kind == numeric_kind::f32 ? builder_.CreateFPTrunc(*src, to_ty)
                                           : builder_.CreateFPExt(*src, to_ty);
    }
    if (from_float) {
      return is_signed_integer(*to_kind) ? builder_.CreateFPToSI(*src, to_ty)
                                         : builder_.CreateFPToUI(*src, to_ty);
    }
    if (to_float) {
      return is_signed_integer(*from_kind) ? builder_.CreateSIToFP(*src, to_ty)
                                           : builder_.CreateUIToFP(*src, to_ty);
    }
    // Integer-to-integer: sign-extend if the *source* was signed, zero-
    // extend otherwise, then truncate to the destination width — matches
    // `bytecode::vm`'s `exec_cast` exactly.
    return is_signed_integer(*from_kind)
               ? builder_.CreateSExtOrTrunc(*src, to_ty)
               : builder_.CreateZExtOrTrunc(*src, to_ty);
  }

  [[nodiscard]] auto compile_call(const hir::hir_call &call)
      -> std::expected<llvm::Value *, codegen_error> {
    if (call.callee->kind != hir_node_kind::hir_local_ref) {
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = call.span,
          .message = "only direct calls to a named function are supported "
                     "yet — calling through a computed callee expression is "
                     "not"});
    }
    const auto &ref = dynamic_cast<const hir::hir_local_ref &>(*call.callee);
    if (lookup_local(ref.symbol) != nullptr) {
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = call.span,
          .message = std::format(
              "calling `{}`, a function value held in a local variable, is "
              "not supported yet — only direct calls to a named module "
              "function are",
              ref.name)});
    }
    const auto found = functions_.find(ref.name);
    if (found == functions_.end()) {
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unknown_callee,
          .span = call.span,
          .message = std::format("call to `{}` could not be resolved to a "
                                 "function in this compiled module",
                                 ref.name)});
    }
    auto args = std::vector<llvm::Value *>{};
    args.reserve(call.args.size());
    for (const auto &arg : call.args) {
      auto value = compile_expr(*arg);
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
      args.push_back(*value);
    }
    return builder_.CreateCall(found->second, args);
  }

  /// Compiles an `if`/`elif`/`else` chain. `want_result`, when non-null, is
  /// the alloca every reaching branch's tail value gets stored into before
  /// branching to the merge block — the same alloca+load merge strategy
  /// used everywhere else in this file instead of building PHI nodes by
  /// hand. Returns whether the merge point is unreachable (every branch
  /// terminated, e.g. via `return`) — the caller must not add a further
  /// unconditional terminator in that case, `compile_if` already added one.
  [[nodiscard]] auto compile_if(const hir::hir_if &node,
                                llvm::AllocaInst *want_result)
      -> std::expected<bool, codegen_error> {
    auto *merge_bb = llvm::BasicBlock::Create(ctx_, "if.end", current_fn_);
    auto any_reaches_merge = false;

    for (size_t i = 0; i < node.branches.size(); ++i) {
      const auto &branch = node.branches[i];
      auto cond = compile_expr(*branch.condition);
      if (!cond.has_value()) {
        return std::unexpected(cond.error());
      }
      auto *then_bb = llvm::BasicBlock::Create(ctx_, "if.then", current_fn_);
      const auto has_more =
          i + 1 < node.branches.size() || node.else_body != nullptr;
      auto *next_bb =
          has_more ? llvm::BasicBlock::Create(ctx_, "if.next", current_fn_)
                   : merge_bb;
      builder_.CreateCondBr(*cond, then_bb, next_bb);
      if (next_bb == merge_bb) {
        // No more conditions and no `else`: the condition's false edge is
        // an implicit "do nothing" arm that lands directly on `merge_bb`,
        // which is therefore reachable even if `then_bb` itself terminates.
        any_reaches_merge = true;
      }

      builder_.SetInsertPoint(then_bb);
      auto terminated = compile_block_as_value(*branch.body, want_result);
      if (!terminated.has_value()) {
        return std::unexpected(terminated.error());
      }
      if (!*terminated) {
        builder_.CreateBr(merge_bb);
        any_reaches_merge = true;
      }

      builder_.SetInsertPoint(next_bb);
      if (next_bb == merge_bb) {
        break;
      }
    }

    if (node.else_body != nullptr) {
      auto terminated = compile_block_as_value(*node.else_body, want_result);
      if (!terminated.has_value()) {
        return std::unexpected(terminated.error());
      }
      if (!*terminated) {
        builder_.CreateBr(merge_bb);
        any_reaches_merge = true;
      }
      builder_.SetInsertPoint(merge_bb);
    }

    if (!any_reaches_merge) {
      builder_.CreateUnreachable();
    }
    return !any_reaches_merge;
  }

  // ------------------------------------------------------------------
  //  Statements and blocks. Every statement/block compiler returns whether
  //  it left the current block terminated (a `ret` was emitted, or an `if`
  //  where every branch terminated) — callers stop compiling further
  //  sibling statements once true, since anything after is unreachable and
  //  LLVM basic blocks may not contain instructions past a terminator.
  // ------------------------------------------------------------------

  [[nodiscard]] auto compile_block_as_value(const hir::hir_block &block,
                                            llvm::AllocaInst *want_result)
      -> std::expected<bool, codegen_error> {
    const auto &stmts = block.stmts;
    for (size_t i = 0; i < stmts.size(); ++i) {
      const auto &stmt = *stmts[i];
      const auto is_last = i + 1 == stmts.size();
      if (is_last && want_result != nullptr &&
          stmt.kind == hir_node_kind::hir_expr_stmt) {
        const auto &expr_stmt = dynamic_cast<const hir::hir_expr_stmt &>(stmt);
        auto value = compile_expr(*expr_stmt.expr);
        if (!value.has_value()) {
          return std::unexpected(value.error());
        }
        builder_.CreateStore(*value, want_result);
        return false;
      }
      auto result = compile_stmt(stmt);
      if (!result.has_value()) {
        return std::unexpected(result.error());
      }
      if (*result) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] auto compile_stmt(const hir_node &node)
      -> std::expected<bool, codegen_error> {
    switch (node.kind) {
    case hir_node_kind::hir_let:
      return compile_let(dynamic_cast<const hir::hir_let &>(node));
    case hir_node_kind::hir_assign:
      return compile_assign(dynamic_cast<const hir::hir_assign &>(node));
    case hir_node_kind::hir_expr_stmt: {
      const auto &expr_stmt = dynamic_cast<const hir::hir_expr_stmt &>(node);
      auto result = compile_expr(*expr_stmt.expr);
      if (!result.has_value()) {
        return std::unexpected(result.error());
      }
      return false;
    }
    case hir_node_kind::hir_return: {
      const auto &ret = dynamic_cast<const hir::hir_return &>(node);
      if (ret.value == nullptr) {
        builder_.CreateRetVoid();
        return true;
      }
      auto value = compile_expr(*ret.value);
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
      if (return_is_unit_) {
        builder_.CreateRetVoid();
      } else {
        builder_.CreateRet(*value);
      }
      return true;
    }
    case hir_node_kind::hir_while:
      return compile_while(dynamic_cast<const hir::hir_while &>(node));
    case hir_node_kind::hir_if: {
      auto terminated =
          compile_if(dynamic_cast<const hir::hir_if &>(node), nullptr);
      if (!terminated.has_value()) {
        return std::unexpected(terminated.error());
      }
      return *terminated;
    }
    default:
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = node.span,
          .message =
              "this statement form is outside llvm_codegen's scalar/"
              "control-flow scope (spec/codegen-design.md increment 3) — "
              "match, while-let, destructuring let/else, and comprehension "
              "pushes all need a heap/aggregate representation increment 6 "
              "hasn't landed yet"});
    }
  }

  [[nodiscard]] auto compile_let(const hir::hir_let &let)
      -> std::expected<bool, codegen_error> {
    auto kind = numeric_kind_for(let.initializer->type, let.span);
    if (!kind.has_value()) {
      return std::unexpected(kind.error());
    }
    auto value = compile_expr(*let.initializer);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    auto *alloca = create_local_alloca(llvm_type_for(ctx_, *kind), let.name);
    builder_.CreateStore(*value, alloca);
    locals_.emplace(let.symbol, alloca);
    return false;
  }

  [[nodiscard]] auto compile_assign(const hir::hir_assign &assign)
      -> std::expected<bool, codegen_error> {
    if (assign.target->kind != hir_node_kind::hir_local_ref) {
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = assign.span,
          .message = "assigning to a field or index target needs a heap/"
                     "aggregate representation llvm_codegen doesn't have "
                     "yet — only assigning directly to a local variable is "
                     "supported"});
    }
    const auto &target =
        dynamic_cast<const hir::hir_local_ref &>(*assign.target);
    auto *alloca = lookup_local(target.symbol);
    if (alloca == nullptr) {
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = assign.span,
          .message = std::format("assignment target `{}` does not resolve "
                                 "to a local variable",
                                 target.name)});
    }
    if (assign.op == ast::assign_op::Assign) {
      auto value = compile_expr(*assign.value);
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
      builder_.CreateStore(*value, alloca);
      return false;
    }

    auto kind = numeric_kind_for(target.type, assign.span);
    if (!kind.has_value()) {
      return std::unexpected(kind.error());
    }
    auto rhs = compile_expr(*assign.value);
    if (!rhs.has_value()) {
      return std::unexpected(rhs.error());
    }
    auto *current =
        builder_.CreateLoad(alloca->getAllocatedType(), alloca, target.name);
    auto new_value = compound_assign_value(assign, *kind, current, *rhs);
    if (!new_value.has_value()) {
      return std::unexpected(new_value.error());
    }
    builder_.CreateStore(*new_value, alloca);
    return false;
  }

  [[nodiscard]] auto
  compound_assign_value(const hir::hir_assign &assign, numeric_kind kind,
                        llvm::Value *current, llvm::Value *rhs)
      -> std::expected<llvm::Value *, codegen_error> {
    using ast::assign_op;
    ast::binary_op equivalent{};
    switch (assign.op) {
    case assign_op::AddAssign:
      equivalent = ast::binary_op::Add;
      break;
    case assign_op::SubAssign:
      equivalent = ast::binary_op::Sub;
      break;
    case assign_op::MulAssign:
      equivalent = ast::binary_op::Mul;
      break;
    case assign_op::DivAssign:
      equivalent = ast::binary_op::Div;
      break;
    case assign_op::ModAssign:
      equivalent = ast::binary_op::Mod;
      break;
    case assign_op::AndAssign:
      equivalent = ast::binary_op::BitAnd;
      break;
    case assign_op::OrAssign:
      equivalent = ast::binary_op::BitOr;
      break;
    case assign_op::XorAssign:
      equivalent = ast::binary_op::BitXor;
      break;
    case assign_op::ShlAssign:
      equivalent = ast::binary_op::Shl;
      break;
    case assign_op::ShrAssign:
      equivalent = ast::binary_op::Shr;
      break;
    case assign_op::AddWrapAssign:
      equivalent = ast::binary_op::AddWrap;
      break;
    case assign_op::SubWrapAssign:
      equivalent = ast::binary_op::SubWrap;
      break;
    case assign_op::MulWrapAssign:
      equivalent = ast::binary_op::MulWrap;
      break;
    case assign_op::AddSatAssign:
      equivalent = ast::binary_op::AddSat;
      break;
    case assign_op::SubSatAssign:
      equivalent = ast::binary_op::SubSat;
      break;
    default:
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = assign.span,
          .message = "this compound assignment operator is not supported "
                     "by llvm_codegen yet"});
    }
    // `compile_binary_op` only reads `bin.op`/`bin.span` off the synthetic
    // node below — building one avoids a second copy of the same opcode
    // dispatch table `compile_binary_op` already has.
    auto synthetic = hir::hir_binary(assign.span, hir::k_unknown_type,
                                     equivalent, nullptr, nullptr);
    return compile_binary_op(synthetic, kind, current, rhs);
  }

  [[nodiscard]] auto compile_while(const hir::hir_while &loop)
      -> std::expected<bool, codegen_error> {
    auto *cond_bb = llvm::BasicBlock::Create(ctx_, "while.cond", current_fn_);
    auto *body_bb = llvm::BasicBlock::Create(ctx_, "while.body", current_fn_);
    auto *end_bb = llvm::BasicBlock::Create(ctx_, "while.end", current_fn_);

    builder_.CreateBr(cond_bb);
    builder_.SetInsertPoint(cond_bb);
    auto cond = compile_expr(*loop.condition);
    if (!cond.has_value()) {
      return std::unexpected(cond.error());
    }
    builder_.CreateCondBr(*cond, body_bb, end_bb);

    builder_.SetInsertPoint(body_bb);
    auto terminated = compile_block_as_value(*loop.body, nullptr);
    if (!terminated.has_value()) {
      return std::unexpected(terminated.error());
    }
    if (!*terminated) {
      builder_.CreateBr(cond_bb);
    }

    builder_.SetInsertPoint(end_bb);
    return false;
  }

  llvm::LLVMContext &ctx_;
  const type_table &types_;
  const std::unordered_map<std::string, llvm::Function *> &functions_;
  llvm::Function *panic_fn_;
  llvm::IRBuilder<> builder_;
  llvm::Function *current_fn_ = nullptr;
  llvm::AllocaInst *alloca_marker_ = nullptr;
  std::unordered_map<hir::symbol_id, llvm::AllocaInst *> locals_;
  bool return_is_unit_ = false;
  llvm::Type *return_llvm_type_ = nullptr;
};

} // namespace

auto compile_module(const hir::hir_module &module, const type_table &types)
    -> std::expected<compiled_module, codegen_error> {
  auto result = compiled_module{
      .context = std::make_unique<llvm::LLVMContext>(),
      .module = nullptr,
  };
  result.module =
      std::make_unique<llvm::Module>(module.module_name, *result.context);
  auto &ctx = *result.context;
  auto &llvm_module = *result.module;

  auto *panic_fn = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(ctx),
                              {llvm::Type::getInt8Ty(ctx)},
                              /*isVarArg=*/false),
      llvm::Function::ExternalLinkage, kPanicSymbolName, llvm_module);
  panic_fn->setDoesNotReturn();

  // Every function's signature is declared before any body is compiled, so
  // a call (forward-referenced, recursive, or mutually recursive) always
  // finds a real `llvm::Function*` — see codegen.h's doc comment.
  auto functions = std::unordered_map<std::string, llvm::Function *>{};
  functions.reserve(module.functions.size());
  for (const auto &fn : module.functions) {
    auto param_types = std::vector<llvm::Type *>{};
    param_types.reserve(fn->params.size());
    for (const auto &param : fn->params) {
      const auto kind = numeric_kind_of(types, param.type);
      if (!kind.has_value()) {
        return std::unexpected(codegen_error{
            .kind = codegen_error_kind::unsupported_type,
            .span = fn->span,
            .message =
                std::format("parameter `{}` of `{}` has no scalar llvm_codegen "
                            "representation yet",
                            param.name, fn->name)});
      }
      param_types.push_back(llvm_type_for(ctx, *kind));
    }

    llvm::Type *return_ty = nullptr;
    if (types.is_unit(fn->return_type)) {
      return_ty = llvm::Type::getVoidTy(ctx);
    } else {
      const auto kind = numeric_kind_of(types, fn->return_type);
      if (!kind.has_value()) {
        return std::unexpected(codegen_error{
            .kind = codegen_error_kind::unsupported_type,
            .span = fn->span,
            .message = std::format("return type of `{}` has no scalar "
                                   "llvm_codegen representation yet",
                                   fn->name)});
      }
      return_ty = llvm_type_for(ctx, *kind);
    }

    auto *fn_type = llvm::FunctionType::get(return_ty, param_types,
                                            /*isVarArg=*/false);
    auto *llvm_fn = llvm::Function::Create(
        fn_type, llvm::Function::ExternalLinkage, fn->name, llvm_module);
    functions.emplace(fn->name, llvm_fn);
  }

  for (const auto &fn : module.functions) {
    auto *llvm_fn = functions.at(fn->name);
    auto compiler = function_compiler(ctx, types, functions, panic_fn);
    auto compiled = compiler.compile(*fn, llvm_fn);
    if (!compiled.has_value()) {
      return std::unexpected(compiled.error());
    }
  }

  auto verify_message = std::string{};
  auto verify_stream = llvm::raw_string_ostream(verify_message);
  if (llvm::verifyModule(llvm_module, &verify_stream)) {
    return std::unexpected(codegen_error{
        .kind = codegen_error_kind::unsupported_construct,
        .span = module.span,
        .message =
            std::format("llvm_codegen produced a module that failed "
                        "verification (this is a codegen bug, not a source "
                        "error): {}",
                        verify_message)});
  }

  return result;
}

} // namespace kira::llvm_codegen
