#include "src/bytecode_compiler/scalar_encoding.h"

#include <bit>
#include <charconv>
#include <cstdint>
#include <format>
#include <string>
#include <system_error>

#include "src/parser/operators.h"
#include "src/parser/text_escape.h"
#include "src/util/integer_literal.h"

namespace cinder::bytecode_compiler {

using bytecode::numeric_kind;
using bytecode::opcode;
using bytecode::slot_value;
using cinder::decode_char_literal;

namespace {

/// The literal's value when it fits in 64 bits — every integer kind but
/// the two 128-bit ones, which use `util::parse_integer_literal_wide`
/// directly.
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

[[nodiscard]] auto encode_f32(float v) -> slot_value {
  return slot_value{static_cast<uint64_t>(std::bit_cast<uint32_t>(v))};
}

} // namespace

/// Encodes one `hir_literal` as the `slot_value` its resolved `numeric_kind`
/// expects. Trusts the checker's own work (`check_integer_fit`,
/// `infer_literal`'s defaulting) rather than re-validating fit or
/// defaulting rules here — this only has to reproduce the *bit pattern*,
/// not re-derive which type the literal ended up with.
[[nodiscard]] auto encode_literal(token_kind lit_kind, std::string_view value,
                                  source_span span, numeric_kind kind)
    -> std::expected<slot_value, compile_error> {
  switch (lit_kind) {
  case token_kind::kw_true:
    return slot_value{uint64_t{1}};
  case token_kind::kw_false:
    return slot_value{uint64_t{0}};
  case token_kind::int_lit: {
    if (bytecode::is_wide_integer(kind)) {
      // The checker already confirmed the value fits; a negative `int128`
      // literal is this magnitude with `op_neg` applied afterward.
      const auto wide = util::parse_integer_literal_wide(value);
      if (!wide.has_value()) {
        return std::unexpected(compile_error{
            .kind = compile_error_kind::unsupported_construct,
            .span = span,
            .message =
                std::format("could not parse integer literal `{}`", value)});
      }
      return slot_value{*wide};
    }
    const auto parsed = parse_uint_literal(value);
    if (!parsed.has_value()) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = span,
          .message =
              std::format("could not parse integer literal `{}`", value)});
    }
    if (bytecode::is_float(kind)) {
      const auto as_double = static_cast<double>(*parsed);
      return kind == numeric_kind::f32
                 ? encode_f32(static_cast<float>(as_double))
                 : slot_value{as_double};
    }
    return slot_value{*parsed};
  }
  case token_kind::float_lit: {
    const auto parsed = parse_float_literal(value);
    if (!parsed.has_value()) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = span,
          .message = std::format("could not parse float literal `{}`", value)});
    }
    return kind == numeric_kind::f32 ? encode_f32(static_cast<float>(*parsed))
                                     : slot_value{*parsed};
  }
  case token_kind::char_lit: {
    const auto parsed = decode_char_literal(value);
    if (!parsed.has_value()) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = span,
          .message =
              std::format("could not decode character literal `{}`", value)});
    }
    return slot_value{static_cast<uint64_t>(*parsed)};
  }
  default:
    return std::unexpected(compile_error{
        .kind = compile_error_kind::unsupported_construct,
        .span = span,
        .message = "string literals and other non-scalar literal kinds are not "
                   "supported by the bytecode VM yet (see spec/todo.md)"});
  }
}

auto negate_literal(slot_value magnitude, numeric_kind kind) -> slot_value {
  if (bytecode::is_wide_integer(kind)) {
    return slot_value{~magnitude.w + 1};
  }
  const auto bits = bytecode::bit_width(kind);
  uint64_t negated = ~magnitude.u + 1;
  if (bits < 64) {
    // Every other slot_value in this VM stores a signed integer sign-extended
    // to the full 64-bit register width (see `store_signed`/`load_signed` in
    // vm.cpp); masking to `bits` alone would leave a positive-looking value
    // for any reader that treats the slot as a plain `int64_t`.
    const uint64_t mask = (uint64_t{1} << bits) - 1;
    negated &= mask;
    const uint64_t sign_bit = uint64_t{1} << (bits - 1);
    if ((negated & sign_bit) != 0) {
      negated |= ~mask;
    }
  }
  return slot_value{negated};
}

// ==========================================================================
//  Operator -> opcode tables.
// ==========================================================================

/// Ordinary (non-short-circuit) binary opcode for `op`, or `nullopt` for
/// `Pipe`/`And`/`Or`/`In`/`NotIn`/`Range`/`RangeInclusive` — `And`/`Or` are
/// compiled specially (short-circuit jumps, see `compile_expr_into`), and
/// the rest have no bytecode representation
/// (membership tests need a container, ranges are pattern-only surface
/// syntax with no expression-position `hir_binary` shape to reach here).
[[nodiscard]] auto binary_opcode_for(ast::binary_op op)
    -> std::optional<opcode> {
  switch (op) {
  case ast::binary_op::eq_eq:
    return opcode::op_eq;
  case ast::binary_op::bang_eq:
    return opcode::op_ne;
  case ast::binary_op::lt:
    return opcode::op_lt;
  case ast::binary_op::lt_eq:
    return opcode::op_le;
  case ast::binary_op::gt:
    return opcode::op_gt;
  case ast::binary_op::gt_eq:
    return opcode::op_ge;
  case ast::binary_op::add:
    return opcode::op_add;
  case ast::binary_op::sub:
    return opcode::op_sub;
  case ast::binary_op::mul:
    return opcode::op_mul;
  case ast::binary_op::div:
    return opcode::op_div;
  case ast::binary_op::mod:
    return opcode::op_mod;
  case ast::binary_op::add_wrap:
    return opcode::op_add_wrap;
  case ast::binary_op::sub_wrap:
    return opcode::op_sub_wrap;
  case ast::binary_op::mul_wrap:
    return opcode::op_mul_wrap;
  case ast::binary_op::add_sat:
    return opcode::op_add_sat;
  case ast::binary_op::sub_sat:
    return opcode::op_sub_sat;
  case ast::binary_op::mul_sat:
    return opcode::op_mul_sat;
  case ast::binary_op::shl:
    return opcode::op_shl;
  case ast::binary_op::shr:
    return opcode::op_shr;
  case ast::binary_op::bit_and:
    return opcode::op_bitand;
  case ast::binary_op::bit_or:
    return opcode::op_bitor;
  case ast::binary_op::bit_xor:
    return opcode::op_bitxor;
  default:
    return std::nullopt;
  }
}

/// The plain binary opcode a compound assignment op (`x += y`, ...)
/// desugars to (`Assign` itself has none — handled separately).
[[nodiscard]] auto compound_assign_opcode_for(ast::assign_op op)
    -> std::optional<opcode> {
  const auto binary = ast::compound_assign_op(op);
  if (!binary.has_value()) {
    return std::nullopt;
  }
  return binary_opcode_for(*binary);
}

} // namespace cinder::bytecode_compiler
