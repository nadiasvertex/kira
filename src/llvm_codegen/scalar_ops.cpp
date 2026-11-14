#include "src/llvm_codegen/scalar_ops.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <format>
#include <optional>
#include <string>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#pragma clang diagnostic ignored "-Wshadow"
#include <llvm/ADT/APInt.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#pragma clang diagnostic pop

#include "src/parser/operators.h"
#include "src/util/integer_literal.h"

namespace cinder::llvm_codegen {

namespace {

using bytecode::bit_width;
using bytecode::is_float;
using bytecode::is_signed_integer;
using bytecode::numeric_kind;
using bytecode::panic_reason;

/// An integer literal's value as an `APInt` of `bits` bits — the only way
/// to build an `i128` constant wider than 64 bits. `nullopt` when the
/// spelling does not parse.
[[nodiscard]] auto parse_wide_literal(std::string_view text, unsigned bits)
    -> std::optional<llvm::APInt> {
  const auto wide = util::parse_integer_literal_wide(text);
  if (!wide.has_value()) {
    return std::nullopt;
  }
  const std::array<uint64_t, 2> words = {static_cast<uint64_t>(*wide),
                                         static_cast<uint64_t>(*wide >> 64)};
  return llvm::APInt(128, words).zextOrTrunc(bits);
}

/// The literal's value when it fits in 64 bits — every integer kind but
/// the two 128-bit ones.
[[nodiscard]] auto parse_uint_literal(std::string_view text)
    -> std::optional<uint64_t> {
  const auto wide = util::parse_integer_literal_wide(text);
  if (!wide.has_value() || *wide > ~uint64_t{0}) {
    return std::nullopt;
  }
  return static_cast<uint64_t>(*wide);
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
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,bugprone-suspicious-stringview-data-usage)
    const char *hex_end = hex.data() + hex.size();
    const auto result = std::from_chars(hex.data(), hex_end, value, 16);
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,bugprone-suspicious-stringview-data-usage)
    if (result.ec != std::errc{} || result.ptr != hex_end) {
      return std::nullopt;
    }
    return value;
  }
  default:
    return std::nullopt;
  }
}

[[nodiscard]] auto unparsed(source_span span, std::string_view what,
                            std::string_view text)
    -> std::unexpected<codegen_error> {
  return std::unexpected(codegen_error{
      .kind = codegen_error_kind::unsupported_construct,
      .span = span,
      .message = std::format("could not parse {} `{}`", what, text)});
}

} // namespace

auto llvm_type_for(llvm::LLVMContext &ctx, numeric_kind kind) -> llvm::Type * {
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
//  Panics
//
//  Every checked-arithmetic guard funnels through here: compute a bool
//  "should panic" condition, branch to a block that calls the runtime and
//  ends in `unreachable`, otherwise continue in a fresh block. The same
//  closed `panic_reason` set and the same trigger conditions as
//  `bytecode::vm`, reified as control flow instead of a C++ exception.
// ==========================================================================

auto scalar_builder::guard_panic(llvm::Value *panic_if_true, panic_reason reason)
    -> void {
  auto &ctx = builder_.getContext();
  auto *current_fn = builder_.GetInsertBlock()->getParent();
  auto *panic_bb = llvm::BasicBlock::Create(ctx, "panic", current_fn);
  auto *ok_bb = llvm::BasicBlock::Create(ctx, "ok", current_fn);
  builder_.CreateCondBr(panic_if_true, panic_bb, ok_bb);
  builder_.SetInsertPoint(panic_bb);
  builder_.CreateCall(panic_fn_,
                      {llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx),
                                              static_cast<uint8_t>(reason))});
  builder_.CreateUnreachable();
  builder_.SetInsertPoint(ok_bb);
}

// ==========================================================================
//  Literals
// ==========================================================================

