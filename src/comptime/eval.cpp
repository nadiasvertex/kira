#include "src/comptime/eval.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string>

#include "src/parser/text_escape.h"

namespace kira::comptime {
namespace {

/// Parses an integer literal spelling (handles `_`, `0x`, `0o`, `0b`) —
/// deliberately a small local copy of `semantic::parse_integer_literal`'s
/// approach rather than a shared dependency: `src/semantic/check.cpp`'s
/// version lives in that file's anonymous namespace and isn't reusable
/// across translation units without its own header, which isn't worth
/// introducing for one helper at this milestone's scope.
auto parse_integer_literal(std::string_view text) -> std::optional<int64_t> {
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
  auto result_value = int64_t{0};
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,bugprone-suspicious-stringview-data-usage)
  const char *digits_end = digits.data() + digits.size();
  const auto result =
      std::from_chars(digits.data(), digits_end, result_value, base);
  if (result.ec != std::errc{} || result.ptr != digits_end) {
    return std::nullopt;
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,bugprone-suspicious-stringview-data-usage)
  return result_value;
}

/// Parses a float literal spelling (handles `_`); `std::from_chars` for
/// `double` isn't available on every supported standard library yet, so
/// this goes through `std::strtod` on a NUL-terminated cleaned copy.
auto parse_float_literal(std::string_view text) -> std::optional<double> {
  auto cleaned = std::string{};
  cleaned.reserve(text.size());
  for (const auto ch : text) {
    if (ch != '_') {
      cleaned.push_back(ch);
    }
  }
  char *end = nullptr;
  errno = 0;
  const auto result = std::strtod(cleaned.c_str(), &end);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  if (end != cleaned.c_str() + cleaned.size() || errno == ERANGE) {
    return std::nullopt;
  }
  return result;
}

/// Promotes an integer/float value pair to a common numeric representation
/// for arithmetic/comparison, mirroring the type checker's own numeric
/// unification: if either side is floating, both become `double`.
struct numeric_pair {
  bool is_float = false;
  double lf = 0.0;
  double rf = 0.0;
  int64_t li = 0;
  int64_t ri = 0;
};

} // namespace

void evaluator::bind_global(const std::string &name, value v) {
  globals_.insert_or_assign(name, std::move(v));
}

auto evaluator::report(source_span span, std::string message) -> value {
  diag_.emit(diagnostic(diagnostic_level::error, std::move(message), file_id_)
                 .with_label(span, "here"));
  return value::make_error();
}

auto evaluator::eval_literal(const ast::literal_expr &lit) -> value {
  switch (lit.lit_kind) {
  case token_kind::int_lit: {
    const auto parsed = parse_integer_literal(lit.value);
    if (!parsed.has_value()) {
      return report(lit.span, std::format("integer literal `{}` does not "
                                          "fit in a compile-time value",
                                          lit.value));
    }
    return value::make_int(*parsed);
  }
  case token_kind::float_lit: {
    const auto parsed = parse_float_literal(lit.value);
    if (!parsed.has_value()) {
      return report(lit.span, std::format("float literal `{}` could not be "
                                          "evaluated at compile time",
                                          lit.value));
    }
    return value::make_float(*parsed);
  }
  case token_kind::string_lit: {
    auto decoded = decode_string_literal(lit.value);
    if (!decoded.has_value()) {
      return report(lit.span, "string literal has an invalid escape "
                              "sequence");
    }
    return value::make_string(std::move(*decoded));
  }
  case token_kind::char_lit: {
    const auto decoded = decode_char_literal(lit.value);
    if (!decoded.has_value()) {
      return report(lit.span, "character literal has an invalid escape "
                              "sequence");
    }
    return value::make_int(static_cast<int64_t>(*decoded));
  }
  case token_kind::kw_true:
    return value::make_bool(true);
  case token_kind::kw_false:
    return value::make_bool(false);
  case token_kind::kw_unit:
    return value::make_unit();
  default:
    return report(lit.span,
                  "this literal form is not yet supported in compile-time "
                  "evaluation");
  }
}

auto evaluator::eval_ident(const ast::ident_expr &ident) -> value {
  if (const auto it = globals_.find(ident.name); it != globals_.end()) {
    return it->second;
  }
  return report(ident.span,
                std::format("`{}` is not a compile-time constant that has "
                            "been evaluated yet",
                            ident.name));
}

auto evaluator::eval_unary(const ast::unary_expr &un) -> value {
  if (un.operand == nullptr) {
    return value::make_error();
  }
  auto operand = evaluate(*un.operand);
  if (operand.is_error()) {
    return operand;
  }
  switch (un.op) {
  case ast::unary_op::neg:
    if (operand.kind == value_kind::integer) {
      return value::make_int(-operand.integer);
    }
    if (operand.kind == value_kind::floating) {
      return value::make_float(-operand.floating);
    }
    return report(un.span, "`-` requires a compile-time numeric operand");
  case ast::unary_op::logical_not:
    if (operand.kind == value_kind::boolean) {
      return value::make_bool(!operand.boolean);
    }
    return report(un.span, "`not` requires a compile-time `bool` operand");
  case ast::unary_op::bit_not:
  case ast::unary_op::deref:
  case ast::unary_op::addr_of:
  case ast::unary_op::addr_of_mut:
    return report(un.span, "this unary operator is not yet supported in "
                           "compile-time evaluation");
  }
  return value::make_error();
}

