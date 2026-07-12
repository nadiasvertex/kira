#include "src/comptime/eval.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <format>
#include <ranges>
#include <string>

#include "src/comptime/hygiene.h"
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
  for (const auto &scope : locals_ | std::views::reverse) {
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

auto evaluator::resolve_name(const std::string &name, source_span span)
    -> value {
  if (const auto *local = lookup_local(name); local != nullptr) {
    return *local;
  }
  if (const auto it = globals_.find(name); it != globals_.end()) {
    return it->second;
  }
  if (pending_statics_.contains(name)) {
    if (const auto *resolved = resolve_pending_static(name, span);
        resolved != nullptr) {
      return *resolved;
    }
    // `resolve_pending_static` already reported a diagnostic (cycle, or the
    // initializer's own evaluation failure) — don't pile on another one.
    return value::make_error();
  }
  if (const auto fn_it = pending_functions_.find(name);
      fn_it != pending_functions_.end()) {
    return value::make_closure(name, fn_it->second);
  }
  return report(span,
                std::format("`{}` is not a compile-time constant that has "
                            "been evaluated yet",
                            name));
}

auto evaluator::eval_ident(const ast::ident_expr &ident) -> value {
  return resolve_name(ident.name, ident.span);
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
  if (const auto *head =
          dynamic_cast<const ast::ident_expr *>(st.type_name.get());
      head != nullptr) {
    type_name = head->name;
  }
  auto fields = std::unordered_map<std::string, value>{};
  for (const auto &field : st.fields) {
    if (field.value == nullptr) {
      // Shorthand `{x}` lowers to `{x: x}` — read the bare name back as a
      // reference.
      if (const auto *local = lookup_local(field.name); local != nullptr) {
        fields.insert_or_assign(field.name, *local);
        continue;
      }
      if (const auto it = globals_.find(field.name); it != globals_.end()) {
        fields.insert_or_assign(field.name, it->second);
        continue;
      }
      return report(field.span,
                    std::format("`{}` is not a compile-time constant that "
                                "has been evaluated yet",
                                field.name));
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
  // The parser can't tell `origin.x` (field access) apart from a real
  // module-qualified path at parse time — see `parse_ident_or_path_expr` —
  // so, mirroring `checker::infer_module_path`, treat the first segment as
  // a value reference and every remaining segment as a field projection.
  if (path.segments.empty()) {
    return value::make_error();
  }
  auto current = resolve_name(path.segments.front(), path.span);
  if (current.is_error()) {
    return current;
  }
  for (size_t i = 1; i < path.segments.size(); ++i) {
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
    current = it->second;
  }
  return current;
}

auto evaluator::eval_quote(const ast::quote_expr &quote) -> value {
  if (quote.parsed_body == nullptr) {
    return report(quote.span, "this quoted fragment could not be parsed");
  }
  // Hygiene (design plan section 5): rename this fragment's own internal
  // `let`/`var`/... bindings to fresh names before it's ever spliced
  // anywhere, so a spliced `` `(let temp = 99)` `` can't silently clobber a
  // same-named binding already active at the splice site. Only `expr`/
  // `stmt` fragments get spliced into an *existing* scope this way — a
  // `def_expr`'s own methods get their own fresh function scope regardless,
  // so renaming would be a harmless no-op there; skipped to keep this
  // narrowly scoped to the case that actually needs it.
  // `quote.parsed_body.get()`/`*quote.parsed_body` are non-const despite
  // `quote` itself being `const`: `unique_ptr::get()`/`operator*()` don't
  // propagate constness to the pointee, so mutating through them here
  // doesn't need (and must not use) a `const_cast`.
  if ((quote.fragment_kind == ast::quote_fragment_kind::expr ||
       quote.fragment_kind == ast::quote_fragment_kind::stmt) &&
      hygiene_renamed_.insert(quote.parsed_body.get()).second) {
    rename_internal_bindings(*quote.parsed_body, hygiene_next_id_);
  }
  // Eager splice materialization: a nested `~(...)` found anywhere inside
  // this quote must be resolved *now*, while whatever locals/globals are
  // active at this exact point in evaluation are still live — not left as
  // literal syntax for some later, unrelated splice site to re-evaluate
  // (see `materialize_quote`'s doc comment for the concrete failure this
  // fixes). Only attempted when a nested splice is actually present, so a
  // quote with none (the overwhelmingly common case, and every case tested
  // before this fix) keeps the original zero-copy boxing behavior exactly.
  const ast::node *body = quote.parsed_body.get();
  if (quote_body_has_nested_splice(*body)) {
    auto materialized = materialize_quote(*body);
    if (materialized == nullptr) {
      return value::make_error();
    }
    body = materialized.get();
    synthesized_fragments_.push_back(std::move(materialized));
  }
  switch (quote.fragment_kind) {
  case ast::quote_fragment_kind::expr:
    return value::make_expr_fragment(body);
  case ast::quote_fragment_kind::stmt:
    return value::make_stmt_fragment(body);
  case ast::quote_fragment_kind::def_expr:
    return value::make_def_expr_fragment(body);
  case ast::quote_fragment_kind::type_expr:
    return value::make_type_expr_fragment(body);
  case ast::quote_fragment_kind::none:
    return report(quote.span, "this quoted fragment could not be classified "
                              "as an expression, statement, or definition");
  }
  return value::make_error();
}

auto evaluator::quote_body_has_nested_splice(const ast::node &node) -> bool {
  switch (node.kind) {
  case ast::node_kind::splice_expr:
  case ast::node_kind::splice_type:
  case ast::node_kind::splice_stmt:
    return true;
  case ast::node_kind::binary_expr: {
    const auto &bin = dynamic_cast<const ast::binary_expr &>(node);
    return (bin.lhs != nullptr && quote_body_has_nested_splice(*bin.lhs)) ||
           (bin.rhs != nullptr && quote_body_has_nested_splice(*bin.rhs));
  }
  case ast::node_kind::field_expr: {
    const auto &fld = dynamic_cast<const ast::field_expr &>(node);
    return fld.object != nullptr && quote_body_has_nested_splice(*fld.object);
  }
  case ast::node_kind::return_stmt: {
    const auto &ret = dynamic_cast<const ast::return_stmt &>(node);
    return ret.value != nullptr && quote_body_has_nested_splice(*ret.value);
  }
  case ast::node_kind::func_decl: {
    const auto &fn = dynamic_cast<const ast::func_decl &>(node);
    if (fn.return_type != nullptr &&
        quote_body_has_nested_splice(*fn.return_type)) {
      return true;
    }
    return std::ranges::any_of(fn.body_stmts, [this](const auto &stmt) -> bool {
      return stmt != nullptr && quote_body_has_nested_splice(*stmt);
    });
  }
  case ast::node_kind::impl_decl: {
    const auto &impl = dynamic_cast<const ast::impl_decl &>(node);
    if (impl.for_type != nullptr &&
        quote_body_has_nested_splice(*impl.for_type)) {
      return true;
    }
    return std::ranges::any_of(impl.items, [this](const auto &item) -> bool {
      return item != nullptr && quote_body_has_nested_splice(*item);
    });
  }
  default:
    return false;
  }
}

namespace {
/// Downcasts a freshly-`materialize_quote`d node to a more specific owned
/// pointer type — safe because every call site knows the dynamic type it
/// just asked `materialize_quote` to build (the node kind selected the
/// construction branch), the same way `expr.field`'s `clone_expr_fragment`
/// downcast is safe for the same reason.
template <typename T> auto as_owned(ast::ptr<ast::node> node) -> ast::ptr<T> {
  return ast::ptr<T>(static_cast<T *>(node.release()));
}
} // namespace

auto evaluator::materialize_quote(const ast::node &node)
    -> ast::ptr<ast::node> {
  switch (node.kind) {
  case ast::node_kind::ident_expr:
  case ast::node_kind::literal_expr:
  case ast::node_kind::field_expr:
  case ast::node_kind::interpolated_string_expr:
    return clone_expr_fragment(node);
  case ast::node_kind::splice_expr: {
    const auto &splice = dynamic_cast<const ast::splice_expr &>(node);
    if (splice.operand == nullptr) {
      return nullptr;
    }
    auto resolved = evaluate(*splice.operand);
    if (resolved.is_error()) {
      return nullptr;
    }
    if (resolved.kind != value_kind::expr_fragment ||
        resolved.fragment == nullptr) {
      report(node.span, "this `~` splice inside a quoted fragment must "
                        "resolve to a quoted expression");
      return nullptr;
    }
    return clone_expr_fragment(*resolved.fragment);
  }
  case ast::node_kind::splice_type: {
    const auto &splice = dynamic_cast<const ast::splice_type &>(node);
    if (splice.operand == nullptr) {
      return nullptr;
    }
    auto resolved = evaluate(*splice.operand);
    if (resolved.is_error()) {
      return nullptr;
    }
    // A `type_value` (a bound generic parameter, e.g. `T` inside `static
    // def derive_show[T]()`) or a plain `expr_fragment` wrapping a single
    // identifier (`derive_show`'s own `~(expr.ident(T.name()))` idiom) both
    // name a type by spelling here. This narrow evaluator-side path has no
    // `type_table`/checker access to do the richer reinterpretation
    // `checker::reinterpret_as_named_type` performs for an ordinary,
    // non-materialized `splice_type` — it only needs to cover these two
    // shapes, the only ones a compile-time generic call can produce.
    auto type_name = std::string{};
    if (resolved.kind == value_kind::type_value) {
      type_name = resolved.type_name;
    } else if (resolved.kind == value_kind::expr_fragment &&
               resolved.fragment != nullptr &&
               resolved.fragment->kind == ast::node_kind::ident_expr) {
      type_name =
          dynamic_cast<const ast::ident_expr &>(*resolved.fragment).name;
    } else {
      report(node.span, "this `~` splice inside a quoted fragment's type "
                        "position must resolve to a type name");
      return nullptr;
    }
    auto named = ast::make<ast::named_type>();
    named->span = node.span;
    named->path.push_back(std::move(type_name));
    return named;
  }
  case ast::node_kind::binary_expr: {
    const auto &bin = dynamic_cast<const ast::binary_expr &>(node);
    if (bin.lhs == nullptr || bin.rhs == nullptr) {
      return nullptr;
    }
    auto lhs = materialize_quote(*bin.lhs);
    auto rhs = materialize_quote(*bin.rhs);
    if (lhs == nullptr || rhs == nullptr) {
      return nullptr;
    }
    auto cloned = ast::make<ast::binary_expr>();
    cloned->span = bin.span;
    cloned->op = bin.op;
    cloned->lhs = as_owned<ast::expr>(std::move(lhs));
    cloned->rhs = as_owned<ast::expr>(std::move(rhs));
    return cloned;
  }
  case ast::node_kind::named_type: {
    const auto &named = dynamic_cast<const ast::named_type &>(node);
    auto cloned = ast::make<ast::named_type>();
    cloned->span = named.span;
    cloned->path = named.path;
    // `type_args` deliberately not deep-cloned — always empty for every
    // shape this narrow materializer's callers (`make_adder`-style helpers,
    // `derive_show`) produce; a generic type reference would need broader
    // support than this milestone offers.
    return cloned;
  }
  case ast::node_kind::ref_type: {
    const auto &ref = dynamic_cast<const ast::ref_type &>(node);
    if (ref.inner == nullptr) {
      return nullptr;
    }
    auto inner_clone = materialize_quote(*ref.inner);
    if (inner_clone == nullptr) {
      return nullptr;
    }
    auto cloned = ast::make<ast::ref_type>();
    cloned->span = ref.span;
    cloned->is_mut = ref.is_mut;
    cloned->inner = as_owned<ast::type_expr>(std::move(inner_clone));
    return cloned;
  }
  case ast::node_kind::return_stmt: {
    const auto &ret = dynamic_cast<const ast::return_stmt &>(node);
    auto cloned = ast::make<ast::return_stmt>();
    cloned->span = ret.span;
    if (ret.value != nullptr) {
      auto value_clone = materialize_quote(*ret.value);
      if (value_clone == nullptr) {
        return nullptr;
      }
      cloned->value = as_owned<ast::expr>(std::move(value_clone));
    }
    return cloned;
  }
  case ast::node_kind::func_decl: {
    const auto &fn = dynamic_cast<const ast::func_decl &>(node);
    auto cloned = ast::make<ast::func_decl>();
    cloned->span = fn.span;
    cloned->visibility = fn.visibility;
    cloned->modifiers.is_pure = fn.modifiers.is_pure;
    cloned->modifiers.is_async = fn.modifiers.is_async;
    cloned->modifiers.is_machine = fn.modifiers.is_machine;
    cloned->modifiers.is_static = fn.modifiers.is_static;
    cloned->modifiers.is_intrinsic = fn.modifiers.is_intrinsic;
    // `modifiers.async_context`/`type_params`/`where_constraints`/
    // `contracts` deliberately left default-empty: none of this
    // materializer's callers produce a method using any of them.
    cloned->name = fn.name;
    for (const auto &orig_param : fn.params) {
      auto cloned_param = ast::param{};
      cloned_param.span = orig_param.span;
      if (orig_param.pattern != nullptr &&
          orig_param.pattern->kind == ast::node_kind::binding_pattern) {
        const auto &binding =
            dynamic_cast<const ast::binding_pattern &>(*orig_param.pattern);
        auto cloned_pattern = ast::make<ast::binding_pattern>();
        cloned_pattern->span = binding.span;
        cloned_pattern->name = binding.name;
        cloned_pattern->is_mut = binding.is_mut;
        cloned_param.pattern = std::move(cloned_pattern);
      }
      if (orig_param.type_annotation != nullptr) {
        auto type_clone = materialize_quote(*orig_param.type_annotation);
        if (type_clone == nullptr) {
          return nullptr;
        }
        cloned_param.type_annotation =
            as_owned<ast::type_expr>(std::move(type_clone));
      }
      // `default_value` deliberately left null: no parameter shape this
      // materializer's callers produce (a plain `self`, or `other: &self`)
      // has one.
      cloned->params.push_back(std::move(cloned_param));
    }
    if (fn.return_type != nullptr) {
      auto return_type_clone = materialize_quote(*fn.return_type);
      if (return_type_clone == nullptr) {
        return nullptr;
      }
      cloned->return_type =
          as_owned<ast::type_expr>(std::move(return_type_clone));
    }
    for (const auto &stmt : fn.body_stmts) {
      if (stmt == nullptr) {
        continue;
      }
      auto stmt_clone = materialize_quote(*stmt);
      if (stmt_clone == nullptr) {
        return nullptr;
      }
      cloned->body_stmts.push_back(std::move(stmt_clone));
    }
    return cloned;
  }
  case ast::node_kind::impl_decl: {
    const auto &impl = dynamic_cast<const ast::impl_decl &>(node);
    auto cloned = ast::make<ast::impl_decl>();
    cloned->span = impl.span;
    if (impl.trait_type != nullptr) {
      auto trait_clone = materialize_quote(*impl.trait_type);
      if (trait_clone == nullptr) {
        return nullptr;
      }
      cloned->trait_type = as_owned<ast::type_expr>(std::move(trait_clone));
    }
    if (impl.for_type != nullptr) {
      auto for_clone = materialize_quote(*impl.for_type);
      if (for_clone == nullptr) {
        return nullptr;
      }
      cloned->for_type = as_owned<ast::type_expr>(std::move(for_clone));
    }
    // `type_params`/`where_constraints` deliberately left empty — not used
    // by any shape this materializer's callers produce.
    for (const auto &item : impl.items) {
      if (item == nullptr) {
        continue;
      }
      auto item_clone = materialize_quote(*item);
      if (item_clone == nullptr) {
        return nullptr;
      }
      cloned->items.push_back(std::move(item_clone));
    }
    return cloned;
  }
  default:
    report(node.span, "this quoted fragment's syntax is too complex for a "
                      "nested `~` splice inside it to be resolved eagerly");
    return nullptr;
  }
}

auto evaluator::call_function(
    const ast::func_decl &fn, const std::string &name, std::vector<value> args,
    source_span span, std::vector<std::pair<std::string, value>> type_args)
    -> value {
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
  auto scope = std::unordered_map<std::string, value>{};
  for (auto &[type_param_name, type_arg] : type_args) {
    scope.insert_or_assign(type_param_name, std::move(type_arg));
  }
  ++call_depth_;
  push_locals({});
  for (size_t i = 0; i < fn.params.size(); ++i) {
    const auto &param = fn.params[i];
    auto arg_value = value{};
    if (i < args.size()) {
      arg_value = std::move(args[i]);
    } else if (param.default_value != nullptr) {
      arg_value = evaluate(*param.default_value);
    } else {
      pop_locals();
      --call_depth_;
      return report(span,
                    std::format("`{}` is missing a required argument", name));
    }
    if (arg_value.is_error()) {
      pop_locals();
      --call_depth_;
      return arg_value;
    }
    if (param.pattern != nullptr &&
        !bind_pattern(*param.pattern, arg_value, scope)) {
      pop_locals();
      --call_depth_;
      return value::make_error();
    }
  }
  locals_.back() = std::move(scope);

  auto result = value::make_unit();
  if (fn.body_expr != nullptr) {
    result = evaluate(*fn.body_expr);
  } else {
    const auto exec = evaluate_stmts(fn.body_stmts);
    if (exec.errored) {
      result = value::make_error();
    } else if (exec.returned) {
      result = exec.result;
    }
  }
  pop_locals();
  --call_depth_;
  return result;
}

auto evaluator::clone_expr_fragment(const ast::node &node)
    -> ast::ptr<ast::expr> {
  switch (node.kind) {
  case ast::node_kind::binary_expr: {
    const auto &bin = dynamic_cast<const ast::binary_expr &>(node);
    if (bin.lhs == nullptr || bin.rhs == nullptr) {
      return nullptr;
    }
    auto cloned_lhs = clone_expr_fragment(*bin.lhs);
    auto cloned_rhs = clone_expr_fragment(*bin.rhs);
    if (cloned_lhs == nullptr || cloned_rhs == nullptr) {
      return nullptr;
    }
    auto cloned = ast::make<ast::binary_expr>();
    cloned->span = bin.span;
    cloned->op = bin.op;
    cloned->lhs = std::move(cloned_lhs);
    cloned->rhs = std::move(cloned_rhs);
    return cloned;
  }
  case ast::node_kind::ident_expr: {
    const auto &ident = dynamic_cast<const ast::ident_expr &>(node);
    auto cloned = ast::make<ast::ident_expr>();
    cloned->span = ident.span;
    cloned->name = ident.name;
    return cloned;
  }
  case ast::node_kind::literal_expr: {
    const auto &lit = dynamic_cast<const ast::literal_expr &>(node);
    auto cloned = ast::make<ast::literal_expr>();
    cloned->span = lit.span;
    cloned->lit_kind = lit.lit_kind;
    cloned->value = lit.value;
    return cloned;
  }
  case ast::node_kind::field_expr: {
    const auto &fld = dynamic_cast<const ast::field_expr &>(node);
    if (fld.object == nullptr) {
      return nullptr;
    }
    auto cloned_object = clone_expr_fragment(*fld.object);
    if (cloned_object == nullptr) {
      return nullptr;
    }
    auto cloned = ast::make<ast::field_expr>();
    cloned->span = fld.span;
    cloned->object = std::move(cloned_object);
    cloned->field_name = fld.field_name;
    return cloned;
  }
  case ast::node_kind::interpolated_string_expr: {
    const auto &interp =
        dynamic_cast<const ast::interpolated_string_expr &>(node);
    auto cloned = ast::make<ast::interpolated_string_expr>();
    cloned->span = interp.span;
    for (const auto &segment : interp.segments) {
      auto cloned_segment = clone_interp_segment(segment);
      if (!cloned_segment.has_value()) {
        return nullptr;
      }
      cloned->segments.push_back(std::move(*cloned_segment));
    }
    return cloned;
  }
  default:
    return nullptr;
  }
}

/// Appends the segments needed to represent `fragment` as part of a larger
/// `interpolated_string_expr` being built by `expr.interp_concat`: a plain
/// string literal contributes one literal-text segment (decoded from its
/// source spelling); an existing `interpolated_string_expr` contributes a
/// deep clone of each of its own segments (so `interp_concat` composes:
/// merging two already-merged fragments flattens rather than nesting);
/// anything else (e.g. a `self.x` field access built by `expr.field`)
/// contributes one value segment, formatted at runtime the same way an
/// ordinary `"{x}"` interpolation would format it. Returns `false` (after
/// reporting) if `fragment`'s shape can't be represented at all.
auto evaluator::append_interp_segments(const ast::node &fragment,
                                       source_span span,
                                       std::vector<ast::interp_segment> &out)
    -> bool {
  if (fragment.kind == ast::node_kind::literal_expr) {
    const auto &lit = dynamic_cast<const ast::literal_expr &>(fragment);
    if (lit.lit_kind == token_kind::string_lit) {
      auto decoded = decode_string_literal(lit.value);
      if (!decoded.has_value()) {
        report(span, "`expr.interp_concat` was given a string literal with "
                     "an invalid escape sequence");
        return false;
      }
      out.push_back(ast::interp_segment{.is_literal = true,
                                        .literal_text = std::move(*decoded)});
      return true;
    }
  }
  if (fragment.kind == ast::node_kind::interpolated_string_expr) {
    const auto &interp =
        dynamic_cast<const ast::interpolated_string_expr &>(fragment);
    for (const auto &segment : interp.segments) {
      auto cloned_segment = clone_interp_segment(segment);
      if (!cloned_segment.has_value()) {
        report(span, "`expr.interp_concat` could not clone a nested "
                     "interpolation value");
        return false;
      }
      out.push_back(std::move(*cloned_segment));
    }
    return true;
  }
  auto cloned = clone_expr_fragment(fragment);
  if (cloned == nullptr) {
    report(span, "`expr.interp_concat`'s argument's syntax is too complex "
                 "to embed (only identifiers, field access, literals, and "
                 "other interpolated strings are supported)");
    return false;
  }
  out.push_back(
      ast::interp_segment{.is_literal = false, .value = std::move(cloned)});
  return true;
}

auto evaluator::clone_interp_segment(const ast::interp_segment &segment)
    -> std::optional<ast::interp_segment> {
  auto cloned = ast::interp_segment{};
  cloned.is_literal = segment.is_literal;
  cloned.literal_text = segment.literal_text;
  if (!segment.is_literal) {
    if (segment.value == nullptr) {
      return std::nullopt;
    }
    cloned.value = clone_expr_fragment(*segment.value);
    if (cloned.value == nullptr) {
      return std::nullopt;
    }
  }
  cloned.self_doc = segment.self_doc;
  cloned.source_text = segment.source_text;
  cloned.has_spec = segment.has_spec;
  if (segment.has_spec) {
    cloned.spec.fill = segment.spec.fill;
    cloned.spec.align = segment.spec.align;
    cloned.spec.sign = segment.spec.sign;
    cloned.spec.alternate = segment.spec.alternate;
    cloned.spec.zero_pad = segment.spec.zero_pad;
    cloned.spec.has_explicit_align = segment.spec.has_explicit_align;
    cloned.spec.type_char = segment.spec.type_char;
    cloned.spec.span = segment.spec.span;
    // `width`/`precision` only carried over when literal (`monostate`/
    // `size_t`) — a dynamic `{expr}` width/precision is dropped rather than
    // cloned, since no AST-builder intrinsic in this milestone ever
    // produces one and this narrow materializer has no general fallback
    // for an arbitrary pre-existing dynamic spec.
    if (std::holds_alternative<size_t>(segment.spec.width)) {
      cloned.spec.width = std::get<size_t>(segment.spec.width);
    }
    if (std::holds_alternative<size_t>(segment.spec.precision)) {
      cloned.spec.precision = std::get<size_t>(segment.spec.precision);
    }
  }
  return cloned;
}

auto evaluator::try_eval_expr_builder_call(const ast::call_expr &call)
    -> std::optional<value> {
  if (call.callee == nullptr ||
      call.callee->kind != ast::node_kind::field_expr) {
    return std::nullopt;
  }
  const auto &field = dynamic_cast<const ast::field_expr &>(*call.callee);
  if (field.object == nullptr ||
      field.object->kind != ast::node_kind::ident_expr) {
    return std::nullopt;
  }
  if (dynamic_cast<const ast::ident_expr &>(*field.object).name != "expr") {
    return std::nullopt;
  }

  if (field.field_name == "lit") {
    if (call.args.size() != 1 || call.args.front().value == nullptr) {
      return report(call.span, "`expr.lit` takes exactly one argument");
    }
    auto arg = evaluate(*call.args.front().value);
    if (arg.is_error()) {
      return arg;
    }
    auto lit = ast::make<ast::literal_expr>();
    lit->span = call.span;
    switch (arg.kind) {
    case value_kind::integer:
      lit->lit_kind = token_kind::int_lit;
      lit->value = std::to_string(arg.integer);
      break;
    case value_kind::floating:
      lit->lit_kind = token_kind::float_lit;
      lit->value = std::to_string(arg.floating);
      break;
    case value_kind::boolean:
      lit->lit_kind = arg.boolean ? token_kind::kw_true : token_kind::kw_false;
      lit->value = arg.boolean ? "true" : "false";
      break;
    case value_kind::string:
      lit->lit_kind = token_kind::string_lit;
      lit->value = encode_string_literal(arg.string);
      break;
    default:
      return report(call.args.front().value->span,
                    "`expr.lit` only supports integer, floating-point, "
                    "boolean, and string values");
    }
    const auto *raw = lit.get();
    synthesized_fragments_.push_back(std::move(lit));
    return value::make_expr_fragment(raw);
  }

  if (field.field_name == "ident") {
    if (call.args.size() != 1 || call.args.front().value == nullptr) {
      return report(call.span, "`expr.ident` takes exactly one argument");
    }
    auto arg = evaluate(*call.args.front().value);
    if (arg.is_error()) {
      return arg;
    }
    if (arg.kind != value_kind::string) {
      return report(call.args.front().value->span,
                    "`expr.ident` expects a string naming the identifier");
    }
    auto ident = ast::make<ast::ident_expr>();
    // `is_variant_ident` (`check.cpp`) distinguishes a `@variant`-sigil
    // reference from a plain identifier purely by comparing `span.len()`
    // against `name.size()` — an original `@p` token's span covers 2 bytes
    // for a 1-byte `name`. Giving this synthesized node the whole call
    // expression's (much longer) span would spuriously trip that check for
    // any short name, so its span must have exactly `name`'s length.
    ident->span = source_span{
        .start = call.span.start,
        .end = call.span.start + static_cast<byte_offset>(arg.string.size())};
    ident->name = arg.string;
    const auto *raw = ident.get();
    synthesized_fragments_.push_back(std::move(ident));
    return value::make_expr_fragment(raw);
  }

  if (field.field_name == "field") {
    if (call.args.size() != 2 || call.args[0].value == nullptr ||
        call.args[1].value == nullptr) {
      return report(call.span, "`expr.field` takes exactly two arguments: "
                               "an `expr` object and a field-name string");
    }
    auto object_arg = evaluate(*call.args[0].value);
    if (object_arg.is_error()) {
      return object_arg;
    }
    if (object_arg.kind != value_kind::expr_fragment ||
        object_arg.fragment == nullptr) {
      return report(call.args[0].value->span,
                    "`expr.field`'s first argument must be a quoted or "
                    "constructed `expr` value");
    }
    auto name_arg = evaluate(*call.args[1].value);
    if (name_arg.is_error()) {
      return name_arg;
    }
    if (name_arg.kind != value_kind::string) {
      return report(call.args[1].value->span,
                    "`expr.field`'s second argument must be a string naming "
                    "the field");
    }
    auto cloned_object = clone_expr_fragment(*object_arg.fragment);
    if (cloned_object == nullptr) {
      return report(call.args[0].value->span,
                    "this quoted `expr` value's syntax is too complex for "
                    "`expr.field` to embed (only identifiers, field access, "
                    "and literals are supported)");
    }
    auto result = ast::make<ast::field_expr>();
    result->span = call.span;
    result->object = std::move(cloned_object);
    result->field_name = name_arg.string;
    const auto *raw = result.get();
    synthesized_fragments_.push_back(std::move(result));
    return value::make_expr_fragment(raw);
  }

  if (field.field_name == "interp_concat") {
    if (call.args.size() != 2 || call.args[0].value == nullptr ||
        call.args[1].value == nullptr) {
      return report(call.span, "`expr.interp_concat` takes exactly two "
                               "`expr` arguments");
    }
    auto lhs = evaluate(*call.args[0].value);
    if (lhs.is_error()) {
      return lhs;
    }
    if (lhs.kind != value_kind::expr_fragment || lhs.fragment == nullptr) {
      return report(call.args[0].value->span,
                    "`expr.interp_concat`'s arguments must be quoted or "
                    "constructed `expr` values");
    }
    auto rhs = evaluate(*call.args[1].value);
    if (rhs.is_error()) {
      return rhs;
    }
    if (rhs.kind != value_kind::expr_fragment || rhs.fragment == nullptr) {
      return report(call.args[1].value->span,
                    "`expr.interp_concat`'s arguments must be quoted or "
                    "constructed `expr` values");
    }
    auto result = ast::make<ast::interpolated_string_expr>();
    result->span = call.span;
    if (!append_interp_segments(*lhs.fragment, call.args[0].value->span,
                                result->segments) ||
        !append_interp_segments(*rhs.fragment, call.args[1].value->span,
                                result->segments)) {
      return value::make_error();
    }
    const auto *raw = result.get();
    synthesized_fragments_.push_back(std::move(result));
    return value::make_expr_fragment(raw);
  }

  if (field.field_name == "debug") {
    if (call.args.size() != 1 || call.args.front().value == nullptr) {
      return report(call.span, "`expr.debug` takes exactly one `expr` "
                               "argument");
    }
    auto arg = evaluate(*call.args.front().value);
    if (arg.is_error()) {
      return arg;
    }
    if (arg.kind != value_kind::expr_fragment || arg.fragment == nullptr) {
      return report(call.args.front().value->span,
                    "`expr.debug`'s argument must be a quoted or "
                    "constructed `expr` value");
    }
    auto cloned = clone_expr_fragment(*arg.fragment);
    if (cloned == nullptr) {
      return report(call.args.front().value->span,
                    "this quoted `expr` value's syntax is too complex for "
                    "`expr.debug` to embed");
    }
    auto result = ast::make<ast::interpolated_string_expr>();
    result->span = call.span;
    auto segment = ast::interp_segment{};
    segment.is_literal = false;
    segment.value = std::move(cloned);
    // A single value segment formatted with the `?` debug type char — the
    // same shape `"{x:?}"` produces at parse time, just built directly
    // instead of parsed, so a value embedded via `expr.debug` renders with
    // `.debug()`-style formatting instead of the `interp_concat` default
    // (`.show()`-style) when the surrounding string is finally assembled.
    segment.has_spec = true;
    segment.spec.type_char = '?';
    result->segments.push_back(std::move(segment));
    const auto *raw = result.get();
    synthesized_fragments_.push_back(std::move(result));
    return value::make_expr_fragment(raw);
  }

  if (field.field_name == "binary") {
    if (call.args.size() != 3 || call.args[0].value == nullptr ||
        call.args[1].value == nullptr || call.args[2].value == nullptr) {
      return report(call.span, "`expr.binary` takes exactly three "
                               "arguments: an operator name string and two "
                               "`expr` operands");
    }
    auto op_arg = evaluate(*call.args[0].value);
    if (op_arg.is_error()) {
      return op_arg;
    }
    if (op_arg.kind != value_kind::string) {
      return report(call.args[0].value->span,
                    "`expr.binary`'s first argument must be a string "
                    "naming the operator");
    }
    // Deliberately narrow — only the operators `derive_eq`/a similar
    // caller actually needs, not a general mirror of `ast::binary_op`.
    const auto op = op_arg.string == "=="    ? ast::binary_op::eq_eq
                    : op_arg.string == "!="  ? ast::binary_op::bang_eq
                    : op_arg.string == "and" ? ast::binary_op::logical_and
                    : op_arg.string == "or"  ? ast::binary_op::logical_or
                                             : std::optional<ast::binary_op>{};
    if (!op.has_value()) {
      return report(call.args[0].value->span,
                    std::format("`expr.binary` does not recognize the "
                                "operator `{}` (supported: `==`, `!=`, "
                                "`and`, `or`)",
                                op_arg.string));
    }
    auto lhs = evaluate(*call.args[1].value);
    if (lhs.is_error()) {
      return lhs;
    }
    if (lhs.kind != value_kind::expr_fragment || lhs.fragment == nullptr) {
      return report(call.args[1].value->span,
                    "`expr.binary`'s operands must be quoted or "
                    "constructed `expr` values");
    }
    auto rhs = evaluate(*call.args[2].value);
    if (rhs.is_error()) {
      return rhs;
    }
    if (rhs.kind != value_kind::expr_fragment || rhs.fragment == nullptr) {
      return report(call.args[2].value->span,
                    "`expr.binary`'s operands must be quoted or "
                    "constructed `expr` values");
    }
    auto cloned_lhs = clone_expr_fragment(*lhs.fragment);
    auto cloned_rhs = clone_expr_fragment(*rhs.fragment);
    if (cloned_lhs == nullptr || cloned_rhs == nullptr) {
      return report(call.span, "one of `expr.binary`'s operands' syntax is too "
                               "complex to embed");
    }
    auto result = ast::make<ast::binary_expr>();
    result->span = call.span;
    result->op = *op;
    result->lhs = std::move(cloned_lhs);
    result->rhs = std::move(cloned_rhs);
    const auto *raw = result.get();
    synthesized_fragments_.push_back(std::move(result));
    return value::make_expr_fragment(raw);
  }

  return report(call.span,
                std::format("`expr.{}` is not a recognized AST-builder "
                            "intrinsic (only `expr.lit`/`expr.ident`/"
                            "`expr.field`/`expr.interp_concat`/`expr.debug`/"
                            "`expr.binary` are supported)",
                            field.field_name));
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

auto evaluator::try_eval_comptime_generic_call(const ast::call_expr &call)
    -> std::optional<value> {
  if (call.callee == nullptr ||
      call.callee->kind != ast::node_kind::index_expr) {
    return std::nullopt;
  }
  const auto &index = dynamic_cast<const ast::index_expr &>(*call.callee);
  if (index.object == nullptr || index.index == nullptr ||
      index.object->kind != ast::node_kind::ident_expr) {
    return std::nullopt;
  }
  const auto &callee_ident =
      dynamic_cast<const ast::ident_expr &>(*index.object);
  const ast::func_decl *fn = nullptr;
  if (const auto it = pending_functions_.find(callee_ident.name);
      it != pending_functions_.end()) {
    fn = it->second;
  }
  if (fn == nullptr || fn->type_params.empty()) {
    return std::nullopt;
  }
  if (index.index->kind != ast::node_kind::ident_expr) {
    return report(call.span, "a compile-time generic call's type argument "
                             "must be a plain type name");
  }
  const auto &type_arg_ident =
      dynamic_cast<const ast::ident_expr &>(*index.index);
  const auto *type_decl = resolve_type_reference(type_arg_ident);
  if (type_decl == nullptr) {
    return report(type_arg_ident.span,
                  std::format("`{}` does not name a known type here",
                              type_arg_ident.name));
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
  auto type_arg = value::make_type_value(type_arg_ident.name, type_decl);
  return call_function(*fn, callee_ident.name, std::move(args), call.span,
                       {{fn->type_params.front().name, std::move(type_arg)}});
}

auto evaluator::eval_call(const ast::call_expr &call) -> value {
  if (call.callee == nullptr) {
    return value::make_error();
  }
  if (auto builder = try_eval_expr_builder_call(call)) {
    return *builder;
  }
  if (auto reflected = try_eval_type_reflection_call(call)) {
    return *reflected;
  }
  if (auto generic_call = try_eval_comptime_generic_call(call)) {
    return *generic_call;
  }
  const auto *callee_ident =
      dynamic_cast<const ast::ident_expr *>(call.callee.get());
  if (callee_ident == nullptr) {
    return report(call.span, "only direct calls to a named `static def` "
                             "function are supported in compile-time "
                             "evaluation");
  }
  const ast::func_decl *fn = nullptr;
  if (const auto it = pending_functions_.find(callee_ident->name);
      it != pending_functions_.end()) {
    fn = it->second;
  } else if (const auto *local = lookup_local(callee_ident->name);
             local != nullptr && local->kind == value_kind::closure) {
    fn = local->function;
  } else if (const auto git = globals_.find(callee_ident->name);
             git != globals_.end() && git->second.kind == value_kind::closure) {
    fn = git->second.function;
  }
  if (fn == nullptr) {
    return report(call.span,
                  std::format("`{}` is not a `static def` function that can "
                              "be called at compile time",
                              callee_ident->name));
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
  return call_function(*fn, callee_ident->name, std::move(args), call.span);
}

auto evaluator::bind_pattern(const ast::pattern &pattern, const value &v,
                             std::unordered_map<std::string, value> &scope)
    -> bool {
  switch (pattern.kind) {
  case ast::node_kind::wildcard_pattern:
    return true;
  case ast::node_kind::binding_pattern: {
    const auto &binding = dynamic_cast<const ast::binding_pattern &>(pattern);
    scope.insert_or_assign(binding.name, v);
    return true;
  }
  case ast::node_kind::group_pattern: {
    const auto &group = dynamic_cast<const ast::group_pattern &>(pattern);
    if (group.inner == nullptr || !bind_pattern(*group.inner, v, scope)) {
      return false;
    }
    if (group.alias.has_value()) {
      scope.insert_or_assign(*group.alias, v);
    }
    return true;
  }
  case ast::node_kind::tuple_pattern: {
    const auto &tup = dynamic_cast<const ast::tuple_pattern &>(pattern);
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
    const auto &st = dynamic_cast<const ast::struct_pattern &>(pattern);
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
  default:
    report(pattern.span, "this pattern form is not yet supported in "
                         "compile-time evaluation");
    return false;
  }
}

auto evaluator::evaluate_iterable(const ast::expr &iterable) -> value {
  if (const auto *bin = dynamic_cast<const ast::binary_expr *>(&iterable);
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
    return report(iterable.span, "`static for` requires a compile-time list "
                                 "or range value to iterate over");
  }
  return evaluated;
}

auto evaluator::evaluate_stmt(const ast::node &node) -> exec_result {
  switch (node.kind) {
  case ast::node_kind::let_stmt: {
    const auto &let = dynamic_cast<const ast::let_stmt &>(node);
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
    const auto &var = dynamic_cast<const ast::var_stmt &>(node);
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
    const auto &assign = dynamic_cast<const ast::assign_stmt &>(node);
    const auto *target =
        dynamic_cast<const ast::ident_expr *>(assign.target.get());
    if (target == nullptr || assign.op != ast::assign_op::assign ||
        assign.value == nullptr) {
      report(node.span, "this assignment form is not yet supported in "
                        "compile-time evaluation");
      return exec_result{.errored = true};
    }
    auto new_value = evaluate(*assign.value);
    if (new_value.is_error()) {
      return exec_result{.errored = true};
    }
    for (auto &scope : locals_ | std::views::reverse) {
      if (scope.contains(target->name)) {
        scope.insert_or_assign(target->name, std::move(new_value));
        return exec_result{};
      }
    }
    report(node.span, std::format("`{}` is not a known compile-time local "
                                  "variable",
                                  target->name));
    return exec_result{.errored = true};
  }
  case ast::node_kind::expr_stmt: {
    const auto &wrapper = dynamic_cast<const ast::expr_stmt &>(node);
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
    const auto &ret = dynamic_cast<const ast::return_stmt &>(node);
    if (ret.value == nullptr) {
      return exec_result{.returned = true, .result = value::make_unit()};
    }
    auto evaluated = evaluate(*ret.value);
    if (evaluated.is_error()) {
      return exec_result{.errored = true};
    }
    return exec_result{.returned = true, .result = std::move(evaluated)};
  }
  case ast::node_kind::if_stmt: {
    const auto &stmt = dynamic_cast<const ast::if_stmt &>(node);
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
  case ast::node_kind::static_decl: {
    const auto &decl = dynamic_cast<const ast::static_decl &>(node);
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
    case ast::static_decl_kind::for_block: {
      // Mirrors `checker::check_static_decl`'s own top-level `static for`
      // iteration exactly (`check.cpp`) — that copy only runs for a `static
      // for` written directly as a module item; this one is what makes the
      // *same* construct work when nested inside a `static def`'s own body
      // (e.g. `derive_show[T]()`'s per-field loop), which previously fell
      // through to the `default` case below and silently did nothing. Only
      // a single loop pattern is supported, matching the same pre-existing
      // restriction.
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
          }
          pop_locals();
        } else {
          const auto exec = evaluate_stmts(decl.for_body);
          pop_locals();
          if (exec.errored || exec.returned) {
            return exec;
          }
        }
      }
      return exec_result{};
    }
    default:
      return exec_result{};
    }
  }
  default:
    report(node.span, "this statement form is not yet supported in "
                      "compile-time evaluation");
    return exec_result{.errored = true};
  }
}

auto evaluator::evaluate_stmts(const std::vector<ast::ptr<ast::node>> &body)
    -> exec_result {
  for (const auto &item : body) {
    if (item == nullptr || item->has_error) {
      continue;
    }
    auto result = evaluate_stmt(*item);
    if (result.errored || result.returned) {
      return result;
    }
  }
  return exec_result{};
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
  case ast::node_kind::array_expr:
    return eval_array(dynamic_cast<const ast::array_expr &>(expr));
  case ast::node_kind::tuple_expr:
    return eval_tuple(dynamic_cast<const ast::tuple_expr &>(expr));
  case ast::node_kind::struct_expr:
    return eval_struct(dynamic_cast<const ast::struct_expr &>(expr));
  case ast::node_kind::field_expr:
    return eval_field(dynamic_cast<const ast::field_expr &>(expr));
  case ast::node_kind::index_expr:
    return eval_index(dynamic_cast<const ast::index_expr &>(expr));
  case ast::node_kind::call_expr:
    return eval_call(dynamic_cast<const ast::call_expr &>(expr));
  case ast::node_kind::module_path_expr:
    return eval_module_path(dynamic_cast<const ast::module_path_expr &>(expr));
  case ast::node_kind::quote_expr:
    return eval_quote(dynamic_cast<const ast::quote_expr &>(expr));
  default:
    return report(expr.span, "this expression form is not yet supported in "
                             "compile-time evaluation");
  }
}

} // namespace kira::comptime