auto scalar_builder::literal(token_kind lit_kind, std::string_view text,
                             source_span span, numeric_kind kind)
    -> std::expected<llvm::Value *, codegen_error> {
  auto *ty = llvm_type_for(builder_.getContext(), kind);
  switch (lit_kind) {
  case token_kind::kw_true:
    return llvm::ConstantInt::get(ty, 1);
  case token_kind::kw_false:
    return llvm::ConstantInt::get(ty, 0);
  case token_kind::int_lit: {
    if (bytecode::is_wide_integer(kind)) {
      const auto wide = parse_wide_literal(text, 128);
      if (!wide.has_value()) {
        return unparsed(span, "integer literal", text);
      }
      return llvm::ConstantInt::get(ty, *wide);
    }
    const auto parsed = parse_uint_literal(text);
    if (!parsed.has_value()) {
      return unparsed(span, "integer literal", text);
    }
    if (is_float(kind)) {
      return llvm::ConstantFP::get(ty, static_cast<double>(*parsed));
    }
    return llvm::ConstantInt::get(ty, *parsed);
  }
  case token_kind::float_lit: {
    const auto parsed = parse_float_literal(text);
    if (!parsed.has_value()) {
      return unparsed(span, "float literal", text);
    }
    return llvm::ConstantFP::get(ty, *parsed);
  }
  case token_kind::char_lit: {
    const auto parsed = decode_char_literal(text);
    if (!parsed.has_value()) {
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = span,
          .message =
              std::format("could not decode character literal `{}`", text)});
    }
    return llvm::ConstantInt::get(ty, *parsed);
  }
  default:
    return std::unexpected(codegen_error{
        .kind = codegen_error_kind::unsupported_construct,
        .span = span,
        .message = "string literals and other non-scalar literal kinds are "
                   "not supported yet (see spec/todo.md)"});
  }
}

auto scalar_builder::negated_literal(std::string_view text, source_span span,
                                     numeric_kind kind)
    -> std::expected<llvm::Value *, codegen_error> {
  auto *ty = llvm_type_for(builder_.getContext(), kind);
  if (bytecode::is_wide_integer(kind)) {
    const auto wide = parse_wide_literal(text, 128);
    if (!wide.has_value()) {
      return unparsed(span, "integer literal", text);
    }
    return llvm::ConstantInt::get(ty, -*wide);
  }
  const auto magnitude = parse_uint_literal(text);
  if (!magnitude.has_value()) {
    return unparsed(span, "integer literal", text);
  }
  return llvm::ConstantInt::get(ty, ~(*magnitude) + 1);
}

// ==========================================================================
//  Operators
// ==========================================================================

