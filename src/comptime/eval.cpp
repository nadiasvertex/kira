#include "src/comptime/eval.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <ranges>
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

auto evaluator::call_function(const ast::func_decl &fn, const std::string &name,
                              std::vector<value> args, source_span span)
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

auto evaluator::eval_call(const ast::call_expr &call) -> value {
  if (call.callee == nullptr) {
    return value::make_error();
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
    default:
      // Nested `static for` isn't evaluated for real yet — skip it rather
      // than fail the whole enclosing body.
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
  default:
    return report(expr.span, "this expression form is not yet supported in "
                             "compile-time evaluation");
  }
}

} // namespace kira::comptime
