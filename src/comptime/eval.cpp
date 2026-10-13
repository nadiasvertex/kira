#include "src/comptime/eval.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <format>
#include <limits>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "src/comptime/hygiene.h"
#include "src/parser/ast_traversal.h"
#include "src/parser/operators.h"
#include "src/parser/text_escape.h"
#include "src/runtime/string_ops.h"
#include "src/utf8/utf8.h"

namespace cinder::comptime {
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
  // Parsed as the unsigned bit pattern rather than a signed `int64_t`
  // directly: a literal like `18446744073709551615` (`uint64::max`) doesn't
  // fit `int64_t`'s range even though its 64-bit representation is exactly
  // the all-ones pattern `eval_cast`'s masking/sign-extension expects to
  // find in `value::integer`. Reinterpreting those bits as `int64_t` (rather
  // than rejecting the literal as out of range) keeps this consistent with
  // how every other 64-bit-wide value already flows through `value::integer`
  // here.
  auto result_value = std::uint64_t{0};
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,bugprone-suspicious-stringview-data-usage)
  const char *digits_end = digits.data() + digits.size();
  const auto result =
      std::from_chars(digits.data(), digits_end, result_value, base);
  if (result.ec != std::errc{} || result.ptr != digits_end) {
    return std::nullopt;
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,bugprone-suspicious-stringview-data-usage)
  return static_cast<int64_t>(result_value);
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

/// Whether `e` is a direct integer literal, or one wrapped in a single
/// cast (`<lit> as T`, since `as` binds tighter than unary `-` — see
/// `checker::infer_unary`'s matching structural check). Used by
/// `evaluator::eval_unary`'s `neg` case to tell a literal whose *positive*
/// magnitude is exactly one past `int64::max` (and therefore reinterprets
/// to `int64::min`'s bit pattern in `parse_integer_literal` above) apart
/// from an ordinary computed value that happens to equal `int64::min` —
/// only the latter is a genuine double-negation overflow.
auto is_int_literal_operand(const ast::expr &e) -> bool {
  if (e.kind == ast::node_kind::literal_expr) {
    return ast::cast<const ast::literal_expr>(e).lit_kind ==
           token_kind::int_lit;
  }
  if (e.kind == ast::node_kind::cast_expr) {
    const auto &cast = ast::cast<const ast::cast_expr>(e);
    return cast.operand != nullptr &&
           cast.operand->kind == ast::node_kind::literal_expr &&
           ast::cast<const ast::literal_expr>(*cast.operand).lit_kind ==
               token_kind::int_lit;
  }
  return false;
}

/// Re-encodes a decoded string value back into a double-quoted source
/// spelling suitable for `ast::literal_expr::value` (later re-decoded by
/// `decode_string_literal` in `eval_literal`, exactly like any ordinary
/// parsed string literal) — the inverse of `decode_string_literal`. Only
/// backslash/quote/newline/tab/carriage-return need escaping for values
/// that originate from compile-time string data (e.g. a reflected field
/// name); anything else is passed through byte-for-byte.
auto encode_string_literal(std::string_view text) -> std::string {
  auto encoded = std::string{"\""};
  encoded.reserve(text.size() + 2);
  for (const auto ch : text) {
    switch (ch) {
    case '\\':
      encoded += "\\\\";
      break;
    case '"':
      encoded += "\\\"";
      break;
    case '\n':
      encoded += "\\n";
      break;
    case '\t':
      encoded += "\\t";
      break;
    case '\r':
      encoded += "\\r";
      break;
    case '{':
      // `decode_string_body` (`text_escape.cpp`) treats a lone `{` as the
      // start of a leftover interpolation region and a doubled `{{` as an
      // escaped literal brace — this value never passed through the
      // lexer's own interpolation splitting, so it must double any brace
      // itself to read back as a literal one.
      encoded += "{{";
      break;
    case '}':
      encoded += "}}";
      break;
    default:
      encoded.push_back(ch);
      break;
    }
  }
  encoded.push_back('"');
  return encoded;
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

/// Structural equality for `==`/`!=` on `variant_instance` values: same sum
/// type, same tag, and (recursively, since a payload value can itself be a
/// nested variant, list, etc.) equal payloads element-for-element. Kinds
/// this evaluator has no other equality rule for (`struct_instance`,
/// `closure`, quote fragments, ...) compare unequal rather than reporting —
/// callers only reach this once at least one side is known to be a variant,
/// so a payload of an unsupported kind is a real "can't compare this"
/// answer, not a diagnostic-worthy evaluator bug.
auto variant_values_equal(const value &lhs, const value &rhs) -> bool {
  if (lhs.kind != rhs.kind) {
    return false;
  }
  switch (lhs.kind) {
  case value_kind::unit:
    return true;
  case value_kind::boolean:
    return lhs.boolean == rhs.boolean;
  case value_kind::integer:
    return lhs.integer == rhs.integer;
  case value_kind::floating:
    return lhs.floating == rhs.floating;
  case value_kind::string:
    return lhs.string == rhs.string;
  case value_kind::variant_instance:
    if (lhs.type_name != rhs.type_name || lhs.variant_tag != rhs.variant_tag ||
        lhs.elements.size() != rhs.elements.size()) {
      return false;
    }
    for (std::size_t i = 0; i < lhs.elements.size(); ++i) {
      if (!variant_values_equal(lhs.elements[i], rhs.elements[i])) {
        return false;
      }
    }
    return true;
  default:
    return false;
  }
}

} // namespace

namespace {
constexpr int k_max_call_depth = 256;
} // namespace

void evaluator::bind_global(const std::string &name, value v) {
  globals_.insert_or_assign(name, std::move(v));
}

void evaluator::register_pending_static(std::string name,
                                        const ast::expr &initializer,
                                        file_id_type owner_file) {
  pending_statics_.insert_or_assign(
      std::move(name),
      pending_static{.initializer = &initializer, .owner_file = owner_file});
}

void evaluator::register_pending_function(std::string name,
                                          const ast::func_decl &decl) {
  pending_functions_.insert_or_assign(std::move(name), &decl);
}

void evaluator::push_locals(std::unordered_map<std::string, value> scope) {
  locals_.push_back(std::move(scope));
}

void evaluator::pop_locals() { locals_.pop_back(); }

auto evaluator::lookup_local(const std::string &name) -> const value * {
  for (const auto &scope :
       locals_ | std::views::drop(frame_base_) | std::views::reverse) {
    if (const auto found = scope.find(name); found != scope.end()) {
      return &found->second;
    }
  }
  return nullptr;
}

auto evaluator::resolve_pending_static(const std::string &name,
                                       source_span span) -> const value * {
  const auto pending_it = pending_statics_.find(name);
  if (pending_it == pending_statics_.end()) {
    return nullptr;
  }
  if (!statics_in_progress_.insert(name).second) {
    report(span, std::format("`{}` is part of a compile-time evaluation "
                             "cycle (it references itself, directly or "
                             "indirectly, while being evaluated)",
                             name));
    return nullptr;
  }
  const auto saved_file = file_id_;
  file_id_ = pending_it->second.owner_file;
  auto evaluated = evaluate(*pending_it->second.initializer);
  file_id_ = saved_file;
  statics_in_progress_.erase(name);
  if (evaluated.is_error()) {
    return nullptr;
  }
  const auto [bound_it, _] =
      globals_.insert_or_assign(name, std::move(evaluated));
  return &bound_it->second;
}

auto evaluator::report(source_span span, std::string message) -> value {
  if (diagnostics_suppressed_) {
    return value::make_error();
  }
  auto error = diagnostic(diagnostic_level::error, std::move(message), file_id_)
                   .with_label(span, "here");
  // An error inside a callee in another file (often the standard library)
  // names a line the user never wrote; point at the call they did write.
  if (!call_sites_.empty() && call_sites_.front().file != file_id_) {
    const auto origin = call_sites_.front().file;
    const auto site = std::ranges::find_if(
        call_sites_ | std::views::reverse,
        [&](const call_site &s) -> bool { return s.file == origin; });
    error.children.push_back(
        diagnostic(diagnostic_level::note,
                   std::format("reached from this compile-time call to `{}`",
                               site->callee),
                   site->file)
            .with_label(site->span, "evaluated at compile time here"));
  }
  diag_.emit(std::move(error));
  return value::make_error();
}