auto scalar_builder::binary(ast::binary_op op, numeric_kind kind,
                            llvm::Value *lhs, llvm::Value *rhs, source_span span)
    -> std::expected<llvm::Value *, codegen_error> {
  using ast::binary_op;
  const auto bits = bit_width(kind);
  const auto signed_kind = is_signed_integer(kind);

  switch (op) {
  case binary_op::eq_eq:
    return is_float(kind) ? builder_.CreateFCmpOEQ(lhs, rhs)
                          : builder_.CreateICmpEQ(lhs, rhs);
  case binary_op::bang_eq:
    return is_float(kind) ? builder_.CreateFCmpONE(lhs, rhs)
                          : builder_.CreateICmpNE(lhs, rhs);
  case binary_op::lt:
    if (is_float(kind)) {
      return builder_.CreateFCmpOLT(lhs, rhs);
    }
    return signed_kind ? builder_.CreateICmpSLT(lhs, rhs)
                       : builder_.CreateICmpULT(lhs, rhs);
  case binary_op::lt_eq:
    if (is_float(kind)) {
      return builder_.CreateFCmpOLE(lhs, rhs);
    }
    return signed_kind ? builder_.CreateICmpSLE(lhs, rhs)
                       : builder_.CreateICmpULE(lhs, rhs);
  case binary_op::gt:
    if (is_float(kind)) {
      return builder_.CreateFCmpOGT(lhs, rhs);
    }
    return signed_kind ? builder_.CreateICmpSGT(lhs, rhs)
                       : builder_.CreateICmpUGT(lhs, rhs);
  case binary_op::gt_eq:
    if (is_float(kind)) {
      return builder_.CreateFCmpOGE(lhs, rhs);
    }
    return signed_kind ? builder_.CreateICmpSGE(lhs, rhs)
                       : builder_.CreateICmpUGE(lhs, rhs);
  case binary_op::add:
    if (is_float(kind)) {
      return builder_.CreateFAdd(lhs, rhs);
    }
    return checked_arith(signed_kind ? llvm::Intrinsic::sadd_with_overflow
                                     : llvm::Intrinsic::uadd_with_overflow,
                         lhs, rhs);
  case binary_op::sub:
    if (is_float(kind)) {
      return builder_.CreateFSub(lhs, rhs);
    }
    return checked_arith(signed_kind ? llvm::Intrinsic::ssub_with_overflow
                                     : llvm::Intrinsic::usub_with_overflow,
                         lhs, rhs);
  case binary_op::mul:
    if (is_float(kind)) {
      return builder_.CreateFMul(lhs, rhs);
    }
    return checked_arith(signed_kind ? llvm::Intrinsic::smul_with_overflow
                                     : llvm::Intrinsic::umul_with_overflow,
                         lhs, rhs);
  case binary_op::div:
    if (is_float(kind)) {
      return builder_.CreateFDiv(lhs, rhs);
    }
    return checked_div(signed_kind, bits, lhs, rhs, /*is_mod=*/false);
  case binary_op::mod:
    if (is_float(kind)) {
      return builder_.CreateFRem(lhs, rhs);
    }
    return checked_div(signed_kind, bits, lhs, rhs, /*is_mod=*/true);
  case binary_op::add_wrap:
    return builder_.CreateAdd(lhs, rhs);
  case binary_op::sub_wrap:
    return builder_.CreateSub(lhs, rhs);
  case binary_op::mul_wrap:
    return builder_.CreateMul(lhs, rhs);
  case binary_op::add_sat:
    return builder_.CreateBinaryIntrinsic(
        signed_kind ? llvm::Intrinsic::sadd_sat : llvm::Intrinsic::uadd_sat,
        lhs, rhs);
  case binary_op::sub_sat:
    return builder_.CreateBinaryIntrinsic(
        signed_kind ? llvm::Intrinsic::ssub_sat : llvm::Intrinsic::usub_sat,
        lhs, rhs);
  case binary_op::mul_sat:
    return checked_mul_sat(signed_kind, bits, lhs, rhs);
  case binary_op::shl:
  case binary_op::shr:
    return checked_shift(op == binary_op::shl, signed_kind, bits, lhs, rhs);
  case binary_op::bit_and:
    return builder_.CreateAnd(lhs, rhs);
  case binary_op::bit_or:
    return builder_.CreateOr(lhs, rhs);
  case binary_op::bit_xor:
    return builder_.CreateXor(lhs, rhs);
  default:
    return std::unexpected(codegen_error{
        .kind = codegen_error_kind::unsupported_construct,
        .span = span,
        .message = std::format("operator `{}` is not supported by "
                               "llvm_codegen yet",
                               ast::binary_op_name(op))});
  }
}

auto scalar_builder::checked_arith(llvm::Intrinsic::ID id, llvm::Value *lhs,
                                   llvm::Value *rhs) -> llvm::Value * {
  auto *pair = builder_.CreateBinaryIntrinsic(id, lhs, rhs);
  // NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) - false positive:
  // the analyzer's path-sensitive trace bottoms out in LLVM's own
  // OperandTraits/User operand-accessor macros (vendored code we don't
  // control), not in anything this call site does.
  auto *value = builder_.CreateExtractValue(pair, 0);
  auto *overflowed = builder_.CreateExtractValue(pair, 1);
  guard_panic(overflowed, panic_reason::integer_overflow);
  return value;
}

auto scalar_builder::checked_div(bool is_signed, int bits, llvm::Value *lhs,
                                 llvm::Value *rhs, bool is_mod) -> llvm::Value * {
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
  return is_mod ? builder_.CreateURem(lhs, rhs) : builder_.CreateUDiv(lhs, rhs);
}