auto evaluator::eval_binary(const ast::binary_expr &bin) -> value {
  if (bin.lhs == nullptr || bin.rhs == nullptr) {
    return value::make_error();
  }

  // Logical operators short-circuit, so the right operand is only
  // evaluated when it can actually affect the result.
  if (bin.op == ast::binary_op::logical_and ||
      bin.op == ast::binary_op::logical_or) {
    auto lhs = evaluate(*bin.lhs);
    if (lhs.is_error()) {
      return lhs;
    }
    if (lhs.kind != value_kind::boolean) {
      return report(bin.span, "`and`/`or` require compile-time `bool` "
                              "operands");
    }
    if (bin.op == ast::binary_op::logical_and && !lhs.boolean) {
      return value::make_bool(false);
    }
    if (bin.op == ast::binary_op::logical_or && lhs.boolean) {
      return value::make_bool(true);
    }
    auto rhs = evaluate(*bin.rhs);
    if (rhs.is_error()) {
      return rhs;
    }
    if (rhs.kind != value_kind::boolean) {
      return report(bin.span, "`and`/`or` require compile-time `bool` "
                              "operands");
    }
    return rhs;
  }

  auto lhs = evaluate(*bin.lhs);
  if (lhs.is_error()) {
    return lhs;
  }
  auto rhs = evaluate(*bin.rhs);
  if (rhs.is_error()) {
    return rhs;
  }

  // Equality is defined for strings too, independent of the numeric path
  // below.
  if (bin.op == ast::binary_op::eq_eq || bin.op == ast::binary_op::bang_eq) {
    if (lhs.kind == value_kind::string && rhs.kind == value_kind::string) {
      const auto equal = lhs.string == rhs.string;
      return value::make_bool(bin.op == ast::binary_op::eq_eq ? equal : !equal);
    }
    if (lhs.kind == value_kind::boolean && rhs.kind == value_kind::boolean) {
      const auto equal = lhs.boolean == rhs.boolean;
      return value::make_bool(bin.op == ast::binary_op::eq_eq ? equal : !equal);
    }
  }

  const auto lhs_numeric =
      lhs.kind == value_kind::integer || lhs.kind == value_kind::floating;
  const auto rhs_numeric =
      rhs.kind == value_kind::integer || rhs.kind == value_kind::floating;
  if (!lhs_numeric || !rhs_numeric) {
    return report(bin.span, "this operator requires compile-time numeric "
                            "operands");
  }

  auto pair = numeric_pair{};
  pair.is_float =
      lhs.kind == value_kind::floating || rhs.kind == value_kind::floating;
  if (pair.is_float) {
    pair.lf = lhs.kind == value_kind::floating
                  ? lhs.floating
                  : static_cast<double>(lhs.integer);
    pair.rf = rhs.kind == value_kind::floating
                  ? rhs.floating
                  : static_cast<double>(rhs.integer);
  } else {
    pair.li = lhs.integer;
    pair.ri = rhs.integer;
  }

  switch (bin.op) {
  case ast::binary_op::add:
    return pair.is_float ? value::make_float(pair.lf + pair.rf)
                         : value::make_int(pair.li + pair.ri);
  case ast::binary_op::sub:
    return pair.is_float ? value::make_float(pair.lf - pair.rf)
                         : value::make_int(pair.li - pair.ri);
  case ast::binary_op::mul:
    return pair.is_float ? value::make_float(pair.lf * pair.rf)
                         : value::make_int(pair.li * pair.ri);
  case ast::binary_op::div:
    if (pair.is_float) {
      return value::make_float(pair.lf / pair.rf);
    }
    if (pair.ri == 0) {
      return report(bin.span, "division by zero in compile-time evaluation");
    }
    return value::make_int(pair.li / pair.ri);
  case ast::binary_op::mod:
    if (pair.is_float) {
      return value::make_float(std::fmod(pair.lf, pair.rf));
    }
    if (pair.ri == 0) {
      return report(bin.span, "division by zero in compile-time evaluation");
    }
    return value::make_int(pair.li % pair.ri);
  case ast::binary_op::eq_eq:
    return value::make_bool(pair.is_float ? pair.lf == pair.rf
                                          : pair.li == pair.ri);
  case ast::binary_op::bang_eq:
    return value::make_bool(pair.is_float ? pair.lf != pair.rf
                                          : pair.li != pair.ri);
  case ast::binary_op::lt:
    return value::make_bool(pair.is_float ? pair.lf < pair.rf
                                          : pair.li < pair.ri);
  case ast::binary_op::lt_eq:
    return value::make_bool(pair.is_float ? pair.lf <= pair.rf
                                          : pair.li <= pair.ri);
  case ast::binary_op::gt:
    return value::make_bool(pair.is_float ? pair.lf > pair.rf
                                          : pair.li > pair.ri);
  case ast::binary_op::gt_eq:
    return value::make_bool(pair.is_float ? pair.lf >= pair.rf
                                          : pair.li >= pair.ri);
  default:
    return report(bin.span, "this operator is not yet supported in "
                            "compile-time evaluation");
  }
}

auto evaluator::evaluate(const ast::expr &expr) -> value {
  if (expr.has_error) {
    return value::make_error();
  }
  switch (expr.kind) {
  case ast::node_kind::literal_expr:
    return eval_literal(dynamic_cast<const ast::literal_expr &>(expr));
  case ast::node_kind::ident_expr:
    return eval_ident(dynamic_cast<const ast::ident_expr &>(expr));
  case ast::node_kind::binary_expr:
    return eval_binary(dynamic_cast<const ast::binary_expr &>(expr));
  case ast::node_kind::unary_expr:
    return eval_unary(dynamic_cast<const ast::unary_expr &>(expr));
  case ast::node_kind::group_expr: {
    const auto &group = dynamic_cast<const ast::group_expr &>(expr);
    return group.inner != nullptr ? evaluate(*group.inner)
                                  : value::make_error();
  }
  default:
    return report(expr.span, "this expression form is not yet supported in "
                             "compile-time evaluation");
  }
}

} // namespace kira::comptime
