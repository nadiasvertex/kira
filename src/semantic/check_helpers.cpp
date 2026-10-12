#include "check_helpers.h"

namespace cinder::semantic::detail {

// ==========================================================================
//  Small helpers
// ==========================================================================

/// How an operator is spelled in source — used to echo a predicate back to
/// the user in a proof diagnostic, so ``cannot prove `i < n` `` quotes the
/// condition the way they would have written it.
auto binary_op_spelling(ast::binary_op op) -> std::string_view {
  switch (op) {
  case ast::binary_op::eq_eq:
    return "==";
  case ast::binary_op::bang_eq:
    return "!=";
  case ast::binary_op::lt:
    return "<";
  case ast::binary_op::lt_eq:
    return "<=";
  case ast::binary_op::gt:
    return ">";
  case ast::binary_op::gt_eq:
    return ">=";
  case ast::binary_op::add:
    return "+";
  case ast::binary_op::sub:
    return "-";
  case ast::binary_op::mul:
    return "*";
  case ast::binary_op::div:
    return "/";
  case ast::binary_op::mod:
    return "%";
  case ast::binary_op::logical_and:
    return "and";
  case ast::binary_op::logical_or:
    return "or";
  default:
    return "?";
  }
}

/// Parses an integer literal spelling (handles `_`, `0x`, `0o`, `0b`).
/// Returns nullopt when the value does not fit in 64 bits.
auto parse_integer_literal(std::string_view text) -> std::optional<uint64_t> {
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
  // std::from_chars requires a raw begin/end pointer pair.
  const char *digits_end = digits.data() + digits.size();
  const auto result = std::from_chars(digits.data(), digits_end, value, base);
  if (result.ec != std::errc{} || result.ptr != digits_end) {
    return std::nullopt;
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,bugprone-suspicious-stringview-data-usage)
  return value;
}

/// A variant-constructor expression (`@some(x)`) parses to an ident whose
/// span still covers the leading `@`; that lets us tell it apart from an
/// ordinary identifier of the same spelling.
auto is_variant_ident(const ast::ident_expr &ident) -> bool {
  return ident.span.len() > ident.name.size();
}

/// The key `k_builtin_methods` files a receiver under, or empty when the
/// receiver is not a type builtin methods are answered for. `slice_mut`
/// shares `slice`'s entries — the two differ in mutability, not in what
/// inherent methods they carry.
auto builtin_method_owner(const type_entry &object) -> std::string_view {
  if (object.kind == type_kind::builtin_kind && object.name == "str") {
    return "str";
  }
  // A fixed `array[T, N]` is its own data block, so `as_ptr`/`as_mut_ptr`
  // are meaningful on it exactly as they are on a `list`/`slice` -- it is
  // the one indexable shape that is not `builtin_generic_kind`.
  if (object.kind == type_kind::array_kind) {
    return "array";
  }
  if (object.kind != type_kind::builtin_generic_kind) {
    return {};
  }
  if (object.name == "slice" || object.name == "slice_mut") {
    return "slice";
  }
  if (object.name == "cell" || object.name == "cell_mut") {
    // Unlike `slice`/`slice_mut`, `cell` and `cell_mut` do *not* share one
    // bucket: `set` is only ever valid on a `cell_mut` receiver, and folding
    // both into one owner would offer it on a read-only `cell` too.
    return object.name;
  }
  if (object.name == "option" || object.name == "result" ||
      object.name == "generator" || object.name == "uninit") {
    return object.name;
  }
  return {};
}

} // namespace cinder::semantic::detail