/// LLVM has no `smul.sat`/`umul.sat` intrinsic (unlike add/sub, which get
/// `sadd.sat`/`ssub.sat` directly) — computed from the overflow-checking
/// multiply, mirroring `sat_mul_signed`/`sat_mul_unsigned` in
/// `src/bytecode/vm.cpp`. On signed overflow the result saturates to the
/// type's max when the operands share a sign or to its min otherwise;
/// unsigned overflow always saturates to max.
auto scalar_builder::checked_mul_sat(bool is_signed, int bits, llvm::Value *lhs,
                                     llvm::Value *rhs) -> llvm::Value * {
  auto *ty = lhs->getType();
  const auto width = static_cast<unsigned>(bits);
  if (!is_signed) {
    auto *pair = builder_.CreateBinaryIntrinsic(
        llvm::Intrinsic::umul_with_overflow, lhs, rhs);
    auto *value = builder_.CreateExtractValue(pair, 0);
    auto *overflowed = builder_.CreateExtractValue(pair, 1);
    auto *max_value =
        llvm::ConstantInt::get(ty, llvm::APInt::getMaxValue(width));
    return builder_.CreateSelect(overflowed, max_value, value);
  }
  auto *pair = builder_.CreateBinaryIntrinsic(
      llvm::Intrinsic::smul_with_overflow, lhs, rhs);
  auto *value = builder_.CreateExtractValue(pair, 0);
  auto *overflowed = builder_.CreateExtractValue(pair, 1);
  auto *zero = llvm::ConstantInt::get(ty, 0);
  auto *min_value =
      llvm::ConstantInt::get(ty, llvm::APInt::getSignedMinValue(width));
  auto *max_value =
      llvm::ConstantInt::get(ty, llvm::APInt::getSignedMaxValue(width));
  auto *lhs_neg = builder_.CreateICmpSLT(lhs, zero);
  auto *rhs_neg = builder_.CreateICmpSLT(rhs, zero);
  auto *same_sign = builder_.CreateICmpEQ(lhs_neg, rhs_neg);
  auto *saturated = builder_.CreateSelect(same_sign, max_value, min_value);
  return builder_.CreateSelect(overflowed, saturated, value);
}

auto scalar_builder::checked_shift(bool is_shl, bool is_signed, int bits,
                                   llvm::Value *lhs, llvm::Value *rhs)
    -> llvm::Value * {
  auto *bits_const =
      llvm::ConstantInt::get(rhs->getType(), static_cast<uint64_t>(bits));
  guard_panic(builder_.CreateICmpUGE(rhs, bits_const),
              panic_reason::integer_overflow);
  if (is_shl) {
    return builder_.CreateShl(lhs, rhs);
  }
  return is_signed ? builder_.CreateAShr(lhs, rhs) : builder_.CreateLShr(lhs, rhs);
}

auto scalar_builder::negate(numeric_kind kind, llvm::Value *value)
    -> llvm::Value * {
  // Float negates plainly; signed integer negation panics exactly when
  // negating the type's minimum value (mirrors `vm.cpp`'s
  // `checked_neg_signed` — the checker guarantees `neg` never reaches an
  // unsigned operand).
  if (is_float(kind)) {
    return builder_.CreateFNeg(value);
  }
  auto *min_value = llvm::ConstantInt::get(
      value->getType(),
      llvm::APInt::getSignedMinValue(static_cast<unsigned>(bit_width(kind))));
  guard_panic(builder_.CreateICmpEQ(value, min_value),
              panic_reason::integer_overflow);
  return builder_.CreateNeg(value);
}

auto scalar_builder::cast(numeric_kind from, numeric_kind to, llvm::Value *value)
    -> llvm::Value * {
  auto *to_ty = llvm_type_for(builder_.getContext(), to);
  const auto from_float = is_float(from);
  const auto to_float = is_float(to);

  if (from_float && to_float) {
    return to == numeric_kind::f32 ? builder_.CreateFPTrunc(value, to_ty)
                                   : builder_.CreateFPExt(value, to_ty);
  }
  if (from_float) {
    return is_signed_integer(to) ? builder_.CreateFPToSI(value, to_ty)
                                 : builder_.CreateFPToUI(value, to_ty);
  }
  if (to_float) {
    return is_signed_integer(from) ? builder_.CreateSIToFP(value, to_ty)
                                   : builder_.CreateUIToFP(value, to_ty);
  }
  // Integer-to-integer: sign-extend if the *source* was signed, zero-extend
  // otherwise, then truncate to the destination width — matches
  // `bytecode::vm`'s `exec_cast` exactly.
  return is_signed_integer(from) ? builder_.CreateSExtOrTrunc(value, to_ty)
                                 : builder_.CreateZExtOrTrunc(value, to_ty);
}

} // namespace cinder::llvm_codegen