auto evaluator::try_eval_ordinary_call(
    const ast::func_decl &fn, const ast::call_expr &call,
    std::vector<std::pair<std::string, value>> type_args)
    -> std::optional<value> {
  const auto saved_suppressed = diagnostics_suppressed_;
  diagnostics_suppressed_ = true;
  auto args = std::vector<value>{};
  args.reserve(call.args.size());
  auto ok = true;
  for (const auto &arg : call.args) {
    if (arg.name.has_value() || arg.value == nullptr) {
      ok = false;
      break;
    }
    auto evaluated = evaluate(*arg.value);
    if (evaluated.is_error()) {
      ok = false;
      break;
    }
    args.push_back(std::move(evaluated));
  }
  auto result = ok ? call_function(fn, fn.name, std::move(args), call.span,
                                   std::move(type_args))
                   : value::make_error();
  diagnostics_suppressed_ = saved_suppressed;
  if (!ok || result.is_error()) {
    return std::nullopt;
  }
  return result;
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

auto evaluator::read_module_name(const std::string &name, source_span span)
    -> bare_name_reading {
  if (name_resolver_) {
    return name_resolver_(name);
  }
  if (const auto it = globals_.find(name); it != globals_.end()) {
    return bare_name_reading{.reading = bare_name_reading::kind::static_value,
                             .base = it->second,
                             .function = nullptr,
                             .reason = {}};
  }
  if (pending_statics_.contains(name)) {
    if (const auto *resolved = resolve_pending_static(name, span);
        resolved != nullptr) {
      return bare_name_reading{.reading = bare_name_reading::kind::static_value,
                               .base = *resolved,
                               .function = nullptr,
                               .reason = {}};
    }
    // `resolve_pending_static` already reported a diagnostic (cycle, or the
    // initializer's own evaluation failure) — don't pile on another one.
    return bare_name_reading{.reading = bare_name_reading::kind::not_constant,
                             .base = {},
                             .function = nullptr,
                             .reason = {}};
  }
  if (const auto fn_it = pending_functions_.find(name);
      fn_it != pending_functions_.end()) {
    return bare_name_reading{.reading = bare_name_reading::kind::function,
                             .base = {},
                             .function = fn_it->second,
                             .reason = {}};
  }
  return bare_name_reading{};
}

auto evaluator::resolve_name(const std::string &name, source_span span)
    -> value {
  if (const auto *local = lookup_local(name); local != nullptr) {
    return *local;
  }
  auto reading = read_module_name(name, span);
  switch (reading.reading) {
  case bare_name_reading::kind::static_value:
    return std::move(reading.base);
  case bare_name_reading::kind::function:
    return value::make_closure(name, reading.function);
  case bare_name_reading::kind::not_constant:
    if (reading.reason.empty()) {
      return value::make_error();
    }
    return report(span, std::move(reading.reason));
  case bare_name_reading::kind::not_found:
    break;
  }
  return report(span,
                std::format("`{}` is not a compile-time constant that has "
                            "been evaluated yet",
                            name));
}

auto evaluator::resolve_callee(const std::string &name, source_span span)
    -> const ast::func_decl * {
  if (const auto *local = lookup_local(name); local != nullptr) {
    return local->kind == value_kind::closure ? local->function : nullptr;
  }
  const auto reading = read_module_name(name, span);
  switch (reading.reading) {
  case bare_name_reading::kind::function:
    return reading.function;
  case bare_name_reading::kind::static_value:
    return reading.base.kind == value_kind::closure ? reading.base.function
                                                    : nullptr;
  case bare_name_reading::kind::not_constant:
  case bare_name_reading::kind::not_found:
    break;
  }
  return nullptr;
}

auto evaluator::eval_ident(const ast::ident_expr &ident) -> value {
  if (const auto variant = resolve_variant(ident)) {
    return value::make_variant(variant->first, variant->second, {});
  }
  if (const auto *local = lookup_local(ident.name); local != nullptr) {
    return *local;
  }
  // `resolve_variant`'s checker-backed answer came back empty. Before
  // reporting an unresolved name, try the name-only variant fallback — but
  // only when nothing else already explains this identifier, so a binding
  // that happens to share a spelling with some unrelated sum type's variant
  // is never misread as a variant constructor. See
  // `resolve_variant_by_name`'s doc comment for why this path exists.
  auto reading = read_module_name(ident.name, ident.span);
  if (reading.reading == bare_name_reading::kind::not_found) {
    if (const auto fallback = resolve_variant_by_name(ident.name)) {
      return value::make_variant(fallback->first, fallback->second, {});
    }
  }
  switch (reading.reading) {
  case bare_name_reading::kind::static_value:
    return std::move(reading.base);
  case bare_name_reading::kind::function:
    return value::make_closure(ident.name, reading.function);
  case bare_name_reading::kind::not_constant:
    if (reading.reason.empty()) {
      return value::make_error();
    }
    return report(ident.span, std::move(reading.reason));
  case bare_name_reading::kind::not_found:
    break;
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
      // `-x` is signed-integer-overflow UB in C++ when `x` is exactly the
      // 64-bit minimum (no positive counterpart exists to negate to), so
      // this boundary must be rejected explicitly rather than evaluated —
      // mirrors the VM's/LLVM backend's own checked negation. Except: a
      // direct integer literal (or one cast, `<lit> as T`) whose *positive*
      // magnitude is exactly one past `int64::max` (`9223372036854775808`)
      // already reinterprets to this same bit pattern in `parse_integer_
      // literal` above — negating that bit pattern in two's complement is a
      // no-op, not UB, and is exactly how `int64::min` (`-9223372036854775808`)
      // gets written at all (see `std.limits.min`). Only a genuinely
      // *computed* value landing on `int64::min` (not a fresh literal) is a
      // real double-negation overflow.
      if (operand.integer == std::numeric_limits<int64_t>::min()) {
        if (is_int_literal_operand(*un.operand)) {
          return value::make_int(operand.integer);
        }
        return report(un.span,
                      "this negation overflows in compile-time evaluation");
      }
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
  // Compile-time values are immutable and never aliased, so a shared
  // reference to one reads exactly like the value itself: `&TABLE[a..b]`
  // (how `std.unicode` hands its tables to lookups) is the sub-list, and
  // `*r` on such a reference is `r`.
  case ast::unary_op::addr_of:
  case ast::unary_op::deref:
    return operand;
  case ast::unary_op::addr_of_mut:
    return report(un.span, "`&mut` cannot be taken in compile-time "
                           "evaluation; compile-time values cannot be "
                           "changed through a reference");
  case ast::unary_op::bit_not:
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
  return apply_binary(bin.op, lhs, rhs, bin.span);
}

auto evaluator::apply_binary(ast::binary_op op, const value &lhs,
                             const value &rhs, source_span span) -> value {
  // Equality is defined for strings too, independent of the numeric path
  // below.
  if (op == ast::binary_op::eq_eq || op == ast::binary_op::bang_eq) {
    if (lhs.kind == value_kind::string && rhs.kind == value_kind::string) {
      const auto equal = lhs.string == rhs.string;
      return value::make_bool(op == ast::binary_op::eq_eq ? equal : !equal);
    }
    if (lhs.kind == value_kind::boolean && rhs.kind == value_kind::boolean) {
      const auto equal = lhs.boolean == rhs.boolean;
      return value::make_bool(op == ast::binary_op::eq_eq ? equal : !equal);
    }
    if (lhs.kind == value_kind::variant_instance ||
        rhs.kind == value_kind::variant_instance) {
      if (lhs.kind != rhs.kind || lhs.type_name != rhs.type_name) {
        return report(
            span, std::format("cannot compare `{}` and `{}` at compile time — "
                              "they are not the same sum type",
                              lhs.kind == value_kind::variant_instance
                                  ? lhs.type_name
                                  : std::string("<non-variant>"),
                              rhs.kind == value_kind::variant_instance
                                  ? rhs.type_name
                                  : std::string("<non-variant>")));
      }
      const auto equal = variant_values_equal(lhs, rhs);
      return value::make_bool(op == ast::binary_op::eq_eq ? equal : !equal);
    }
  }

  // `str`'s `ord` and `add` impls (`std.string`), on the same primitives.
  if (lhs.kind == value_kind::string && rhs.kind == value_kind::string) {
    const auto order = runtime::str_compare(lhs.string, rhs.string);
    switch (op) {
    case ast::binary_op::lt:
      return value::make_bool(order < 0);
    case ast::binary_op::lt_eq:
      return value::make_bool(order <= 0);
    case ast::binary_op::gt:
      return value::make_bool(order > 0);
    case ast::binary_op::gt_eq:
      return value::make_bool(order >= 0);
    case ast::binary_op::add:
      return value::make_string(lhs.string + rhs.string);
    default:
      break;
    }
  }

  const auto lhs_numeric =
      lhs.kind == value_kind::integer || lhs.kind == value_kind::floating;
  const auto rhs_numeric =
      rhs.kind == value_kind::integer || rhs.kind == value_kind::floating;
  if (!lhs_numeric || !rhs_numeric) {
    return report(span, "this operator requires compile-time numeric "
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

  switch (op) {
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
      return report(span, "division by zero in compile-time evaluation");
    }
    return value::make_int(pair.li / pair.ri);
  case ast::binary_op::mod:
    if (pair.is_float) {
      return value::make_float(std::fmod(pair.lf, pair.rf));
    }
    if (pair.ri == 0) {
      return report(span, "division by zero in compile-time evaluation");
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
    return report(span, "this operator is not yet supported in "
                        "compile-time evaluation");
  }
}

auto evaluator::eval_array(const ast::array_expr &arr) -> value {
  if (arr.fill_value != nullptr) {
    auto fill = evaluate(*arr.fill_value);
    if (fill.is_error()) {
      return fill;
    }
    if (arr.fill_count == nullptr) {
      return value::make_error();
    }
    auto count = evaluate(*arr.fill_count);
    if (count.is_error()) {
      return count;
    }
    if (count.kind != value_kind::integer || count.integer < 0) {
      return report(arr.span, "a `[val; count]` repeat count must be a "
                              "non-negative compile-time integer");
    }
    auto elements = std::vector<value>{};
    elements.reserve(static_cast<size_t>(count.integer));
    for (auto i = int64_t{0}; i < count.integer; ++i) {
      elements.push_back(fill);
    }
    return value::make_list(std::move(elements));
  }
  auto elements = std::vector<value>{};
  elements.reserve(arr.elements.size());
  for (const auto &element : arr.elements) {
    if (element == nullptr) {
      return value::make_error();
    }
    auto evaluated = evaluate(*element);
    if (evaluated.is_error()) {
      return evaluated;
    }
    elements.push_back(std::move(evaluated));
  }
  return value::make_list(std::move(elements));
}

auto evaluator::eval_tuple(const ast::tuple_expr &tup) -> value {
  auto elements = std::vector<value>{};
  elements.reserve(tup.elements.size());
  for (const auto &element : tup.elements) {
    if (element == nullptr) {
      return value::make_error();
    }
    auto evaluated = evaluate(*element);
    if (evaluated.is_error()) {
      return evaluated;
    }
    elements.push_back(std::move(evaluated));
  }
  return value::make_list(std::move(elements));
}

auto evaluator::eval_struct(const ast::struct_expr &st) -> value {
  auto type_name = std::string{};
  if (const auto *head = ast::as<const ast::ident_expr>(st.type_name.get());
      head != nullptr) {
    type_name = head->name;
  }
  auto fields = std::unordered_map<std::string, value>{};
  for (const auto &field : st.fields) {
    if (field.value == nullptr) {
      // Shorthand `{x}` lowers to `{x: x}` — read the bare name back as a
      // reference.
      auto shorthand = resolve_name(field.name, field.span);
      if (shorthand.is_error()) {
        return shorthand;
      }
      fields.insert_or_assign(field.name, std::move(shorthand));
      continue;
    }
    auto evaluated = evaluate(*field.value);
    if (evaluated.is_error()) {
      return evaluated;
    }
    fields.insert_or_assign(field.name, std::move(evaluated));
  }
  return value::make_struct(std::move(type_name), std::move(fields));
}

auto evaluator::eval_field(const ast::field_expr &fld) -> value {
  if (fld.object == nullptr) {
    return value::make_error();
  }
  auto object = evaluate(*fld.object);
  if (object.is_error()) {
    return object;
  }
  if (object.kind != value_kind::struct_instance) {
    return report(fld.span, "`.` field access requires a compile-time "
                            "struct value");
  }
  const auto it = object.fields.find(fld.field_name);
  if (it == object.fields.end()) {
    return report(fld.span,
                  std::format("compile-time struct value has no field `{}`",
                              fld.field_name));
  }
  return it->second;
}

auto evaluator::eval_index(const ast::index_expr &idx) -> value {
  if (idx.object == nullptr || idx.index == nullptr) {
    return value::make_error();
  }
  auto object = evaluate(*idx.object);
  if (object.is_error()) {
    return object;
  }
  // `s[a .. b]` on a `str` or a list: the sub-range, with the same bounds
  // rule the backends enforce.
  if (const auto *range = ast::as<const ast::binary_expr>(idx.index.get());
      range != nullptr && range->lhs != nullptr && range->rhs != nullptr &&
      (range->op == ast::binary_op::range ||
       range->op == ast::binary_op::range_inclusive)) {
    const auto lo = evaluate(*range->lhs);
    if (lo.is_error()) {
      return lo;
    }
    const auto hi = evaluate(*range->rhs);
    if (hi.is_error()) {
      return hi;
    }
    if (lo.kind != value_kind::integer || hi.kind != value_kind::integer) {
      return report(idx.span, "a compile-time range index requires integer "
                              "bounds");
    }
    const auto end = range->op == ast::binary_op::range_inclusive
                         ? hi.integer + 1
                         : hi.integer;
    const auto size = object.kind == value_kind::string
                          ? object.string.size()
                          : object.elements.size();
    if (object.kind != value_kind::string && object.kind != value_kind::list) {
      return report(idx.span, "`[a .. b]` requires a compile-time `str` or "
                              "list value");
    }
    if (lo.integer < 0 || end < lo.integer || static_cast<size_t>(end) > size) {
      return report(idx.span,
                    std::format("compile-time range {}..{} is out of bounds "
                                "for a length of {}",
                                lo.integer, end, size));
    }
    const auto first = static_cast<size_t>(lo.integer);
    const auto count = static_cast<size_t>(end - lo.integer);
    if (object.kind == value_kind::string) {
      return value::make_string(object.string.substr(first, count));
    }
    const auto from =
        object.elements.begin() + static_cast<std::ptrdiff_t>(first);
    return value::make_list(
        std::vector<value>(from, from + static_cast<std::ptrdiff_t>(count)));
  }
  auto index = evaluate(*idx.index);
  if (index.is_error()) {
    return index;
  }
  if (object.kind != value_kind::list) {
    return report(idx.span, "`[]` indexing requires a compile-time list "
                            "value");
  }
  if (index.kind != value_kind::integer || index.integer < 0 ||
      static_cast<size_t>(index.integer) >= object.elements.size()) {
    return report(idx.span, "compile-time index is out of range");
  }
  return object.elements[static_cast<size_t>(index.integer)];
}

auto evaluator::eval_module_path(const ast::module_path_expr &path) -> value {
  // The parser can't tell `origin.x` (field access) from a module-qualified
  // path, so the checker decides (`path_resolver_`), told only whether the
  // root is one of this evaluator's own locals.
  if (path.segments.empty()) {
    return value::make_error();
  }
  const auto &root = path.segments.front();
  auto reading = dotted_path_reading{};
  if (path_resolver_) {
    reading = path_resolver_(path, lookup_local(root) != nullptr);
  }
  auto current = value{};
  switch (reading.reading) {
  case dotted_path_reading::kind::local:
    current = resolve_name(root, path.span);
    break;
  case dotted_path_reading::kind::static_value:
    current = std::move(reading.base);
    break;
  case dotted_path_reading::kind::function:
    return value::make_closure(path.segments.back(), reading.function);
  case dotted_path_reading::kind::not_constant:
    if (reading.reason.empty()) {
      return value::make_error();
    }
    return report(path.span, std::move(reading.reason));
  }
  if (current.is_error()) {
    return current;
  }
  for (size_t i = reading.first_field; i < path.segments.size(); ++i) {
    if (current.kind != value_kind::struct_instance) {
      return report(path.span, "`.` field access requires a compile-time "
                               "struct value");
    }
    const auto it = current.fields.find(path.segments[i]);
    if (it == current.fields.end()) {
      return report(path.span,
                    std::format("compile-time struct value has no field `{}`",
                                path.segments[i]));
    }
    // Copy out before assigning: `it->second` lives inside `current.fields`,
    // which `current`'s own assignment frees mid-copy.
    auto field = it->second;
    current = std::move(field);
  }
  return current;
}

auto evaluator::eval_quote(const ast::quote_expr &quote) -> value {
  // The splices resolved now, while the locals they read are live.
  auto splices = std::vector<value>{};
  for (const auto *splice : fragment_builder::eager_splices(quote)) {
    const auto *operand =
        splice->kind == ast::node_kind::splice_expr
            ? ast::cast<const ast::splice_expr>(*splice).operand.get()
            : ast::cast<const ast::splice_type>(*splice).operand.get();
    if (operand == nullptr) {
      return value::make_error();
    }
    auto resolved = evaluate(*operand);
    if (resolved.is_error()) {
      return resolved;
    }
    splices.push_back(std::move(resolved));
  }
  auto built = fragments_.quote(quote, splices);
  if (!built.has_value()) {
    return report(built.error().span, std::move(built.error().message));
  }
  return *std::move(built);
}

auto evaluator::call_function(
    const ast::func_decl &fn, const std::string &name, std::vector<value> args,
    source_span span, std::vector<std::pair<std::string, value>> type_args)
    -> value {
  // A generic call runs the instance its type arguments name.
  //
  // Silent evaluation (`try_evaluate`) also probes ordinary code — a `let`
  // whose initializer might be a compile-time constant — so there only a
  // `static def` runs on the VM. An ordinary function would actually run,
  // however long it takes, to bind a value nothing may read.
  const auto speculative = diagnostics_suppressed_ && !fn.modifiers.is_static;
  if (vm_call_ && vm_mode_ != vm_mode::off && !speculative &&
      !fn.modifiers.is_intrinsic && args.size() == fn.params.size()) {
    const auto vm_args = args;
    const auto vm_type_args = type_args;
    const auto interpret = [&]() -> value {
      return interpret_function(fn, name, std::move(args), span,
                                std::move(type_args));
    };
    if (auto result =
            call_on_vm(fn, name, vm_args, vm_type_args, span, interpret)) {
      return *std::move(result);
    }
  }
  return interpret_function(fn, name, std::move(args), span,
                            std::move(type_args));
}

auto evaluator::call_on_vm(
    const ast::func_decl &fn, const std::string &name,
    std::span<const value> args,
    std::span<const std::pair<std::string, value>> type_args, source_span span,
    const std::function<value()> &interpret) -> std::optional<value> {
  auto outcome = vm_call_(fn, args, type_args);
  if (outcome.outcome == vm_call_outcome::kind::failed) {
    outcome.message = std::format("compile-time call to `{}` failed: {}", name,
                                  outcome.message);
  }
  return use_vm_outcome(std::move(outcome),
                        std::format("compile-time call to `{}`", name), span,
                        interpret);
}

auto evaluator::use_vm_outcome(vm_call_outcome outcome, std::string_view what,
                               source_span span,
                               const std::function<value()> &run_interpreted)
    -> std::optional<value> {
  // The comparison is between two whole runs, so the interpreted one uses
  // the interpreter throughout.
  const auto interpret = [&]() -> value {
    const auto saved = std::exchange(vm_mode_, vm_mode::off);
    auto interpreted = run_interpreted();
    vm_mode_ = saved;
    return interpreted;
  };
  switch (outcome.outcome) {
  case vm_call_outcome::kind::not_run:
    return std::nullopt;
  case vm_call_outcome::kind::failed:
    if (vm_mode_ == vm_mode::compare) {
      // The interpreter decides; a failure the VM alone reports is a
      // difference, and the interpreter's own failure reports itself.
      auto interpreted = interpret();
      if (!interpreted.is_error()) {
        report(span, std::format("{} failed on the VM but not in the "
                                 "interpreter: {}",
                                 what, outcome.message));
      }
      return interpreted;
    }
    return report_vm_failure(span, std::move(outcome.message),
                             std::move(outcome.notes));
  case vm_call_outcome::kind::returned:
    break;
  }
  if (vm_mode_ != vm_mode::compare) {
    return std::move(outcome.result);
  }
  // Silently: a call the interpreter cannot run and the VM can is the VM
  // doing more, not a difference.
  const auto saved_suppressed = std::exchange(diagnostics_suppressed_, true);
  auto interpreted = interpret();
  diagnostics_suppressed_ = saved_suppressed;
  if (interpreted.is_error()) {
    return std::move(outcome.result);
  }
  if (!same_value(interpreted, outcome.result)) {
    report(span, std::format("{} returned {} on the VM but {} in the "
                             "interpreter",
                             what, describe_value(outcome.result),
                             describe_value(interpreted)));
  }
  return interpreted;
}

auto evaluator::report_vm_failure(source_span span, std::string message,
                                  std::vector<diagnostic> notes) -> value {
  if (diagnostics_suppressed_) {
    return value::make_error();
  }
  auto error = diagnostic(diagnostic_level::error, std::move(message), file_id_)
                   .with_label(span, "evaluated at compile time here");
  for (auto &note : notes) {
    error.children.push_back(std::move(note));
  }
  diag_.emit(std::move(error));
  return value::make_error();
}

auto evaluator::interpret_function(
    const ast::func_decl &fn, const std::string &name, std::vector<value> args,
    source_span span, std::vector<std::pair<std::string, value>> type_args)
    -> value {
  static const auto trace = std::getenv("CINDER_COMPTIME_TRACE") != nullptr;
  if (trace) {
    std::println(stderr, "comptime interp: call `{}` depth {}", name,
                 call_depth_);
  }
  if (call_depth_ >= k_max_call_depth) {
    return report(span, std::format("compile-time call to `{}` exceeded the "
                                    "maximum evaluation depth of {} — this "
                                    "usually means unbounded recursion",
                                    name, k_max_call_depth));
  }
  if (args.size() > fn.params.size()) {
    return report(span,
                  std::format("`{}` takes {} argument(s), but {} were given",
                              name, fn.params.size(), args.size()));
  }
  if (fn.modifiers.is_intrinsic) {
    if (args.size() != fn.params.size()) {
      return report(span, std::format("`{}` takes {} argument(s), but {} were "
                                      "given",
                                      name, fn.params.size(), args.size()));
    }
    return call_intrinsic(fn, args, span);
  }
  // Reported before switching into `fn`'s context: it is the call site's
  // mistake, so it belongs to the caller's file.
  for (size_t i = args.size(); i < fn.params.size(); ++i) {
    if (fn.params[i].default_value == nullptr) {
      return report(span,
                    std::format("`{}` is missing a required argument", name));
    }
  }
  auto scope = std::unordered_map<std::string, value>{};
  for (auto &[type_param_name, type_arg] : type_args) {
    scope.insert_or_assign(type_param_name, std::move(type_arg));
  }
  ++call_depth_;
  push_locals({});
  const auto saved_frame_base = frame_base_;
  frame_base_ = locals_.size() - 1;
  // Default arguments and the body both run in `fn`'s own context
  // (`call_context_`), with only `fn`'s parameters in scope.
  const auto run = [&] -> value {
    for (size_t i = 0; i < fn.params.size(); ++i) {
      const auto &param = fn.params[i];
      auto arg_value =
          i < args.size() ? std::move(args[i]) : evaluate(*param.default_value);
      if (arg_value.is_error()) {
        return arg_value;
      }
      if (param.pattern != nullptr &&
          !bind_pattern(*param.pattern, arg_value, scope)) {
        return value::make_error();
      }
    }
    locals_.back() = std::move(scope);
    if (fn.body_expr != nullptr) {
      return evaluate(*fn.body_expr);
    }
    const auto exec = evaluate_block_value(fn.body_stmts);
    if (exec.errored) {
      return value::make_error();
    }
    return exec.returned ? exec.result : value::make_unit();
  };
  const auto *saved_function = current_function_;
  current_function_ = &fn;
  call_sites_.push_back(
      call_site{.file = file_id_, .span = span, .callee = name});
  auto result = call_context_ ? call_context_(fn, run) : run();
  call_sites_.pop_back();
  current_function_ = saved_function;
  pop_locals();
  frame_base_ = saved_frame_base;
  --call_depth_;
  return result;
}

auto evaluator::eval_lambda(const ast::lambda_expr &lambda) -> value {
  auto captures = std::unordered_map<std::string, value>{};
  if (lambda.captures.has_value()) {
    for (const auto &capture : *lambda.captures) {
      if (ast::is_reference_capture(capture.mode)) {
        return report(capture.span,
                      std::format("`{}` is captured by reference, which "
                                  "compile-time evaluation does not support; "
                                  "capture it by value instead",
                                  capture.name));
      }
      if (const auto *local = lookup_local(capture.name); local != nullptr) {
        captures.insert_or_assign(capture.name, *local);
      }
    }
  } else {
    // Implicit capture is by value on both backends, so a snapshot of the
    // frame reads the same. Inner scopes shadow outer ones.
    for (const auto &scope : locals_ | std::views::drop(frame_base_)) {
      for (const auto &[name, local] : scope) {
        captures.insert_or_assign(name, local);
      }
    }
  }
  return value::make_lambda(&lambda, std::move(captures), current_function_);
}

auto evaluator::call_lambda(const value &callee, std::vector<value> args,
                            source_span span) -> value {
  const auto &lambda = ast::cast<const ast::lambda_expr>(*callee.fragment);
  if (args.size() != lambda.params.size()) {
    return report(span, std::format("this lambda takes {} argument(s), but {} "
                                    "were given",
                                    lambda.params.size(), args.size()));
  }
  if (call_depth_ >= k_max_call_depth) {
    return report(span, std::format("compile-time lambda call exceeded the "
                                    "maximum evaluation depth of {} — this "
                                    "usually means unbounded recursion",
                                    k_max_call_depth));
  }
  auto scope = callee.fields;
  for (size_t i = 0; i < args.size(); ++i) {
    const auto *pattern =
        ast::as<const ast::pattern>(lambda.params[i].pattern.get());
    if (pattern == nullptr || !bind_pattern(*pattern, args[i], scope)) {
      return value::make_error();
    }
  }
  ++call_depth_;
  push_locals(std::move(scope));
  const auto saved_frame_base = frame_base_;
  frame_base_ = locals_.size() - 1;
  const auto run = [&] -> value {
    if (lambda.body_expr != nullptr) {
      return evaluate(*lambda.body_expr);
    }
    const auto exec = evaluate_block_value(lambda.body_stmts);
    if (exec.errored) {
      return value::make_error();
    }
    return exec.returned ? exec.result : value::make_unit();
  };
  const auto *saved_function = current_function_;
  current_function_ = callee.function;
  call_sites_.push_back(
      call_site{.file = file_id_, .span = span, .callee = "a lambda"});
  auto result = callee.function != nullptr && call_context_
                    ? call_context_(*callee.function, run)
                    : run();
  call_sites_.pop_back();
  current_function_ = saved_function;
  pop_locals();
  frame_base_ = saved_frame_base;
  --call_depth_;
  return result;
}

auto evaluator::try_eval_expr_builder_call(const ast::call_expr &call)
    -> std::optional<value> {
  if (call.callee == nullptr ||
      call.callee->kind != ast::node_kind::field_expr) {
    return std::nullopt;
  }
  const auto &field = ast::cast<const ast::field_expr>(*call.callee);
  if (field.object == nullptr ||
      field.object->kind != ast::node_kind::ident_expr) {
    return std::nullopt;
  }
  if (ast::cast<const ast::ident_expr>(*field.object).name != "expr") {
    return std::nullopt;
  }
  auto args = std::vector<value>{};
  auto spans = std::vector<source_span>{};
  for (const auto &arg : call.args) {
    if (arg.value == nullptr) {
      return value::make_error();
    }
    auto evaluated = evaluate(*arg.value);
    if (evaluated.is_error()) {
      return evaluated;
    }
    args.push_back(std::move(evaluated));
    spans.push_back(arg.value->span);
  }
  auto built = fragments_.build(field.field_name, args, spans, call.span);
  if (!built.has_value()) {
    return report(built.error().span, std::move(built.error().message));
  }
  return *std::move(built);
}

auto evaluator::resolve_type_reference(const ast::ident_expr &ident)
    -> const ast::type_decl * {
  if (const auto *local = lookup_local(ident.name);
      local != nullptr && local->kind == value_kind::type_value) {
    return local->type_decl;
  }
  const auto it = pending_types_.find(ident.name);
  return it != pending_types_.end() ? it->second : nullptr;
}

namespace {

/// Every builtin scalar type name a literal type argument can spell —
/// mirrors the set `checker::type_key_of`/`types_.entry` would classify as
/// `builtin_kind` with no declaration. Kept narrow and explicit (rather than
/// reused from `semantic::types_`, which the evaluator has no dependency
/// on) since this is the one place the evaluator ever needs to recognize a
/// builtin name written directly as a type argument, rather than reached
/// through an already-resolved `type_param_slots_` binding (see
/// `checker::check_function`'s `type_param_locals`, which covers the more
/// common "T inside a generic body" case via `types_.entry` directly and
/// therefore also handles compound builtins like `array[int32, 4]` that
/// this list deliberately doesn't).
constexpr std::array<std::string_view, 21> k_builtin_scalar_type_names = {{
    "bool",   "char",    "byte",   "str",     "unit",    "int8",     "int16",
    "int32",  "int64",   "int128", "isize",   "uint8",   "uint16",   "uint32",
    "uint64", "uint128", "usize",  "float32", "float64", "float128", "fn",
}};

} // namespace

auto evaluator::resolve_generic_type_arg(const ast::ident_expr &ident)
    -> std::optional<value> {
  if (const auto *local = lookup_local(ident.name);
      local != nullptr && local->kind == value_kind::type_value) {
    return *local;
  }
  if (const auto it = pending_types_.find(ident.name);
      it != pending_types_.end()) {
    return value::make_type_value(it->second->name, it->second);
  }
  if (std::ranges::contains(k_builtin_scalar_type_names,
                            std::string_view(ident.name))) {
    return value::make_type_value(ident.name, nullptr);
  }
  return std::nullopt;
}

auto evaluator::unwrap_explicit_generic_callee(
    const ast::expr &callee, std::vector<const ast::expr *> &args_out)
    -> const ast::expr * {
  if (callee.kind == ast::node_kind::index_expr) {
    const auto &index = ast::cast<const ast::index_expr>(callee);
    if (index.object == nullptr || index.index == nullptr) {
      return nullptr;
    }
    args_out.push_back(index.index.get());
    return index.object.get();
  }
  if (callee.kind == ast::node_kind::call_expr) {
    const auto &applied = ast::cast<const ast::call_expr>(callee);
    if (applied.callee == nullptr) {
      return nullptr;
    }
    for (const auto &arg : applied.args) {
      if (arg.value == nullptr || arg.name.has_value()) {
        return nullptr; // a named bracket argument isn't this shape
      }
      args_out.push_back(arg.value.get());
    }
    return applied.callee.get();
  }
  return nullptr;
}

auto evaluator::resolve_variant(const ast::node &node)
    -> std::optional<std::pair<std::string, std::string>> {
  if (!variant_resolver_) {
    return std::nullopt;
  }
  auto resolved = variant_resolver_(node);
  if (!resolved) {
    return std::nullopt;
  }
  const auto type_it = pending_types_.find(resolved->first);
  if (type_it == pending_types_.end()) {
    // A builtin sum type (`option`, `result`) has no declaration to check
    // the tag against; the checker already did.
    if (resolved->first == "option" || resolved->first == "result") {
      return resolved;
    }
    return std::nullopt;
  }
  const auto *decl = type_it->second;
  const auto *sum_def =
      ast::as<const ast::sum_type_def>(decl->definition.get());
  if (sum_def == nullptr) {
    return std::nullopt;
  }
  for (const auto &variant : sum_def->body.variants) {
    if (variant.name == resolved->second) {
      return resolved;
    }
  }
  return std::nullopt;
}

auto evaluator::resolve_variant_by_name(const std::string &name)
    -> std::optional<std::pair<std::string, std::string>> {
  // The builtin `option`/`result` have no declaration in `pending_types_`.
  if (name == "some" || name == "none") {
    return std::make_pair(std::string{"option"}, name);
  }
  if (name == "ok" || name == "err") {
    return std::make_pair(std::string{"result"}, name);
  }
  for (const auto &[type_name, decl] : pending_types_) {
    if (decl == nullptr) {
      continue;
    }
    const auto *sum_def =
        ast::as<const ast::sum_type_def>(decl->definition.get());
    if (sum_def == nullptr) {
      continue;
    }
    for (const auto &variant : sum_def->body.variants) {
      if (variant.name == name) {
        return std::make_pair(type_name, name);
      }
    }
  }
  return std::nullopt;
}

auto evaluator::try_eval_comptime_generic_call(const ast::call_expr &call)
    -> std::optional<value> {
  if (call.callee == nullptr) {
    return std::nullopt;
  }
  auto type_arg_exprs = std::vector<const ast::expr *>{};
  const auto *base =
      unwrap_explicit_generic_callee(*call.callee, type_arg_exprs);
  if (base == nullptr || type_arg_exprs.empty() ||
      base->kind != ast::node_kind::ident_expr) {
    return std::nullopt;
  }
  const auto &callee_ident = ast::cast<const ast::ident_expr>(*base);
  const auto *fn = resolve_callee(callee_ident.name, callee_ident.span);
  if (fn == nullptr || fn->type_params.empty() ||
      type_arg_exprs.size() > fn->type_params.size()) {
    return std::nullopt;
  }
  // The checker's solution carries the exact types; the spellings below are
  // read only when it recorded none.
  auto solved =
      generic_call_resolver_ ? generic_call_resolver_(call) : std::nullopt;
  const auto read_spellings = !solved.has_value();
  auto type_args =
      std::move(solved).value_or(std::vector<std::pair<std::string, value>>{});
  for (size_t i = 0; read_spellings && i < type_arg_exprs.size(); ++i) {
    const auto *type_arg_expr = type_arg_exprs[i];
    if (type_arg_expr->kind != ast::node_kind::ident_expr) {
      return report(call.span, "a compile-time generic call's type argument "
                               "must be a plain type name");
    }
    const auto &type_arg_ident =
        ast::cast<const ast::ident_expr>(*type_arg_expr);
    auto resolved = resolve_generic_type_arg(type_arg_ident);
    if (!resolved.has_value()) {
      return report(type_arg_ident.span,
                    std::format("`{}` does not name a known type here",
                                type_arg_ident.name));
    }
    type_args.emplace_back(fn->type_params[i].name, std::move(*resolved));
  }
  auto args = std::vector<value>{};
  args.reserve(call.args.size());
  for (const auto &arg : call.args) {
    if (arg.name.has_value()) {
      return report(arg.span, "named arguments are not yet supported in "
                              "compile-time function calls");
    }
    if (arg.value == nullptr) {
      return value::make_error();
    }
    auto evaluated = evaluate(*arg.value);
    if (evaluated.is_error()) {
      return evaluated;
    }
    args.push_back(std::move(evaluated));
  }
  return call_function(*fn, callee_ident.name, std::move(args), call.span,
                       std::move(type_args));
}

auto evaluator::qualified_callee_path(const ast::expr &callee)
    -> std::optional<ast::module_path_expr> {
  // The parser leaves a call's last segment off the path: `a.f(x)` is a
  // field access `f` on `a`, `a.b.f(x)` one on the path `a.b`.
  const auto *field = ast::as<const ast::field_expr>(&callee);
  if (field == nullptr || field->object == nullptr) {
    return std::nullopt;
  }
  auto path = ast::module_path_expr{};
  path.span = callee.span;
  if (const auto *root = ast::as<const ast::ident_expr>(field->object.get())) {
    path.segments.push_back(root->name);
  } else if (const auto *prefix =
                 ast::as<const ast::module_path_expr>(field->object.get())) {
    path.segments = prefix->segments;
  } else {
    return std::nullopt;
  }
  path.segments.push_back(field->field_name);
  return path;
}

auto evaluator::method_callee(const ast::expr &callee)
    -> const ast::field_expr * {
  const auto *field = ast::as<const ast::field_expr>(&callee);
  if (field == nullptr || field->object == nullptr) {
    return nullptr;
  }
  const auto path = qualified_callee_path(callee);
  if (!path.has_value()) {
    // `"ab".len()`, `xs[0].len()`: no name or path for the checker to read
    // as a module-level function, so this can only be a method call.
    return field;
  }
  // `a.b.f(x)`: a method `f` on the value `a.b` unless the checker reads
  // the whole path as naming something else (a module function, or a name
  // with no compile-time value, which the path route reports).
  const auto root_is_local = lookup_local(path->segments.front()) != nullptr;
  if (!path_resolver_) {
    return root_is_local ? field : nullptr;
  }
  switch (path_resolver_(*path, root_is_local).reading) {
  case dotted_path_reading::kind::local:
  case dotted_path_reading::kind::static_value:
    return field;
  case dotted_path_reading::kind::function:
  case dotted_path_reading::kind::not_constant:
    break;
  }
  return nullptr;
}

auto evaluator::eval_local_push(const ast::call_expr &call,
                                const ast::field_expr &method)
    -> std::optional<value> {
  const auto *target = ast::as<const ast::ident_expr>(method.object.get());
  if (method.field_name != "push" || target == nullptr ||
      call.args.size() != 1 || call.args.front().value == nullptr) {
    return std::nullopt;
  }
  const auto find_list = [&] -> value * {
    for (auto &scope :
         locals_ | std::views::drop(frame_base_) | std::views::reverse) {
      if (const auto found = scope.find(target->name); found != scope.end()) {
        return found->second.kind == value_kind::list ? &found->second
                                                      : nullptr;
      }
    }
    return nullptr;
  };
  if (find_list() == nullptr) {
    return std::nullopt;
  }
  auto element = evaluate(*call.args.front().value);
  if (element.is_error()) {
    return element;
  }
  // Found again: evaluating the argument may have grown `locals_`.
  auto *list = find_list();
  if (list == nullptr) {
    return value::make_error();
  }
  list->elements.push_back(std::move(element));
  return value::make_unit();
}

auto evaluator::eval_method_call(const ast::call_expr &call,
                                 const ast::field_expr &method,
                                 const value &receiver) -> value {
  const auto &name = method.field_name;
  auto args = std::vector<value>{};
  args.reserve(call.args.size() + 1);
  args.push_back(receiver);
  for (const auto &arg : call.args) {
    if (arg.name.has_value()) {
      return report(arg.span, "named arguments are not yet supported in "
                              "compile-time method calls");
    }
    if (arg.value == nullptr) {
      return value::make_error();
    }
    auto evaluated = evaluate(*arg.value);
    if (evaluated.is_error()) {
      return evaluated;
    }
    args.push_back(std::move(evaluated));
  }
  // `str.len()` is built into the language (`k_builtin_methods`), not an
  // `extend str` method with a body to run. A compile-time list is the
  // evaluator's own representation, not `std.list`'s, so its methods are
  // built in here too (`push` in `eval_local_push`).
  if ((receiver.kind == value_kind::string ||
       receiver.kind == value_kind::list) &&
      name == "len") {
    if (args.size() != 1) {
      return report(call.span, "`len` takes no arguments");
    }
    return value::make_int(static_cast<int64_t>(
        receiver.kind == value_kind::string ? receiver.string.size()
                                            : receiver.elements.size()));
  }
  // `str.as_bytes()`: a compile-time `slice[byte]` is a list of integers.
  if (receiver.kind == value_kind::string && name == "as_bytes") {
    if (args.size() != 1) {
      return report(call.span, "`as_bytes` takes no arguments");
    }
    auto bytes = std::vector<value>{};
    bytes.reserve(receiver.string.size());
    for (const auto byte : receiver.string) {
      bytes.push_back(value::make_int(static_cast<unsigned char>(byte)));
    }
    return value::make_list(std::move(bytes));
  }
  if (receiver.kind == value_kind::list) {
    return report(call.span,
                  std::format("the list method `{}` cannot be called at "
                              "compile time: only `len()` and `push(x)` on a "
                              "local `var` are available",
                              name));
  }
  // A struct field holding a function value: `ops.apply(x)`.
  if (receiver.kind == value_kind::struct_instance) {
    if (const auto it = receiver.fields.find(name);
        it != receiver.fields.end()) {
      if (it->second.kind != value_kind::closure ||
          it->second.function == nullptr) {
        return report(call.span,
                      std::format("field `{}` is not a function, so it "
                                  "cannot be called",
                                  name));
      }
      args.erase(args.begin());
      return call_function(*it->second.function, name, std::move(args),
                           call.span);
    }
  }
  if (!method_resolver_) {
    return report(call.span,
                  std::format("the method `{}` cannot be called at compile "
                              "time here: only built-in methods are available",
                              name));
  }
  const auto fn = method_resolver_(*method.object, receiver, name);
  if (!fn.has_value()) {
    return report(call.span, std::format("the method `{}` cannot be called at "
                                         "compile time: {}",
                                         name, fn.error()));
  }
  return call_function(**fn, name, std::move(args), call.span);
}

auto evaluator::call_intrinsic(const ast::func_decl &fn,
                               std::span<const value> args, source_span span)
    -> value {
  const auto &name = fn.name;
  const auto all_strings = [&](size_t count) -> bool {
    return std::ranges::all_of(args.first(std::min(count, args.size())),
                               [](const value &arg) -> bool {
                                 return arg.kind == value_kind::string;
                               });
  };
  const auto found = [](std::optional<size_t> pos) -> value {
    auto fields = std::unordered_map<std::string, value>{};
    fields.insert_or_assign("found", value::make_bool(pos.has_value()));
    fields.insert_or_assign(
        "pos", value::make_int(static_cast<int64_t>(pos.value_or(0))));
    return value::make_struct("find_result", std::move(fields));
  };
  if (name == "rt_str_eq" && all_strings(2)) {
    return value::make_bool(runtime::str_equal(args[0].string, args[1].string));
  }
  if (name == "rt_str_cmp" && all_strings(2)) {
    return value::make_int(
        runtime::str_compare(args[0].string, args[1].string));
  }
  if (name == "rt_str_find" && all_strings(2) &&
      args[2].kind == value_kind::integer && args[2].integer >= 0) {
    return found(runtime::str_find(args[0].string, args[1].string,
                                   static_cast<size_t>(args[2].integer)));
  }
  if (name == "rt_str_rfind" && all_strings(2)) {
    return found(runtime::str_rfind(args[0].string, args[1].string));
  }
  if (name == "rt_str_reverse" && all_strings(1)) {
    return value::make_string(runtime::str_reverse(args[0].string));
  }
  if (name == "rt_str_trim" && all_strings(1) &&
      args[1].kind == value_kind::integer && args[1].integer >= 0 &&
      args[1].integer <= 2) {
    return value::make_string(std::string{runtime::str_trim(
        args[0].string, static_cast<runtime::trim_mode>(args[1].integer))});
  }
  if (name == "rt_str_replace" && all_strings(3)) {
    return value::make_string(
        runtime::str_replace(args[0].string, args[1].string, args[2].string));
  }
  if (name == "rt_str_concat" && all_strings(2)) {
    return value::make_string(args[0].string + args[1].string);
  }
  if (name == "rt_fmt_char_from_codepoint" && args.size() == 1 &&
      args[0].kind == value_kind::integer) {
    const auto cp = args[0].integer;
    if (cp < 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
      return report(span, std::format("{:#x} is not a Unicode scalar value, "
                                      "so it has no UTF-8 encoding",
                                      cp));
    }
    auto out = std::string{};
    encode_utf8_scalar(static_cast<uint32_t>(cp), out);
    return value::make_string(std::move(out));
  }
  return report(span, std::format("the intrinsic `{}` has no compile-time "
                                  "implementation, so it can only be called "
                                  "from code that runs at run time",
                                  name));
}

auto evaluator::eval_call(const ast::call_expr &call) -> value {
  if (call.callee == nullptr) {
    return value::make_error();
  }
  // `some`/`ok`/`err` are keywords, so a call spelled with one is always a
  // variant constructor, even in a body the checker has not typed yet (a
  // method in a module checked after the one evaluating it).
  auto variant = resolve_variant(call);
  if (const auto *callee = ast::as<const ast::ident_expr>(call.callee.get());
      !variant && callee != nullptr &&
      (callee->name == "some" || callee->name == "ok" ||
       callee->name == "err")) {
    variant = resolve_variant_by_name(callee->name);
  }
  if (variant) {
    auto payload = std::vector<value>{};
    payload.reserve(call.args.size());
    for (const auto &arg : call.args) {
      if (arg.name.has_value()) {
        return report(arg.span, "named arguments are not supported when "
                                "constructing a variant at compile time");
      }
      if (arg.value == nullptr) {
        return value::make_error();
      }
      auto arg_value = evaluate(*arg.value);
      if (arg_value.is_error()) {
        return arg_value;
      }
      payload.push_back(std::move(arg_value));
    }
    return value::make_variant(variant->first, variant->second,
                               std::move(payload));
  }
  if (auto builder = try_eval_expr_builder_call(call)) {
    return *builder;
  }
  if (auto reflected = try_eval_type_reflection_call(call)) {
    return *reflected;
  }
  if (auto reflected = try_eval_module_reflection_call(call)) {
    return *reflected;
  }
  if (auto reflected = try_eval_trait_reflection_call(call)) {
    return *reflected;
  }
  // Tried before the general comptime-generic-call dispatch below: both
  // recognize the same `name[A, B](...)` bracket shape, but `implements`
  // is never a registered `pending_functions_` entry (it's evaluator-
  // built-in, like `T.fields()`), so trying it first costs nothing on
  // every call that isn't `implements[..](...)` and avoids relying on
  // "falls through because `pending_functions_` doesn't have it" as the
  // only thing keeping the two apart.
  if (auto implemented = try_eval_implements_call(call)) {
    return *implemented;
  }
  if (auto generic_call = try_eval_comptime_generic_call(call)) {
    return *generic_call;
  }
  const ast::func_decl *fn = nullptr;
  auto callee_name = std::string{};
  if (const auto *callee_ident =
          ast::as<const ast::ident_expr>(call.callee.get())) {
    if (const auto *local = lookup_local(callee_ident->name);
        local != nullptr && local->kind == value_kind::lambda) {
      const auto callee = *local;
      auto args = std::vector<value>{};
      args.reserve(call.args.size());
      for (const auto &arg : call.args) {
        if (arg.name.has_value() || arg.value == nullptr) {
          return report(arg.span, "a lambda takes only positional arguments");
        }
        auto evaluated = evaluate(*arg.value);
        if (evaluated.is_error()) {
          return evaluated;
        }
        args.push_back(std::move(evaluated));
      }
      return call_lambda(callee, std::move(args), call.span);
    }
    callee_name = callee_ident->name;
    fn = resolve_callee(callee_name, callee_ident->span);
  } else if (const auto *method = method_callee(*call.callee)) {
    if (auto pushed = eval_local_push(call, *method)) {
      return *pushed;
    }
    auto receiver = evaluate(*method->object);
    if (receiver.is_error()) {
      return receiver;
    }
    return eval_method_call(call, *method, receiver);
  } else if (auto callee_path = qualified_callee_path(*call.callee)) {
    // `inner.scale(2)`: the checker reads the path, the same as any other
    // dotted name (`eval_module_path`).
    callee_name = callee_path->segments | std::views::join_with('.') |
                  std::ranges::to<std::string>();
    const auto callee = eval_module_path(*callee_path);
    if (callee.is_error()) {
      return callee;
    }
    if (callee.kind == value_kind::closure) {
      fn = callee.function;
    }
  } else {
    return report(call.span, "only direct calls to a named `static def` "
                             "function are supported in compile-time "
                             "evaluation");
  }
  if (fn == nullptr) {
    return report(call.span,
                  std::format("`{}` is not a `static def` function that can "
                              "be called at compile time",
                              callee_name));
  }
  auto args = std::vector<value>{};
  args.reserve(call.args.size());
  for (const auto &arg : call.args) {
    if (arg.name.has_value()) {
      return report(arg.span, "named arguments are not yet supported in "
                              "compile-time function calls");
    }
    if (arg.value == nullptr) {
      return value::make_error();
    }
    auto evaluated = evaluate(*arg.value);
    if (evaluated.is_error()) {
      return evaluated;
    }
    args.push_back(std::move(evaluated));
  }
  // An implicit generic call (`measure(b)`) names no type arguments; the
  // checker's solution binds them, so `T.name()` and a method on a `T`
  // value work in the callee's body.
  auto type_args = std::vector<std::pair<std::string, value>>{};
  if (!fn->type_params.empty() && generic_call_resolver_) {
    type_args = generic_call_resolver_(call).value_or(
        std::vector<std::pair<std::string, value>>{});
  }
  return call_function(*fn, callee_name, std::move(args), call.span,
                       std::move(type_args));
}

auto evaluator::bind_pattern(const ast::pattern &pattern, const value &v,
                             std::unordered_map<std::string, value> &scope)
    -> bool {
  switch (pattern.kind) {
  case ast::node_kind::wildcard_pattern:
    return true;
  case ast::node_kind::literal_pattern: {
    const auto &lit = ast::cast<const ast::literal_pattern>(pattern);
    switch (lit.lit_kind) {
    case token_kind::kw_true:
      return v.kind == value_kind::boolean && v.boolean;
    case token_kind::kw_false:
      return v.kind == value_kind::boolean && !v.boolean;
    case token_kind::int_lit: {
      const auto parsed = parse_integer_literal(lit.value);
      return parsed.has_value() && v.kind == value_kind::integer &&
             v.integer == *parsed;
    }
    case token_kind::float_lit: {
      const auto parsed = parse_float_literal(lit.value);
      return parsed.has_value() && v.kind == value_kind::floating &&
             v.floating == *parsed;
    }
    case token_kind::string_lit: {
      auto decoded = decode_string_literal(lit.value);
      return decoded.has_value() && v.kind == value_kind::string &&
             v.string == *decoded;
    }
    case token_kind::char_lit: {
      const auto decoded = decode_char_literal(lit.value);
      return decoded.has_value() && v.kind == value_kind::integer &&
             std::cmp_equal(v.integer, *decoded);
    }
    default:
      report(pattern.span, "this literal pattern form is not yet supported "
                           "in compile-time evaluation");
      return false;
    }
  }
  case ast::node_kind::binding_pattern: {
    const auto &binding = ast::cast<const ast::binding_pattern>(pattern);
    scope.insert_or_assign(binding.name, v);
    return true;
  }
  case ast::node_kind::group_pattern: {
    const auto &group = ast::cast<const ast::group_pattern>(pattern);
    if (group.inner == nullptr || !bind_pattern(*group.inner, v, scope)) {
      return false;
    }
    if (group.alias.has_value()) {
      scope.insert_or_assign(*group.alias, v);
    }
    return true;
  }
  case ast::node_kind::tuple_pattern: {
    const auto &tup = ast::cast<const ast::tuple_pattern>(pattern);
    if (v.kind != value_kind::list ||
        v.elements.size() != tup.elements.size()) {
      report(pattern.span, "compile-time tuple pattern does not match the "
                           "shape of the value being destructured");
      return false;
    }
    for (size_t i = 0; i < tup.elements.size(); ++i) {
      if (tup.elements[i] == nullptr ||
          !bind_pattern(*tup.elements[i], v.elements[i], scope)) {
        return false;
      }
    }
    return true;
  }
  case ast::node_kind::struct_pattern: {
    const auto &st = ast::cast<const ast::struct_pattern>(pattern);
    if (v.kind != value_kind::struct_instance) {
      report(pattern.span, "compile-time struct pattern requires a struct "
                           "value");
      return false;
    }
    for (const auto &field : st.fields) {
      if (field.is_rest) {
        continue;
      }
      const auto it = v.fields.find(field.name);
      if (it == v.fields.end()) {
        report(field.span,
               std::format("compile-time struct value has no field `{}`",
                           field.name));
        return false;
      }
      if (field.pattern != nullptr) {
        if (!bind_pattern(*field.pattern, it->second, scope)) {
          return false;
        }
      } else {
        scope.insert_or_assign(field.name, it->second);
      }
    }
    return true;
  }
  case ast::node_kind::constructor_pattern: {
    const auto &ctor = ast::cast<const ast::constructor_pattern>(pattern);
    if (v.kind != value_kind::variant_instance || v.variant_tag != ctor.name) {
      return false;
    }
    if (ctor.args.size() != v.elements.size()) {
      report(pattern.span,
             std::format("compile-time variant pattern `@{}` expects {} "
                         "payload value{}, found {}",
                         ctor.name, v.elements.size(),
                         v.elements.size() == 1 ? "" : "s", ctor.args.size()));
      return false;
    }
    for (size_t i = 0; i < ctor.args.size(); ++i) {
      if (ctor.args[i] == nullptr ||
          !bind_pattern(*ctor.args[i], v.elements[i], scope)) {
        return false;
      }
    }
    return true;
  }
  case ast::node_kind::option_pattern:
  case ast::node_kind::result_pattern: {
    // `@some(p)`, `@ok(p)`, `@err(e)`: a one-payload variant of the builtin
    // `option`/`result`.
    const auto kind =
        pattern.kind == ast::node_kind::option_pattern
            ? ast::cast<const ast::option_pattern>(pattern).option_kind
            : ast::cast<const ast::result_pattern>(pattern).result_kind;
    const auto *inner =
        pattern.kind == ast::node_kind::option_pattern
            ? ast::cast<const ast::option_pattern>(pattern).inner.get()
            : ast::cast<const ast::result_pattern>(pattern).inner.get();
    const auto *tag = kind == ast::option_result_kind::some ? "some"
                      : kind == ast::option_result_kind::ok ? "ok"
                                                            : "err";
    if (v.kind != value_kind::variant_instance || v.variant_tag != tag ||
        v.elements.size() != 1) {
      return false;
    }
    return inner == nullptr || bind_pattern(*inner, v.elements.front(), scope);
  }
  case ast::node_kind::or_pattern: {
    const auto &alt = ast::cast<const ast::or_pattern>(pattern);
    for (const auto &alternative : alt.alternatives) {
      if (alternative == nullptr) {
        continue;
      }
      // Each alternative gets its own trial scope: an alternative that
      // fails to match may have partially populated bindings on the way
      // to discovering the mismatch (e.g. a nested constructor pattern
      // whose own sub-pattern didn't match), and those must not leak into
      // the next alternative's attempt or into the caller's scope on
      // overall failure.
      auto trial = scope;
      if (bind_pattern(*alternative, v, trial)) {
        scope = std::move(trial);
        return true;
      }
    }
    return false;
  }
  default:
    report(pattern.span, "this pattern form is not yet supported in "
                         "compile-time evaluation");
    return false;
  }
}

auto evaluator::evaluate_iterable(const ast::expr &iterable) -> value {
  if (const auto *bin = ast::as<const ast::binary_expr>(&iterable);
      bin != nullptr && (bin->op == ast::binary_op::range ||
                         bin->op == ast::binary_op::range_inclusive)) {
    if (bin->lhs == nullptr || bin->rhs == nullptr) {
      return value::make_error();
    }
    auto lo = evaluate(*bin->lhs);
    if (lo.is_error()) {
      return lo;
    }
    auto hi = evaluate(*bin->rhs);
    if (hi.is_error()) {
      return hi;
    }
    if (lo.kind != value_kind::integer || hi.kind != value_kind::integer) {
      return report(iterable.span, "a compile-time range requires integer "
                                   "bounds");
    }
    const auto end = bin->op == ast::binary_op::range_inclusive ? hi.integer + 1
                                                                : hi.integer;
    auto elements = std::vector<value>{};
    for (auto i = lo.integer; i < end; ++i) {
      elements.push_back(value::make_int(i));
    }
    return value::make_list(std::move(elements));
  }
  auto evaluated = evaluate(iterable);
  if (evaluated.is_error()) {
    return evaluated;
  }
  if (evaluated.kind != value_kind::list) {
    return report(iterable.span, "a compile-time `for` requires a list "
                                 "or range value to iterate over");
  }
  return evaluated;
}

auto evaluator::eval_match(const ast::match_expr &match) -> value {
  if (match.subject == nullptr) {
    return value::make_error();
  }
  auto subject = evaluate(*match.subject);
  if (subject.is_error()) {
    return subject;
  }
  for (const auto &arm : match.arms) {
    if (arm.pattern == nullptr || arm.has_error) {
      continue;
    }
    const auto &pattern = ast::cast<const ast::pattern>(*arm.pattern);
    auto trial_scope = locals_.back();
    if (!bind_pattern(pattern, subject, trial_scope)) {
      continue;
    }
    locals_.back() = std::move(trial_scope);
    if (arm.guard != nullptr) {
      auto guard = evaluate(*arm.guard);
      if (guard.is_error()) {
        return value::make_error();
      }
      if (guard.kind != value_kind::boolean || !guard.boolean) {
        continue;
      }
    }
    if (arm.body_expr != nullptr) {
      return evaluate(*arm.body_expr);
    }
    auto result = evaluate_block_value(arm.body_stmts);
    if (result.errored) {
      return value::make_error();
    }
    if (!result.returned) {
      return report(arm.span, "this compile-time `match` arm must produce a "
                              "value (end with `return` or a tail "
                              "expression)");
    }
    return result.result;
  }
  return report(match.span, "no arm of this compile-time `match` matched its "
                            "subject");
}

auto evaluator::eval_if(const ast::if_expr &if_expr_node) -> value {
  for (const auto &branch : if_expr_node.branches) {
    if (branch.condition == nullptr) {
      continue;
    }
    auto condition = evaluate(*branch.condition);
    if (condition.is_error()) {
      return value::make_error();
    }
    if (condition.kind != value_kind::boolean) {
      return report(branch.span, "a compile-time `if` condition must be a "
                                 "`bool`");
    }
    if (!condition.boolean) {
      continue;
    }
    auto result = evaluate_block_value(branch.body);
    if (result.errored) {
      return value::make_error();
    }
    if (!result.returned) {
      return report(branch.span, "this compile-time `if` branch must "
                                 "produce a value (end with `return` or a "
                                 "tail expression)");
    }
    return result.result;
  }
  auto result = evaluate_block_value(if_expr_node.else_body);
  if (result.errored) {
    return value::make_error();
  }
  if (!result.returned) {
    return report(if_expr_node.span, "this compile-time `if` expression's "
                                     "`else` branch must produce a value "
                                     "(end with `return` or a tail "
                                     "expression)");
  }
  return result.result;
}

namespace {

/// Bit width + signedness for a builtin scalar `as`-cast target. Mirrors
/// `bytecode::numeric_kind`/`bit_width` (`src/bytecode/value.h`) in spirit
/// without pulling in a dependency from `comptime` on `bytecode` — kept
/// narrow and self-contained, the same tradeoff `k_builtin_scalar_type_
/// names` above already makes. `int128`/`uint128`/`float128` are
/// deliberately absent: `comptime::value` stores every integer as a 64-bit
/// `int64_t` and every float as a `double` (`value.h`), so a cast result
/// wider than that has nowhere to live, and the runtime has no
/// representation for them yet either (todo item 6) — a cast to one reports
/// "not supported" rather than silently narrowing to 64 bits. `char` isn't
/// listed separately since a `char` value is just an `integer` holding its
/// code point (see `eval_literal`'s `char_lit` case), identical in shape to
/// `uint32`.
struct scalar_cast_kind {
  int bits = 0;
  bool is_signed = false;
  bool is_float = false;
};

[[nodiscard]] auto scalar_cast_kind_of(std::string_view name)
    -> std::optional<scalar_cast_kind> {
  if (name == "int8") {
    return scalar_cast_kind{.bits = 8, .is_signed = true};
  }
  if (name == "int16") {
    return scalar_cast_kind{.bits = 16, .is_signed = true};
  }
  if (name == "int32") {
    return scalar_cast_kind{.bits = 32, .is_signed = true};
  }
  if (name == "int64" || name == "isize") {
    return scalar_cast_kind{.bits = 64, .is_signed = true};
  }
  if (name == "uint8" || name == "byte") {
    return scalar_cast_kind{.bits = 8, .is_signed = false};
  }
  if (name == "uint16") {
    return scalar_cast_kind{.bits = 16, .is_signed = false};
  }
  if (name == "uint32" || name == "char") {
    return scalar_cast_kind{.bits = 32, .is_signed = false};
  }
  if (name == "uint64" || name == "usize") {
    return scalar_cast_kind{.bits = 64, .is_signed = false};
  }
  if (name == "float32") {
    return scalar_cast_kind{.bits = 32, .is_float = true};
  }
  if (name == "float64") {
    return scalar_cast_kind{.bits = 64, .is_float = true};
  }
  return std::nullopt;
}

/// Sign-extends the low `bits` of `raw` to a full 64-bit two's-complement
/// value — mirrors `bytecode::vm`'s `sign_extend` (`src/bytecode/vm.cpp`).
[[nodiscard]] auto sign_extend_to_64(std::uint64_t raw, int bits)
    -> std::int64_t {
  if (bits >= 64) {
    return static_cast<std::int64_t>(raw);
  }
  const auto mask = (std::uint64_t{1} << bits) - 1;
  raw &= mask;
  const auto sign_bit = std::uint64_t{1} << (bits - 1);
  return static_cast<std::int64_t>((raw ^ sign_bit) - sign_bit);
}

} // namespace

auto evaluator::eval_cast(const ast::cast_expr &cast) -> value {
  if (cast.operand == nullptr || cast.target_type == nullptr) {
    return value::make_error();
  }
  auto operand = evaluate(*cast.operand);
  if (operand.is_error()) {
    return operand;
  }

  const auto *named = ast::as<const ast::named_type>(cast.target_type.get());
  if (named == nullptr || named->path.size() != 1) {
    return report(cast.span, "this cast target is not yet supported in "
                             "compile-time evaluation");
  }

  // Resolve through a bound generic type parameter (`v as T`) the same way
  // a type argument is, so `T` names the concrete scalar its call site
  // instantiated it at, not the literal spelling "T".
  const std::string &type_name_written = named->path.front();
  std::string resolved_name = type_name_written;
  if (const auto *local = lookup_local(type_name_written);
      local != nullptr && local->kind == value_kind::type_value) {
    resolved_name = local->type_name;
  }

  if (resolved_name == "bool") {
    if (operand.kind == value_kind::boolean) {
      return operand;
    }
    if (operand.kind == value_kind::integer) {
      return value::make_bool(operand.integer != 0);
    }
    if (operand.kind == value_kind::floating) {
      return value::make_bool(operand.floating != 0.0);
    }
    return report(cast.span,
                  "cannot cast this value to `bool` at compile time");
  }

  const auto kind = scalar_cast_kind_of(resolved_name);
  if (!kind.has_value()) {
    return report(cast.span, std::format("cast to `{}` is not supported in "
                                         "compile-time evaluation",
                                         resolved_name));
  }

  if (kind->is_float) {
    double as_double = 0.0;
    if (operand.kind == value_kind::integer) {
      as_double = static_cast<double>(operand.integer);
    } else if (operand.kind == value_kind::floating) {
      as_double = operand.floating;
    } else {
      return report(cast.span, "`as` requires a compile-time numeric operand");
    }
    if (kind->bits == 32) {
      as_double = static_cast<double>(static_cast<float>(as_double));
    }
    return value::make_float(as_double);
  }

  std::int64_t as_int = 0;
  if (operand.kind == value_kind::integer) {
    as_int = operand.integer;
  } else if (operand.kind == value_kind::floating) {
    as_int = static_cast<std::int64_t>(operand.floating);
  } else {
    return report(cast.span, "`as` requires a compile-time numeric operand");
  }

  const auto raw = static_cast<std::uint64_t>(as_int);
  if (kind->is_signed) {
    return value::make_int(sign_extend_to_64(raw, kind->bits));
  }
  const auto mask = kind->bits >= 64 ? ~std::uint64_t{0}
                                     : (std::uint64_t{1} << kind->bits) - 1;
  return value::make_int(static_cast<std::int64_t>(raw & mask));
}

auto evaluator::evaluate_stmt(const ast::node &node) -> exec_result {
  switch (node.kind) {
  case ast::node_kind::let_stmt: {
    const auto &let = ast::cast<const ast::let_stmt>(node);
    if (let.initializer == nullptr || let.pattern == nullptr) {
      return exec_result{.errored = true};
    }
    auto initial = evaluate(*let.initializer);
    if (initial.is_error()) {
      return exec_result{.errored = true};
    }
    if (!bind_pattern(*let.pattern, initial, locals_.back())) {
      return exec_result{.errored = true};
    }
    return exec_result{};
  }
  case ast::node_kind::var_stmt: {
    const auto &var = ast::cast<const ast::var_stmt>(node);
    if (var.initializer == nullptr) {
      return exec_result{.errored = true};
    }
    auto initial = evaluate(*var.initializer);
    if (initial.is_error()) {
      return exec_result{.errored = true};
    }
    locals_.back().insert_or_assign(var.name, std::move(initial));
    return exec_result{};
  }
  case ast::node_kind::assign_stmt: {
    const auto &assign = ast::cast<const ast::assign_stmt>(node);
    const auto *target = ast::as<const ast::ident_expr>(assign.target.get());
    const auto compound_op = ast::compound_assign_op(assign.op);
    if (target == nullptr || assign.value == nullptr ||
        (assign.op != ast::assign_op::assign && !compound_op.has_value())) {
      report(node.span, "this assignment form is not yet supported in "
                        "compile-time evaluation");
      return exec_result{.errored = true};
    }
    auto new_value = evaluate(*assign.value);
    if (new_value.is_error()) {
      return exec_result{.errored = true};
    }
    for (auto &scope :
         locals_ | std::views::drop(frame_base_) | std::views::reverse) {
      const auto found = scope.find(target->name);
      if (found == scope.end()) {
        continue;
      }
      if (compound_op.has_value()) {
        new_value =
            apply_binary(*compound_op, found->second, new_value, node.span);
        if (new_value.is_error()) {
          return exec_result{.errored = true};
        }
      }
      found->second = std::move(new_value);
      return exec_result{};
    }
    report(node.span, std::format("`{}` is not a known compile-time local "
                                  "variable",
                                  target->name));
    return exec_result{.errored = true};
  }
  case ast::node_kind::expr_stmt: {
    const auto &wrapper = ast::cast<const ast::expr_stmt>(node);
    if (wrapper.expr == nullptr) {
      return exec_result{.errored = true};
    }
    auto evaluated = evaluate(*wrapper.expr);
    if (evaluated.is_error()) {
      return exec_result{.errored = true};
    }
    return exec_result{};
  }
  case ast::node_kind::return_stmt: {
    const auto &ret = ast::cast<const ast::return_stmt>(node);
    if (ret.value == nullptr) {
      return exec_result{.returned = true, .result = value::make_unit()};
    }
    auto evaluated = evaluate(*ret.value);
    if (evaluated.is_error()) {
      return exec_result{.errored = true};
    }
    return exec_result{.returned = true, .result = std::move(evaluated)};
  }
  case ast::node_kind::match_stmt: {
    const auto &stmt = ast::cast<const ast::match_stmt>(node);
    if (stmt.subject == nullptr) {
      return exec_result{.errored = true};
    }
    auto subject = evaluate(*stmt.subject);
    if (subject.is_error()) {
      return exec_result{.errored = true};
    }
    for (const auto &arm : stmt.arms) {
      if (arm.pattern == nullptr || arm.has_error) {
        continue;
      }
      const auto &pattern = ast::cast<const ast::pattern>(*arm.pattern);
      auto trial_scope = locals_.back();
      if (!bind_pattern(pattern, subject, trial_scope)) {
        continue;
      }
      locals_.back() = std::move(trial_scope);
      if (arm.guard != nullptr) {
        auto guard = evaluate(*arm.guard);
        if (guard.is_error()) {
          return exec_result{.errored = true};
        }
        if (guard.kind != value_kind::boolean || !guard.boolean) {
          continue;
        }
      }
      // A compact `=> expr` arm provides the enclosing block's value the
      // same way a bare tail expression would (`checker::node_provides_
      // function_value`'s `match_stmt` case, `check.cpp`) — treated here as
      // an implicit return, since that is the only role a `match`
      // statement's *value* can play in the statement sequences this
      // evaluator executes (there is no separate "match as a value-
      // producing expression, used non-tail" form in `static def` bodies
      // today). A block-form arm (`body_stmts`) runs as an ordinary
      // statement sequence and only returns if it explicitly does.
      if (arm.body_expr != nullptr) {
        auto result = evaluate(*arm.body_expr);
        if (result.is_error()) {
          return exec_result{.errored = true};
        }
        return exec_result{.returned = true, .result = std::move(result)};
      }
      return evaluate_stmts(arm.body_stmts);
    }
    report(node.span, "no arm of this compile-time `match` matched its "
                      "subject");
    return exec_result{.errored = true};
  }
  case ast::node_kind::if_stmt: {
    const auto &stmt = ast::cast<const ast::if_stmt>(node);
    for (const auto &branch : stmt.branches) {
      if (branch.condition == nullptr) {
        continue;
      }
      auto condition = evaluate(*branch.condition);
      if (condition.is_error()) {
        return exec_result{.errored = true};
      }
      if (condition.kind != value_kind::boolean) {
        report(branch.span, "a compile-time `if` condition must be a "
                            "`bool`");
        return exec_result{.errored = true};
      }
      if (condition.boolean) {
        return evaluate_stmts(branch.body);
      }
    }
    return evaluate_stmts(stmt.else_body);
  }
  case ast::node_kind::scope_stmt:
    return evaluate_stmts(ast::cast<const ast::scope_stmt>(node).body);
  case ast::node_kind::static_decl: {
    const auto &decl = ast::cast<const ast::static_decl>(node);
    switch (decl.decl_kind) {
    case ast::static_decl_kind::binding: {
      if (decl.initializer == nullptr || decl.name.empty()) {
        return exec_result{};
      }
      auto evaluated = evaluate(*decl.initializer);
      if (evaluated.is_error()) {
        return exec_result{.errored = true};
      }
      locals_.back().insert_or_assign(decl.name, std::move(evaluated));
      return exec_result{};
    }
    case ast::static_decl_kind::assertion: {
      if (decl.assert_condition == nullptr) {
        return exec_result{};
      }
      auto evaluated = evaluate(*decl.assert_condition);
      if (!evaluated.is_error() && evaluated.kind == value_kind::boolean &&
          !evaluated.is_true()) {
        report(decl.assert_condition->span,
               decl.assert_message.value_or(
                   "`static assert` condition was false"));
        return exec_result{.errored = true};
      }
      return exec_result{};
    }
    case ast::static_decl_kind::conditional_compilation: {
      if (decl.if_condition == nullptr) {
        return exec_result{};
      }
      auto condition = evaluate(*decl.if_condition);
      if (condition.is_error() || condition.kind != value_kind::boolean) {
        return exec_result{};
      }
      return evaluate_stmts(condition.boolean ? decl.if_body : decl.else_body);
    }
    case ast::static_decl_kind::for_inline:
    case ast::static_decl_kind::for_block:
      // An inline form outside tail position gives nothing a value, so its
      // yields are evaluated (for their diagnostics) and dropped.
      return evaluate_static_for(decl, nullptr);
    default:
      return exec_result{};
    }
  }
  case ast::node_kind::while_stmt:
    return evaluate_while(ast::cast<const ast::while_stmt>(node));
  case ast::node_kind::for_stmt:
    return evaluate_for(ast::cast<const ast::for_stmt>(node));
  case ast::node_kind::break_stmt:
    return exec_result{.broke = true};
  case ast::node_kind::continue_stmt:
    return exec_result{.continued = true};
  default:
    report(node.span, "this statement form is not yet supported in "
                      "compile-time evaluation");
    return exec_result{.errored = true};
  }
}

auto evaluator::count_loop_iteration(std::size_t &iterations, source_span span)
    -> bool {
  if (++iterations <= k_max_loop_iterations) {
    return true;
  }
  report(span, std::format("this compile-time loop ran more than {} "
                           "iterations — this usually means it never ends",
                           k_max_loop_iterations));
  return false;
}

auto evaluator::run_loop_body(const std::vector<ast::ptr<ast::node>> &body,
                              std::unordered_map<std::string, value> scope,
                              bool &stop) -> std::optional<exec_result> {
  push_locals(std::move(scope));
  auto exec = evaluate_stmts(body);
  pop_locals();
  if (exec.errored || exec.returned) {
    return exec;
  }
  stop = exec.broke;
  return std::nullopt;
}

auto evaluator::evaluate_while(const ast::while_stmt &loop) -> exec_result {
  auto iterations = std::size_t{0};
  while (true) {
    auto scope = std::unordered_map<std::string, value>{};
    if (loop.let_pattern != nullptr) {
      if (loop.let_expr == nullptr) {
        return exec_result{.errored = true};
      }
      auto subject = evaluate(*loop.let_expr);
      if (subject.is_error()) {
        return exec_result{.errored = true};
      }
      if (!bind_pattern(*loop.let_pattern, subject, scope)) {
        return exec_result{};
      }
    } else {
      if (loop.condition == nullptr) {
        return exec_result{.errored = true};
      }
      auto condition = evaluate(*loop.condition);
      if (condition.is_error()) {
        return exec_result{.errored = true};
      }
      if (condition.kind != value_kind::boolean) {
        report(loop.condition->span, "a compile-time `while` condition must "
                                     "be a `bool`");
        return exec_result{.errored = true};
      }
      if (!condition.boolean) {
        return exec_result{};
      }
    }
    const auto &header =
        loop.condition != nullptr ? *loop.condition : *loop.let_expr;
    if (!count_loop_iteration(iterations, header.span)) {
      return exec_result{.errored = true};
    }
    auto stop = false;
    if (auto exit = run_loop_body(loop.body, std::move(scope), stop)) {
      return *exit;
    }
    if (stop) {
      return exec_result{};
    }
  }
}

auto evaluator::evaluate_for(const ast::for_stmt &loop) -> exec_result {
  if (loop.iterable == nullptr || loop.patterns.size() != 1 ||
      loop.patterns[0] == nullptr) {
    report(loop.span, "a compile-time `for` supports exactly one loop "
                      "binding");
    return exec_result{.errored = true};
  }
  auto list_value = evaluate_iterable(*loop.iterable);
  if (list_value.is_error()) {
    return exec_result{.errored = true};
  }
  auto iterations = std::size_t{0};
  for (const auto &element : list_value.elements) {
    if (!count_loop_iteration(iterations, loop.iterable->span)) {
      return exec_result{.errored = true};
    }
    auto scope = std::unordered_map<std::string, value>{};
    if (!bind_pattern(*loop.patterns[0], element, scope)) {
      return exec_result{.errored = true};
    }
    if (loop.guard != nullptr) {
      push_locals(scope);
      auto guard = evaluate(*loop.guard);
      pop_locals();
      if (guard.is_error()) {
        return exec_result{.errored = true};
      }
      if (guard.kind != value_kind::boolean) {
        report(loop.guard->span, "a compile-time `for` guard must be a "
                                 "`bool`");
        return exec_result{.errored = true};
      }
      if (!guard.boolean) {
        continue;
      }
    }
    auto stop = false;
    if (auto exit = run_loop_body(loop.body, std::move(scope), stop)) {
      return *exit;
    }
    if (stop) {
      break;
    }
  }
  return exec_result{};
}

auto evaluator::evaluate_static_for(const ast::static_decl &decl,
                                    std::vector<value> *yields) -> exec_result {
  // Mirrors `checker::check_static_decl`'s own top-level `static for`
  // iteration exactly (`check.cpp`) — that copy only runs for a `static
  // for` written directly as a module item; this one is what makes the
  // *same* construct work when nested inside a `static def`'s own body
  // (e.g. `derive_show[T]()`'s per-field loop). Only a single loop pattern
  // is supported, matching the same pre-existing restriction.
  if (decl.for_iterable == nullptr || decl.for_iterable->has_error ||
      decl.for_patterns.size() != 1 || decl.for_patterns[0] == nullptr) {
    return exec_result{};
  }
  auto list_value = evaluate_iterable(*decl.for_iterable);
  if (list_value.is_error()) {
    return exec_result{.errored = true};
  }
  for (const auto &element_value : list_value.elements) {
    auto scope = std::unordered_map<std::string, value>{};
    if (!bind_pattern(*decl.for_patterns[0], element_value, scope)) {
      return exec_result{.errored = true};
    }
    push_locals(std::move(scope));
    if (decl.for_guard != nullptr) {
      auto guard = evaluate(*decl.for_guard);
      if (guard.is_error() || guard.kind != value_kind::boolean) {
        pop_locals();
        return exec_result{.errored = true};
      }
      if (!guard.boolean) {
        pop_locals();
        continue;
      }
    }
    if (decl.decl_kind == ast::static_decl_kind::for_inline) {
      if (decl.for_yield != nullptr) {
        auto yielded = evaluate(*decl.for_yield);
        if (yielded.is_error()) {
          pop_locals();
          return exec_result{.errored = true};
        }
        if (yields != nullptr) {
          yields->push_back(std::move(yielded));
        }
      }
      pop_locals();
    } else {
      // `static for` unrolls, so a `break`/`continue` in its body
      // belongs to an enclosing ordinary loop.
      const auto exec = evaluate_stmts(decl.for_body);
      pop_locals();
      if (exec.interrupts()) {
        return exec;
      }
    }
  }
  return exec_result{};
}

auto evaluator::evaluate_stmts(const std::vector<ast::ptr<ast::node>> &body)
    -> exec_result {
  for (const auto &item : body) {
    if (item == nullptr || item->has_error) {
      continue;
    }
    auto result = evaluate_stmt(*item);
    if (result.interrupts()) {
      return result;
    }
  }
  return exec_result{};
}

auto evaluator::evaluate_tail(const ast::node &node) -> exec_result {
  switch (node.kind) {
  case ast::node_kind::expr_stmt: {
    const auto &wrapper = ast::cast<const ast::expr_stmt>(node);
    if (wrapper.expr == nullptr) {
      return exec_result{.errored = true};
    }
    auto evaluated = evaluate(*wrapper.expr);
    if (evaluated.is_error()) {
      return exec_result{.errored = true};
    }
    return exec_result{.returned = true, .result = std::move(evaluated)};
  }
  case ast::node_kind::if_stmt: {
    const auto &stmt = ast::cast<const ast::if_stmt>(node);
    for (const auto &branch : stmt.branches) {
      if (branch.condition == nullptr) {
        continue;
      }
      auto condition = evaluate(*branch.condition);
      if (condition.is_error()) {
        return exec_result{.errored = true};
      }
      if (condition.kind != value_kind::boolean) {
        report(branch.span, "a compile-time `if` condition must be a "
                            "`bool`");
        return exec_result{.errored = true};
      }
      if (condition.boolean) {
        return evaluate_block_value(branch.body);
      }
    }
    return evaluate_block_value(stmt.else_body);
  }
  case ast::node_kind::match_stmt: {
    const auto &stmt = ast::cast<const ast::match_stmt>(node);
    if (stmt.subject == nullptr) {
      return exec_result{.errored = true};
    }
    auto subject = evaluate(*stmt.subject);
    if (subject.is_error()) {
      return exec_result{.errored = true};
    }
    for (const auto &arm : stmt.arms) {
      if (arm.pattern == nullptr || arm.has_error) {
        continue;
      }
      const auto &pattern = ast::cast<const ast::pattern>(*arm.pattern);
      auto trial_scope = locals_.back();
      if (!bind_pattern(pattern, subject, trial_scope)) {
        continue;
      }
      locals_.back() = std::move(trial_scope);
      if (arm.guard != nullptr) {
        auto guard = evaluate(*arm.guard);
        if (guard.is_error()) {
          return exec_result{.errored = true};
        }
        if (guard.kind != value_kind::boolean || !guard.boolean) {
          continue;
        }
      }
      if (arm.body_expr != nullptr) {
        auto result = evaluate(*arm.body_expr);
        if (result.is_error()) {
          return exec_result{.errored = true};
        }
        return exec_result{.returned = true, .result = std::move(result)};
      }
      return evaluate_block_value(arm.body_stmts);
    }
    report(node.span, "no arm of this compile-time `match` matched its "
                      "subject");
    return exec_result{.errored = true};
  }
  case ast::node_kind::static_decl: {
    const auto &decl = ast::cast<const ast::static_decl>(node);
    if (decl.decl_kind == ast::static_decl_kind::conditional_compilation &&
        decl.if_condition != nullptr) {
      auto condition = evaluate(*decl.if_condition);
      if (condition.is_error()) {
        return exec_result{.errored = true};
      }
      if (condition.kind != value_kind::boolean) {
        report(decl.if_condition->span, "a compile-time `static if` "
                                        "condition must be a `bool`");
        return exec_result{.errored = true};
      }
      return evaluate_block_value(condition.boolean ? decl.if_body
                                                    : decl.else_body);
    }
    if (decl.decl_kind == ast::static_decl_kind::for_inline) {
      // A tail inline `static for ... => e` is the block's value: the list
      // of the values it yields.
      auto yields = std::vector<value>{};
      auto exec = evaluate_static_for(decl, &yields);
      if (exec.interrupts()) {
        return exec;
      }
      return exec_result{.returned = true,
                         .result = value::make_list(std::move(yields))};
    }
    return evaluate_stmt(node);
  }
  default:
    return evaluate_stmt(node);
  }
}

auto evaluator::evaluate_block_value(
    const std::vector<ast::ptr<ast::node>> &body) -> exec_result {
  auto last_index = std::optional<size_t>{};
  for (size_t i = 0; i < body.size(); ++i) {
    if (body[i] != nullptr && !body[i]->has_error) {
      last_index = i;
    }
  }
  for (size_t i = 0; i < body.size(); ++i) {
    const auto &item = body[i];
    if (item == nullptr || item->has_error) {
      continue;
    }
    if (last_index.has_value() && i == *last_index) {
      return evaluate_tail(*item);
    }
    auto result = evaluate_stmt(*item);
    if (result.interrupts()) {
      return result;
    }
  }
  return exec_result{};
}

auto evaluator::evaluate(const ast::expr &expr) -> value {
  if (expr.has_error) {
    return value::make_error();
  }
  static const auto trace = std::getenv("CINDER_COMPTIME_TRACE") != nullptr;
  if (trace && call_depth_ == 0 && eval_depth_ == 0) {
    std::println(stderr, "comptime interp: expr kind {} file {} at {}{}",
                 static_cast<int>(expr.kind), file_id_, expr.span.start,
                 diagnostics_suppressed_ ? " (silent)" : "");
  }
  ++eval_depth_;
  auto result = value{};
  // A whole compile-time expression runs on the VM when it can; the parts
  // of one the interpreter is already running stay there.
  if (vm_eval_ && vm_mode_ != vm_mode::off && !probing_ && call_depth_ == 0 &&
      eval_depth_ == 1) {
    auto outcome = vm_eval_(expr);
    const auto interpret = [&]() -> value { return evaluate_unwrapped(expr); };
    if (auto ran = use_vm_outcome(std::move(outcome),
                                  "a compile-time expression", expr.span,
                                  interpret)) {
      result = *std::move(ran);
    } else {
      result = evaluate_unwrapped(expr);
    }
  } else {
    result = evaluate_unwrapped(expr);
  }
  --eval_depth_;
  return result;
}

auto evaluator::evaluate_unwrapped(const ast::expr &expr) -> value {
  switch (expr.kind) {
  case ast::node_kind::literal_expr:
    return eval_literal(ast::cast<const ast::literal_expr>(expr));
  case ast::node_kind::ident_expr:
    return eval_ident(ast::cast<const ast::ident_expr>(expr));
  case ast::node_kind::binary_expr:
    return eval_binary(ast::cast<const ast::binary_expr>(expr));
  case ast::node_kind::unary_expr:
    return eval_unary(ast::cast<const ast::unary_expr>(expr));
  case ast::node_kind::group_expr: {
    const auto &group = ast::cast<const ast::group_expr>(expr);
    return group.inner != nullptr ? evaluate(*group.inner)
                                  : value::make_error();
  }
  case ast::node_kind::array_expr:
    return eval_array(ast::cast<const ast::array_expr>(expr));
  case ast::node_kind::tuple_expr:
    return eval_tuple(ast::cast<const ast::tuple_expr>(expr));
  case ast::node_kind::struct_expr:
    return eval_struct(ast::cast<const ast::struct_expr>(expr));
  case ast::node_kind::field_expr:
    return eval_field(ast::cast<const ast::field_expr>(expr));
  case ast::node_kind::index_expr:
    return eval_index(ast::cast<const ast::index_expr>(expr));
  case ast::node_kind::call_expr:
    return eval_call(ast::cast<const ast::call_expr>(expr));
  case ast::node_kind::module_path_expr:
    return eval_module_path(ast::cast<const ast::module_path_expr>(expr));
  case ast::node_kind::quote_expr:
    return eval_quote(ast::cast<const ast::quote_expr>(expr));
  case ast::node_kind::interpolated_string_expr:
    return eval_interpolated_string(
        ast::cast<const ast::interpolated_string_expr>(expr));
  case ast::node_kind::match_expr:
    return eval_match(ast::cast<const ast::match_expr>(expr));
  case ast::node_kind::if_expr:
    return eval_if(ast::cast<const ast::if_expr>(expr));
  case ast::node_kind::cast_expr:
    return eval_cast(ast::cast<const ast::cast_expr>(expr));
  case ast::node_kind::lambda_expr:
    return eval_lambda(ast::cast<const ast::lambda_expr>(expr));
  default:
    return report(expr.span, "this expression form is not yet supported in "
                             "compile-time evaluation");
  }
}

/// Compile-time evaluation of a `"...{expr}..."` interpolated string literal
/// — needed so a derive body (e.g. `derive_show_sum`/`derive_eq_sum`,
/// `src/std/deriving.cn`) can compute a name like `"p{i}"` at comptime to
/// pass to `expr.ident(...)`; ordinary runtime string interpolation (e.g. a
/// user's `println("{x}")`) never reaches here — it lowers straight to
/// runtime formatting code instead. Deliberately narrow: no format-spec
/// support (width/precision/alignment/type-char), since nothing a comptime
/// derive body builds needs one — a segment with `has_spec` set reports
/// rather than silently ignoring the spec.
auto evaluator::eval_interpolated_string(
    const ast::interpolated_string_expr &interp) -> value {
  auto result = std::string{};
  for (const auto &segment : interp.segments) {
    if (segment.is_literal) {
      result += segment.literal_text;
      continue;
    }
    if (segment.has_spec) {
      return report(interp.span,
                    "a compile-time interpolated string cannot use a "
                    "`:format_spec` — only plain `{expr}` segments are "
                    "supported here");
    }
    if (segment.value == nullptr) {
      return value::make_error();
    }
    auto evaluated = evaluate(*segment.value);
    if (evaluated.is_error()) {
      return evaluated;
    }
    switch (evaluated.kind) {
    case value_kind::string:
      result += evaluated.string;
      break;
    case value_kind::integer:
      result += std::to_string(evaluated.integer);
      break;
    case value_kind::floating:
      result += std::to_string(evaluated.floating);
      break;
    case value_kind::boolean:
      result += evaluated.boolean ? "true" : "false";
      break;
    default:
      return report(segment.value->span,
                    "this value cannot be formatted inside a compile-time "
                    "interpolated string (only strings, integers, floats, "
                    "and booleans are supported here)");
    }
  }
  return value::make_string(std::move(result));
}

} // namespace cinder::comptime
