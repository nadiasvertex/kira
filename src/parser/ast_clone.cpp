#include "ast_clone.h"

#include <format>

namespace kira::ast {

namespace {

[[nodiscard]] auto unsupported(const node &n, std::string_view what)
    -> std::unexpected<clone_error> {
  return std::unexpected(clone_error{
      .span = n.span,
      .message = std::format(
          "cannot clone this construct for monomorphization: {}", what)});
}

[[nodiscard]] auto clone_type_expr(const type_expr &t)
    -> std::expected<ptr<type_expr>, clone_error>;
[[nodiscard]] auto clone_expr_impl(const expr &e)
    -> std::expected<ptr<expr>, clone_error>;
[[nodiscard]] auto clone_pattern(const pattern &p)
    -> std::expected<ptr<pattern>, clone_error>;
[[nodiscard]] auto clone_node(const node &n)
    -> std::expected<ptr<node>, clone_error>;

template <typename T>
[[nodiscard]] auto clone_optional(const ptr<T> &original)
    -> std::expected<ptr<T>, clone_error> {
  if (original == nullptr) {
    return ptr<T>{};
  }
  if constexpr (std::is_same_v<T, type_expr>) {
    return clone_type_expr(*original);
  } else if constexpr (std::is_same_v<T, expr>) {
    return clone_expr_impl(*original);
  } else if constexpr (std::is_same_v<T, pattern>) {
    return clone_pattern(*original);
  } else if constexpr (std::is_same_v<T, node>) {
    return clone_node(*original);
  }
}

/// Clones a generic-argument payload (`type_arg::value`) — weakly typed at
/// parse time (`ast.h`: "disambiguate later in semantic analysis"), so it is
/// either a type (`result[usize, io_error]`) or a compile-time *value*
/// (the `n + 1` in `vec[T, n + 1]`); both occur, and both are just delegated
/// to the cloner for whichever they are.
[[nodiscard]] auto clone_type_arg_value(const node &n)
    -> std::expected<ptr<node>, clone_error> {
  if (const auto *as_type = dynamic_cast<const type_expr *>(&n)) {
    auto cloned = clone_type_expr(*as_type);
    if (!cloned.has_value()) {
      return std::unexpected(cloned.error());
    }
    return ptr<node>(std::move(*cloned));
  }
  if (const auto *as_expr = dynamic_cast<const expr *>(&n)) {
    auto cloned = clone_expr_impl(*as_expr);
    if (!cloned.has_value()) {
      return std::unexpected(cloned.error());
    }
    return ptr<node>(std::move(*cloned));
  }
  return unsupported(n, "this generic argument");
}

[[nodiscard]] auto clone_type_expr(const type_expr &t)
    -> std::expected<ptr<type_expr>, clone_error> {
  switch (t.kind) {
  case node_kind::named_type: {
    const auto &named = dynamic_cast<const named_type &>(t);
    auto cloned = make<named_type>();
    cloned->span = named.span;
    cloned->path = named.path;
    for (const auto &arg : named.type_args) {
      auto value = clone_type_arg_value(*arg.value);
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
      cloned->type_args.push_back(type_arg{
          .span = arg.span, .value = std::move(*value), .name = arg.name});
    }
    return ptr<type_expr>(std::move(cloned));
  }
  case node_kind::slice_type: {
    const auto &slice = dynamic_cast<const slice_type &>(t);
    auto element = clone_optional(slice.element);
    if (!element.has_value()) {
      return std::unexpected(element.error());
    }
    auto cloned = make<slice_type>();
    cloned->span = slice.span;
    cloned->element = std::move(*element);
    return ptr<type_expr>(std::move(cloned));
  }
  case node_kind::array_type: {
    const auto &array = dynamic_cast<const array_type &>(t);
    auto element = clone_optional(array.element);
    if (!element.has_value()) {
      return std::unexpected(element.error());
    }
    auto size = clone_optional(array.size);
    if (!size.has_value()) {
      return std::unexpected(size.error());
    }
    auto cloned = make<array_type>();
    cloned->span = array.span;
    cloned->element = std::move(*element);
    cloned->size = std::move(*size);
    return ptr<type_expr>(std::move(cloned));
  }
  case node_kind::ref_type: {
    const auto &ref = dynamic_cast<const ref_type &>(t);
    auto inner = clone_optional(ref.inner);
    if (!inner.has_value()) {
      return std::unexpected(inner.error());
    }
    auto cloned = make<ref_type>();
    cloned->span = ref.span;
    cloned->inner = std::move(*inner);
    cloned->is_mut = ref.is_mut;
    return ptr<type_expr>(std::move(cloned));
  }
  case node_kind::ptr_type: {
    const auto &raw = dynamic_cast<const ptr_type &>(t);
    auto inner = clone_optional(raw.inner);
    if (!inner.has_value()) {
      return std::unexpected(inner.error());
    }
    auto cloned = make<ptr_type>();
    cloned->span = raw.span;
    cloned->inner = std::move(*inner);
    cloned->is_mut = raw.is_mut;
    return ptr<type_expr>(std::move(cloned));
  }
  case node_kind::tuple_type: {
    const auto &tuple = dynamic_cast<const tuple_type &>(t);
    auto cloned = make<tuple_type>();
    cloned->span = tuple.span;
    for (const auto &element : tuple.elements) {
      auto cloned_element = clone_optional(element);
      if (!cloned_element.has_value()) {
        return std::unexpected(cloned_element.error());
      }
      cloned->elements.push_back(std::move(*cloned_element));
    }
    return ptr<type_expr>(std::move(cloned));
  }
  case node_kind::fn_type: {
    const auto &fn = dynamic_cast<const fn_type &>(t);
    auto cloned = make<fn_type>();
    cloned->span = fn.span;
    for (const auto &param : fn.param_types) {
      auto cloned_param = clone_optional(param);
      if (!cloned_param.has_value()) {
        return std::unexpected(cloned_param.error());
      }
      cloned->param_types.push_back(std::move(*cloned_param));
    }
    auto return_type = clone_optional(fn.return_type);
    if (!return_type.has_value()) {
      return std::unexpected(return_type.error());
    }
    cloned->return_type = std::move(*return_type);
    return ptr<type_expr>(std::move(cloned));
  }
  default:
    return unsupported(t, "this type-expression shape");
  }
}

[[nodiscard]] auto clone_pattern_list(const std::vector<ptr<pattern>> &patterns)
    -> std::expected<std::vector<ptr<pattern>>, clone_error> {
  auto cloned = std::vector<ptr<pattern>>{};
  cloned.reserve(patterns.size());
  for (const auto &p : patterns) {
    auto cloned_pattern = clone_optional(p);
    if (!cloned_pattern.has_value()) {
      return std::unexpected(cloned_pattern.error());
    }
    cloned.push_back(std::move(*cloned_pattern));
  }
  return cloned;
}

[[nodiscard]] auto clone_pattern(const pattern &p)
    -> std::expected<ptr<pattern>, clone_error> {
  switch (p.kind) {
  case node_kind::wildcard_pattern: {
    auto cloned = make<wildcard_pattern>();
    cloned->span = p.span;
    return ptr<pattern>(std::move(cloned));
  }
  case node_kind::literal_pattern: {
    const auto &lit = dynamic_cast<const literal_pattern &>(p);
    auto cloned = make<literal_pattern>();
    cloned->span = lit.span;
    cloned->lit_kind = lit.lit_kind;
    cloned->value = lit.value;
    return ptr<pattern>(std::move(cloned));
  }
  case node_kind::binding_pattern: {
    const auto &binding = dynamic_cast<const binding_pattern &>(p);
    auto cloned = make<binding_pattern>();
    cloned->span = binding.span;
    cloned->name = binding.name;
    cloned->is_mut = binding.is_mut;
    return ptr<pattern>(std::move(cloned));
  }
  case node_kind::constructor_pattern: {
    const auto &ctor = dynamic_cast<const constructor_pattern &>(p);
    auto args = clone_pattern_list(ctor.args);
    if (!args.has_value()) {
      return std::unexpected(args.error());
    }
    auto cloned = make<constructor_pattern>();
    cloned->span = ctor.span;
    cloned->name = ctor.name;
    cloned->args = std::move(*args);
    return ptr<pattern>(std::move(cloned));
  }
  case node_kind::tuple_pattern: {
    const auto &tuple = dynamic_cast<const tuple_pattern &>(p);
    auto elements = clone_pattern_list(tuple.elements);
    if (!elements.has_value()) {
      return std::unexpected(elements.error());
    }
    auto cloned = make<tuple_pattern>();
    cloned->span = tuple.span;
    cloned->elements = std::move(*elements);
    return ptr<pattern>(std::move(cloned));
  }
  case node_kind::array_pattern: {
    const auto &array = dynamic_cast<const array_pattern &>(p);
    auto elements = clone_pattern_list(array.elements);
    if (!elements.has_value()) {
      return std::unexpected(elements.error());
    }
    auto cloned = make<array_pattern>();
    cloned->span = array.span;
    cloned->elements = std::move(*elements);
    return ptr<pattern>(std::move(cloned));
  }
  case node_kind::struct_pattern: {
    const auto &object = dynamic_cast<const struct_pattern &>(p);
    auto cloned = make<struct_pattern>();
    cloned->span = object.span;
    for (const auto &field : object.fields) {
      auto sub = clone_optional(field.pattern);
      if (!sub.has_value()) {
        return std::unexpected(sub.error());
      }
      cloned->fields.push_back(field_pattern{.span = field.span,
                                             .name = field.name,
                                             .pattern = std::move(*sub),
                                             .is_rest = field.is_rest});
    }
    return ptr<pattern>(std::move(cloned));
  }
  case node_kind::range_pattern: {
    const auto &range = dynamic_cast<const range_pattern &>(p);
    auto start = clone_optional(range.start);
    if (!start.has_value()) {
      return std::unexpected(start.error());
    }
    auto end = clone_optional(range.end);
    if (!end.has_value()) {
      return std::unexpected(end.error());
    }
    auto cloned = make<range_pattern>();
    cloned->span = range.span;
    cloned->start = std::move(*start);
    cloned->end = std::move(*end);
    cloned->inclusive = range.inclusive;
    return ptr<pattern>(std::move(cloned));
  }
  case node_kind::option_pattern: {
    const auto &option = dynamic_cast<const option_pattern &>(p);
    auto inner = clone_optional(option.inner);
    if (!inner.has_value()) {
      return std::unexpected(inner.error());
    }
    auto cloned = make<option_pattern>();
    cloned->span = option.span;
    cloned->option_kind = option.option_kind;
    cloned->inner = std::move(*inner);
    return ptr<pattern>(std::move(cloned));
  }
  case node_kind::result_pattern: {
    const auto &result = dynamic_cast<const result_pattern &>(p);
    auto inner = clone_optional(result.inner);
    if (!inner.has_value()) {
      return std::unexpected(inner.error());
    }
    auto cloned = make<result_pattern>();
    cloned->span = result.span;
    cloned->result_kind = result.result_kind;
    cloned->inner = std::move(*inner);
    return ptr<pattern>(std::move(cloned));
  }
  case node_kind::ref_pattern: {
    const auto &ref = dynamic_cast<const ref_pattern &>(p);
    auto inner = clone_optional(ref.inner);
    if (!inner.has_value()) {
      return std::unexpected(inner.error());
    }
    auto cloned = make<ref_pattern>();
    cloned->span = ref.span;
    cloned->inner = std::move(*inner);
    return ptr<pattern>(std::move(cloned));
  }
  case node_kind::or_pattern: {
    const auto &alt = dynamic_cast<const or_pattern &>(p);
    auto alternatives = clone_pattern_list(alt.alternatives);
    if (!alternatives.has_value()) {
      return std::unexpected(alternatives.error());
    }
    auto cloned = make<or_pattern>();
    cloned->span = alt.span;
    cloned->alternatives = std::move(*alternatives);
    return ptr<pattern>(std::move(cloned));
  }
  case node_kind::group_pattern: {
    const auto &group = dynamic_cast<const group_pattern &>(p);
    auto inner = clone_optional(group.inner);
    if (!inner.has_value()) {
      return std::unexpected(inner.error());
    }
    auto cloned = make<group_pattern>();
    cloned->span = group.span;
    cloned->inner = std::move(*inner);
    cloned->alias = group.alias;
    return ptr<pattern>(std::move(cloned));
  }
  default:
    return unsupported(p, "this pattern shape");
  }
}

[[nodiscard]] auto clone_call_args(const std::vector<call_arg> &args)
    -> std::expected<std::vector<call_arg>, clone_error> {
  auto cloned = std::vector<call_arg>{};
  cloned.reserve(args.size());
  for (const auto &arg : args) {
    auto value = clone_optional(arg.value);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    cloned.push_back(call_arg{
        .span = arg.span, .name = arg.name, .value = std::move(*value)});
  }
  return cloned;
}

[[nodiscard]] auto clone_node_list(const std::vector<ptr<node>> &nodes)
    -> std::expected<std::vector<ptr<node>>, clone_error>;

/// Deep-clones one of a format spec's width/precision slots, which is unset,
/// a literal count, or a *dynamic* `{expr}` — the last of which owns an
/// expression, and is why a `format_spec` cannot simply be copied.
[[nodiscard]] auto
clone_format_count(const std::variant<std::monostate, size_t, ptr<expr>> &slot)
    -> std::expected<std::variant<std::monostate, size_t, ptr<expr>>,
                     clone_error> {
  if (const auto *count = std::get_if<size_t>(&slot)) {
    return *count;
  }
  if (const auto *dynamic = std::get_if<ptr<expr>>(&slot);
      dynamic != nullptr && *dynamic != nullptr) {
    auto cloned = clone_expr_impl(**dynamic);
    if (!cloned.has_value()) {
      return std::unexpected(cloned.error());
    }
    return std::move(*cloned);
  }
  return std::monostate{};
}

/// Deep-clones an interpolation segment's format spec.
[[nodiscard]] auto clone_format_spec(const format_spec &spec)
    -> std::expected<format_spec, clone_error> {
  auto width = clone_format_count(spec.width);
  if (!width.has_value()) {
    return std::unexpected(width.error());
  }
  auto precision = clone_format_count(spec.precision);
  if (!precision.has_value()) {
    return std::unexpected(precision.error());
  }
  auto cloned = format_spec{};
  cloned.fill = spec.fill;
  cloned.align = spec.align;
  cloned.sign = spec.sign;
  cloned.alternate = spec.alternate;
  cloned.zero_pad = spec.zero_pad;
  cloned.has_explicit_align = spec.has_explicit_align;
  cloned.width = std::move(*width);
  cloned.precision = std::move(*precision);
  cloned.type_char = spec.type_char;
  cloned.span = spec.span;
  return cloned;
}

/// Clones the `if`/`elif` branch list shared by `if_stmt` and `if_expr`,
/// including the `if let` form — a branch is a pattern *or* a condition, and
/// the two shapes are otherwise identical.
[[nodiscard]] auto clone_if_branches(const std::vector<if_branch> &branches)
    -> std::expected<std::vector<if_branch>, clone_error> {
  auto cloned = std::vector<if_branch>{};
  cloned.reserve(branches.size());
  for (const auto &branch : branches) {
    auto condition = clone_optional(branch.condition);
    if (!condition.has_value()) {
      return std::unexpected(condition.error());
    }
    auto let_pattern = clone_optional(branch.let_pattern);
    if (!let_pattern.has_value()) {
      return std::unexpected(let_pattern.error());
    }
    auto let_expr = clone_optional(branch.let_expr);
    if (!let_expr.has_value()) {
      return std::unexpected(let_expr.error());
    }
    auto body = clone_node_list(branch.body);
    if (!body.has_value()) {
      return std::unexpected(body.error());
    }
    cloned.push_back(if_branch{.span = branch.span,
                               .condition = std::move(*condition),
                               .let_pattern = std::move(*let_pattern),
                               .let_expr = std::move(*let_expr),
                               .body = std::move(*body)});
  }
  return cloned;
}

/// Clones the arm list shared by `match_stmt` and `match_expr`.
[[nodiscard]] auto clone_match_arms(const std::vector<match_arm> &arms)
    -> std::expected<std::vector<match_arm>, clone_error> {
  auto cloned = std::vector<match_arm>{};
  cloned.reserve(arms.size());
  for (const auto &arm : arms) {
    auto pattern = clone_optional(arm.pattern);
    if (!pattern.has_value()) {
      return std::unexpected(pattern.error());
    }
    auto guard = clone_optional(arm.guard);
    if (!guard.has_value()) {
      return std::unexpected(guard.error());
    }
    auto body_expr = clone_optional(arm.body_expr);
    if (!body_expr.has_value()) {
      return std::unexpected(body_expr.error());
    }
    auto body_stmts = clone_node_list(arm.body_stmts);
    if (!body_stmts.has_value()) {
      return std::unexpected(body_stmts.error());
    }
    cloned.push_back(match_arm{.span = arm.span,
                               .pattern = std::move(*pattern),
                               .guard = std::move(*guard),
                               .body_expr = std::move(*body_expr),
                               .body_stmts = std::move(*body_stmts),
                               .has_error = arm.has_error});
  }
  return cloned;
}

[[nodiscard]] auto clone_expr_impl(const expr &e)
    -> std::expected<ptr<expr>, clone_error> {
  // An `error_expr` is *spelled* `node_kind::ident_expr` (see its constructor
  // in ast.h), so it has to be recognized before the switch or it would be
  // cast to an `ident_expr` it isn't. It is not always a parse failure: an
  // `if let` branch parks one in `if_branch::condition`, the slot its pattern
  // and scrutinee replace, so cloning any `if let` at all reaches this.
  if (const auto *error = dynamic_cast<const error_expr *>(&e)) {
    return ptr<expr>(make<error_expr>(error->span, error->description));
  }

  switch (e.kind) {
  case node_kind::ident_expr: {
    const auto &ident = dynamic_cast<const ident_expr &>(e);
    auto cloned = make<ident_expr>();
    cloned->span = ident.span;
    cloned->name = ident.name;
    return ptr<expr>(std::move(cloned));
  }
  case node_kind::literal_expr: {
    const auto &lit = dynamic_cast<const literal_expr &>(e);
    auto cloned = make<literal_expr>();
    cloned->span = lit.span;
    cloned->lit_kind = lit.lit_kind;
    cloned->value = lit.value;
    return ptr<expr>(std::move(cloned));
  }
  case node_kind::binary_expr: {
    const auto &bin = dynamic_cast<const binary_expr &>(e);
    auto lhs = clone_optional(bin.lhs);
    if (!lhs.has_value()) {
      return std::unexpected(lhs.error());
    }
    auto rhs = clone_optional(bin.rhs);
    if (!rhs.has_value()) {
      return std::unexpected(rhs.error());
    }
    auto cloned = make<binary_expr>();
    cloned->span = bin.span;
    cloned->op = bin.op;
    cloned->lhs = std::move(*lhs);
    cloned->rhs = std::move(*rhs);
    return ptr<expr>(std::move(cloned));
  }
  case node_kind::unary_expr: {
    const auto &un = dynamic_cast<const unary_expr &>(e);
    auto operand = clone_optional(un.operand);
    if (!operand.has_value()) {
      return std::unexpected(operand.error());
    }
    auto cloned = make<unary_expr>();
    cloned->span = un.span;
    cloned->op = un.op;
    cloned->operand = std::move(*operand);
    return ptr<expr>(std::move(cloned));
  }
  case node_kind::field_expr: {
    const auto &field = dynamic_cast<const field_expr &>(e);
    auto object = clone_optional(field.object);
    if (!object.has_value()) {
      return std::unexpected(object.error());
    }
    if (!field.generic_args.empty()) {
      return unsupported(e, "a method call with generic arguments");
    }
    auto cloned = make<field_expr>();
    cloned->span = field.span;
    cloned->object = std::move(*object);
    cloned->field_name = field.field_name;
    return ptr<expr>(std::move(cloned));
  }
  case node_kind::index_expr: {
    const auto &index = dynamic_cast<const index_expr &>(e);
    auto object = clone_optional(index.object);
    if (!object.has_value()) {
      return std::unexpected(object.error());
    }
    auto idx = clone_optional(index.index);
    if (!idx.has_value()) {
      return std::unexpected(idx.error());
    }
    auto cloned = make<index_expr>();
    cloned->span = index.span;
    cloned->object = std::move(*object);
    cloned->index = std::move(*idx);
    return ptr<expr>(std::move(cloned));
  }
  case node_kind::call_expr: {
    const auto &call = dynamic_cast<const call_expr &>(e);
    auto callee = clone_optional(call.callee);
    if (!callee.has_value()) {
      return std::unexpected(callee.error());
    }
    auto args = clone_call_args(call.args);
    if (!args.has_value()) {
      return std::unexpected(args.error());
    }
    auto cloned = make<call_expr>();
    cloned->span = call.span;
    cloned->callee = std::move(*callee);
    cloned->args = std::move(*args);
    return ptr<expr>(std::move(cloned));
  }
  case node_kind::try_expr: {
    const auto &try_e = dynamic_cast<const try_expr &>(e);
    auto operand = clone_optional(try_e.operand);
    if (!operand.has_value()) {
      return std::unexpected(operand.error());
    }
    auto cloned = make<try_expr>();
    cloned->span = try_e.span;
    cloned->operand = std::move(*operand);
    return ptr<expr>(std::move(cloned));
  }
  case node_kind::array_expr: {
    const auto &array = dynamic_cast<const array_expr &>(e);
    auto cloned = make<array_expr>();
    cloned->span = array.span;
    for (const auto &element : array.elements) {
      auto cloned_element = clone_expr_impl(*element);
      if (!cloned_element.has_value()) {
        return std::unexpected(cloned_element.error());
      }
      cloned->elements.push_back(std::move(*cloned_element));
    }
    auto fill_value = clone_optional(array.fill_value);
    if (!fill_value.has_value()) {
      return std::unexpected(fill_value.error());
    }
    auto fill_count = clone_optional(array.fill_count);
    if (!fill_count.has_value()) {
      return std::unexpected(fill_count.error());
    }
    cloned->fill_value = std::move(*fill_value);
    cloned->fill_count = std::move(*fill_count);
    return ptr<expr>(std::move(cloned));
  }
  case node_kind::tuple_expr: {
    const auto &tuple = dynamic_cast<const tuple_expr &>(e);
    auto cloned = make<tuple_expr>();
    cloned->span = tuple.span;
    for (const auto &element : tuple.elements) {
      auto cloned_element = clone_optional(element);
      if (!cloned_element.has_value()) {
        return std::unexpected(cloned_element.error());
      }
      cloned->elements.push_back(std::move(*cloned_element));
    }
    return ptr<expr>(std::move(cloned));
  }
  case node_kind::group_expr: {
    const auto &group = dynamic_cast<const group_expr &>(e);
    auto inner = clone_optional(group.inner);
    if (!inner.has_value()) {
      return std::unexpected(inner.error());
    }
    auto cloned = make<group_expr>();
    cloned->span = group.span;
    cloned->inner = std::move(*inner);
    return ptr<expr>(std::move(cloned));
  }
  case node_kind::cast_expr: {
    const auto &cast = dynamic_cast<const cast_expr &>(e);
    auto operand = clone_optional(cast.operand);
    if (!operand.has_value()) {
      return std::unexpected(operand.error());
    }
    auto target = clone_optional(cast.target_type);
    if (!target.has_value()) {
      return std::unexpected(target.error());
    }
    auto cloned = make<cast_expr>();
    cloned->span = cast.span;
    cloned->operand = std::move(*operand);
    cloned->target_type = std::move(*target);
    return ptr<expr>(std::move(cloned));
  }
  case node_kind::struct_expr: {
    const auto &object = dynamic_cast<const struct_expr &>(e);
    auto type_name = clone_optional(object.type_name);
    if (!type_name.has_value()) {
      return std::unexpected(type_name.error());
    }
    auto cloned = make<struct_expr>();
    cloned->span = object.span;
    cloned->type_name = std::move(*type_name);
    for (const auto &field : object.fields) {
      auto value = clone_optional(field.value);
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
      cloned->fields.push_back(struct_field_init{
          .span = field.span, .name = field.name, .value = std::move(*value)});
    }
    return ptr<expr>(std::move(cloned));
  }
  case node_kind::block_expr: {
    const auto &block = dynamic_cast<const block_expr &>(e);
    auto stmts = clone_node_list(block.stmts);
    if (!stmts.has_value()) {
      return std::unexpected(stmts.error());
    }
    auto cloned = make<block_expr>();
    cloned->span = block.span;
    cloned->stmts = std::move(*stmts);
    return ptr<expr>(std::move(cloned));
  }
  case node_kind::if_expr: {
    const auto &conditional = dynamic_cast<const if_expr &>(e);
    auto branches = clone_if_branches(conditional.branches);
    if (!branches.has_value()) {
      return std::unexpected(branches.error());
    }
    auto else_body = clone_node_list(conditional.else_body);
    if (!else_body.has_value()) {
      return std::unexpected(else_body.error());
    }
    auto cloned = make<if_expr>();
    cloned->span = conditional.span;
    cloned->branches = std::move(*branches);
    cloned->else_body = std::move(*else_body);
    return ptr<expr>(std::move(cloned));
  }
  case node_kind::match_expr: {
    const auto &match = dynamic_cast<const match_expr &>(e);
    auto subject = clone_optional(match.subject);
    if (!subject.has_value()) {
      return std::unexpected(subject.error());
    }
    auto arms = clone_match_arms(match.arms);
    if (!arms.has_value()) {
      return std::unexpected(arms.error());
    }
    auto cloned = make<match_expr>();
    cloned->span = match.span;
    cloned->subject = std::move(*subject);
    cloned->arms = std::move(*arms);
    return ptr<expr>(std::move(cloned));
  }
  case node_kind::interpolated_string_expr: {
    const auto &interp = dynamic_cast<const interpolated_string_expr &>(e);
    auto cloned = make<interpolated_string_expr>();
    cloned->span = interp.span;
    for (const auto &segment : interp.segments) {
      auto value = clone_optional(segment.value);
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
      auto spec = clone_format_spec(segment.spec);
      if (!spec.has_value()) {
        return std::unexpected(spec.error());
      }
      auto cloned_segment = interp_segment{};
      cloned_segment.is_literal = segment.is_literal;
      cloned_segment.literal_text = segment.literal_text;
      cloned_segment.value = std::move(*value);
      cloned_segment.self_doc = segment.self_doc;
      cloned_segment.source_text = segment.source_text;
      cloned_segment.has_spec = segment.has_spec;
      cloned_segment.spec = std::move(*spec);
      cloned->segments.push_back(std::move(cloned_segment));
    }
    return ptr<expr>(std::move(cloned));
  }
  case node_kind::yield_expr: {
    const auto &yield = dynamic_cast<const yield_expr &>(e);
    auto value = clone_optional(yield.value);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    auto cloned = make<yield_expr>();
    cloned->span = yield.span;
    cloned->value = std::move(*value);
    return ptr<expr>(std::move(cloned));
  }
  case node_kind::module_path_expr: {
    // A dotted path (`self.value`, `DB.query`) the parser leaves unresolved
    // between field access and a qualified module reference — carried as bare
    // segments, so a shallow copy suffices.
    const auto &path = dynamic_cast<const module_path_expr &>(e);
    auto cloned = make<module_path_expr>();
    cloned->span = path.span;
    cloned->segments = path.segments;
    return ptr<expr>(std::move(cloned));
  }
  default:
    return unsupported(e, "this expression shape");
  }
}

[[nodiscard]] auto clone_node_list(const std::vector<ptr<node>> &nodes)
    -> std::expected<std::vector<ptr<node>>, clone_error> {
  auto cloned = std::vector<ptr<node>>{};
  cloned.reserve(nodes.size());
  for (const auto &n : nodes) {
    if (n == nullptr) {
      return std::unexpected(
          clone_error{.span = source_span::dummy(),
                      .message = "cannot clone a missing statement"});
    }
    auto cloned_node = clone_node(*n);
    if (!cloned_node.has_value()) {
      return std::unexpected(cloned_node.error());
    }
    cloned.push_back(std::move(*cloned_node));
  }
  return cloned;
}

[[nodiscard]] auto clone_node(const node &n)
    -> std::expected<ptr<node>, clone_error> {
  switch (n.kind) {
  case node_kind::let_stmt: {
    const auto &let = dynamic_cast<const let_stmt &>(n);
    if (!let.else_body.empty()) {
      return unsupported(n, "a `let ... else` statement");
    }
    auto pattern = clone_optional(let.pattern);
    if (!pattern.has_value()) {
      return std::unexpected(pattern.error());
    }
    auto type_annotation = clone_optional(let.type_annotation);
    if (!type_annotation.has_value()) {
      return std::unexpected(type_annotation.error());
    }
    auto initializer = clone_optional(let.initializer);
    if (!initializer.has_value()) {
      return std::unexpected(initializer.error());
    }
    auto cloned = make<let_stmt>();
    cloned->span = let.span;
    cloned->pattern = std::move(*pattern);
    cloned->type_annotation = std::move(*type_annotation);
    cloned->initializer = std::move(*initializer);
    return ptr<node>(std::move(cloned));
  }
  case node_kind::var_stmt: {
    const auto &var = dynamic_cast<const var_stmt &>(n);
    auto type_annotation = clone_optional(var.type_annotation);
    if (!type_annotation.has_value()) {
      return std::unexpected(type_annotation.error());
    }
    auto initializer = clone_optional(var.initializer);
    if (!initializer.has_value()) {
      return std::unexpected(initializer.error());
    }
    auto cloned = make<var_stmt>();
    cloned->span = var.span;
    cloned->name = var.name;
    cloned->type_annotation = std::move(*type_annotation);
    cloned->initializer = std::move(*initializer);
    return ptr<node>(std::move(cloned));
  }
  case node_kind::assign_stmt: {
    const auto &assign = dynamic_cast<const assign_stmt &>(n);
    auto target = clone_optional(assign.target);
    if (!target.has_value()) {
      return std::unexpected(target.error());
    }
    auto value = clone_optional(assign.value);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    auto cloned = make<assign_stmt>();
    cloned->span = assign.span;
    cloned->target = std::move(*target);
    cloned->op = assign.op;
    cloned->value = std::move(*value);
    return ptr<node>(std::move(cloned));
  }
  case node_kind::expr_stmt: {
    const auto &stmt = dynamic_cast<const expr_stmt &>(n);
    auto value = clone_optional(stmt.expr);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    auto cloned = make<expr_stmt>();
    cloned->span = stmt.span;
    cloned->expr = std::move(*value);
    return ptr<node>(std::move(cloned));
  }
  case node_kind::return_stmt: {
    const auto &ret = dynamic_cast<const return_stmt &>(n);
    auto value = clone_optional(ret.value);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    auto cloned = make<return_stmt>();
    cloned->span = ret.span;
    cloned->value = std::move(*value);
    return ptr<node>(std::move(cloned));
  }
  case node_kind::if_stmt: {
    const auto &if_s = dynamic_cast<const if_stmt &>(n);
    auto branches = clone_if_branches(if_s.branches);
    if (!branches.has_value()) {
      return std::unexpected(branches.error());
    }
    auto else_body = clone_node_list(if_s.else_body);
    if (!else_body.has_value()) {
      return std::unexpected(else_body.error());
    }
    auto cloned = make<if_stmt>();
    cloned->span = if_s.span;
    cloned->branches = std::move(*branches);
    cloned->else_body = std::move(*else_body);
    return ptr<node>(std::move(cloned));
  }
  case node_kind::while_stmt: {
    const auto &while_s = dynamic_cast<const while_stmt &>(n);
    auto condition = clone_optional(while_s.condition);
    if (!condition.has_value()) {
      return std::unexpected(condition.error());
    }
    auto let_pattern = clone_optional(while_s.let_pattern);
    if (!let_pattern.has_value()) {
      return std::unexpected(let_pattern.error());
    }
    auto let_expr = clone_optional(while_s.let_expr);
    if (!let_expr.has_value()) {
      return std::unexpected(let_expr.error());
    }
    auto body = clone_node_list(while_s.body);
    if (!body.has_value()) {
      return std::unexpected(body.error());
    }
    auto cloned = make<while_stmt>();
    cloned->span = while_s.span;
    cloned->condition = std::move(*condition);
    cloned->let_pattern = std::move(*let_pattern);
    cloned->let_expr = std::move(*let_expr);
    cloned->body = std::move(*body);
    return ptr<node>(std::move(cloned));
  }
  case node_kind::for_stmt: {
    const auto &loop = dynamic_cast<const for_stmt &>(n);
    auto patterns = clone_pattern_list(loop.patterns);
    if (!patterns.has_value()) {
      return std::unexpected(patterns.error());
    }
    auto iterable = clone_optional(loop.iterable);
    if (!iterable.has_value()) {
      return std::unexpected(iterable.error());
    }
    auto guard = clone_optional(loop.guard);
    if (!guard.has_value()) {
      return std::unexpected(guard.error());
    }
    auto body = clone_node_list(loop.body);
    if (!body.has_value()) {
      return std::unexpected(body.error());
    }
    auto cloned = make<for_stmt>();
    cloned->span = loop.span;
    cloned->patterns = std::move(*patterns);
    cloned->iterable = std::move(*iterable);
    cloned->guard = std::move(*guard);
    cloned->body = std::move(*body);
    return ptr<node>(std::move(cloned));
  }
  case node_kind::match_stmt: {
    const auto &match = dynamic_cast<const match_stmt &>(n);
    auto subject = clone_optional(match.subject);
    if (!subject.has_value()) {
      return std::unexpected(subject.error());
    }
    auto arms = clone_match_arms(match.arms);
    if (!arms.has_value()) {
      return std::unexpected(arms.error());
    }
    auto cloned = make<match_stmt>();
    cloned->span = match.span;
    cloned->subject = std::move(*subject);
    cloned->arms = std::move(*arms);
    return ptr<node>(std::move(cloned));
  }
  default:
    // Not a statement — a pattern or an expression reached through a generic
    // `ptr<node>` slot rather than a block-body list (`match_arm::pattern`
    // and `if_branch::let_pattern` are patterns; `array_type::size`, a
    // fixed-array length, is an expression).
    if (const auto *as_pattern = dynamic_cast<const pattern *>(&n)) {
      auto cloned = clone_pattern(*as_pattern);
      if (!cloned.has_value()) {
        return std::unexpected(cloned.error());
      }
      return ptr<node>(std::move(*cloned));
    }
    if (const auto *as_expr = dynamic_cast<const expr *>(&n)) {
      auto cloned = clone_expr_impl(*as_expr);
      if (!cloned.has_value()) {
        return std::unexpected(cloned.error());
      }
      return ptr<node>(std::move(*cloned));
    }
    return unsupported(n, "this statement shape");
  }
}

} // namespace

/// The public face of the anonymous-namespace cloner above; see the header.
auto clone_expr(const expr &e) -> std::expected<ptr<expr>, clone_error> {
  return clone_expr_impl(e);
}

auto clone_func_decl(const func_decl &decl)
    -> std::expected<ptr<func_decl>, clone_error> {
  if (decl.modifiers.async_context != nullptr || decl.modifiers.is_async) {
    return unsupported(decl, "an async function");
  }

  auto cloned = make<func_decl>();
  cloned->span = decl.span;
  cloned->documentation = decl.documentation;
  cloned->visibility = decl.visibility;
  cloned->modifiers = func_modifiers{.is_pure = decl.modifiers.is_pure,
                                     .is_async = false,
                                     .is_machine = decl.modifiers.is_machine,
                                     .is_static = decl.modifiers.is_static,
                                     .is_intrinsic = false,
                                     .async_context = nullptr};
  cloned->modifiers.is_generator = decl.modifiers.is_generator;
  cloned->name = decl.name;

  // A const-generic instance keeps its template's parameter list — the
  // checker substitutes each one's constant while checking the clone, rather
  // than editing the clone's syntax (`semantic::checker::push_type_params`).
  for (const auto &param : decl.type_params) {
    auto bound_or_type = clone_optional(param.bound_or_type);
    if (!bound_or_type.has_value()) {
      return std::unexpected(bound_or_type.error());
    }
    cloned->type_params.push_back(
        type_param{.span = param.span,
                   .name = param.name,
                   .bound_or_type = std::move(*bound_or_type),
                   .is_value_param = param.is_value_param,
                   .higher_kinded_arity = param.higher_kinded_arity});
  }

  // Carried across like the type parameters, and for the same reason: the
  // clone keeps the template's constraints and the checker substitutes while
  // checking it. A `where` clause used to be refused outright here, which was
  // right while bounds were inert — a constraint that changes nothing is one
  // more thing to get wrong in a clone. `solve_from_bounds` gives them a job
  // (`where I: iterator[T]` is what solves `T` for an adapter), so an
  // instance that dropped them would be checked under weaker information than
  // the call that asked for it.
  for (const auto &constraint : decl.where_constraints) {
    auto subject = clone_optional(constraint.subject);
    if (!subject.has_value()) {
      return std::unexpected(subject.error());
    }
    auto constraint_bound = clone_optional(constraint.bound_or_type);
    if (!constraint_bound.has_value()) {
      return std::unexpected(constraint_bound.error());
    }
    cloned->where_constraints.push_back(
        where_constraint{.span = constraint.span,
                         .subject = std::move(*subject),
                         .bound_or_type = std::move(*constraint_bound)});
  }

  for (const auto &contract : decl.contracts) {
    auto condition = clone_optional(contract.condition);
    if (!condition.has_value()) {
      return std::unexpected(condition.error());
    }
    cloned->contracts.push_back(
        contract_clause{.span = contract.span,
                        .is_pre = contract.is_pre,
                        .condition = std::move(*condition),
                        .message = contract.message});
  }

  for (const auto &param : decl.params) {
    if (param.default_value != nullptr) {
      return unsupported(decl, "a parameter with a default value");
    }
    auto pattern = clone_optional(param.pattern);
    if (!pattern.has_value()) {
      return std::unexpected(pattern.error());
    }
    auto type_annotation = clone_optional(param.type_annotation);
    if (!type_annotation.has_value()) {
      return std::unexpected(type_annotation.error());
    }
    cloned->params.push_back(
        ast::param{.span = param.span,
                   .pattern = std::move(*pattern),
                   .type_annotation = std::move(*type_annotation),
                   .default_value = nullptr});
  }

  auto return_type = clone_optional(decl.return_type);
  if (!return_type.has_value()) {
    return std::unexpected(return_type.error());
  }
  cloned->return_type = std::move(*return_type);

  if (decl.body_expr != nullptr) {
    auto body_expr = clone_expr_impl(*decl.body_expr);
    if (!body_expr.has_value()) {
      return std::unexpected(body_expr.error());
    }
    cloned->body_expr = std::move(*body_expr);
  } else {
    auto body_stmts = clone_node_list(decl.body_stmts);
    if (!body_stmts.has_value()) {
      return std::unexpected(body_stmts.error());
    }
    cloned->body_stmts = std::move(*body_stmts);
  }

  return cloned;
}

namespace {

/// Clones a `type` declaration's definition body. The definition is one of a
/// struct body, a sum body, a bare type expression (an alias like
/// `type row = DB.conn`), or a refinement type; each is dispatched here so a
/// projection through a module parameter is cloned into the instantiation.
[[nodiscard]] auto clone_type_definition(const node &def)
    -> std::expected<ptr<node>, clone_error> {
  switch (def.kind) {
  case node_kind::struct_type_def: {
    const auto &struct_def = dynamic_cast<const struct_type_def &>(def);
    auto cloned = make<struct_type_def>();
    cloned->span = struct_def.span;
    cloned->body.span = struct_def.body.span;
    for (const auto &field : struct_def.body.fields) {
      auto type = clone_optional(field.type);
      if (!type.has_value()) {
        return std::unexpected(type.error());
      }
      cloned->body.fields.push_back(
          struct_field{.span = field.span,
                       .visibility = field.visibility,
                       .name = field.name,
                       .type = std::move(*type),
                       .documentation = field.documentation});
    }
    return ptr<node>(std::move(cloned));
  }
  case node_kind::sum_type_def: {
    const auto &sum_def = dynamic_cast<const sum_type_def &>(def);
    auto cloned = make<sum_type_def>();
    cloned->span = sum_def.span;
    cloned->body.span = sum_def.body.span;
    for (const auto &variant : sum_def.body.variants) {
      auto cloned_variant = sum_variant{.span = variant.span,
                                        .name = variant.name,
                                        .payload_types = {},
                                        .documentation = variant.documentation};
      for (const auto &payload : variant.payload_types) {
        auto type = clone_optional(payload);
        if (!type.has_value()) {
          return std::unexpected(type.error());
        }
        cloned_variant.payload_types.push_back(std::move(*type));
      }
      cloned->body.variants.push_back(std::move(cloned_variant));
    }
    return ptr<node>(std::move(cloned));
  }
  case node_kind::refinement_type: {
    const auto &refine = dynamic_cast<const refinement_type &>(def);
    auto base = clone_optional(refine.base);
    if (!base.has_value()) {
      return std::unexpected(base.error());
    }
    auto predicate = clone_optional(refine.predicate);
    if (!predicate.has_value()) {
      return std::unexpected(predicate.error());
    }
    auto cloned = make<refinement_type>();
    cloned->span = refine.span;
    cloned->base = std::move(*base);
    cloned->predicate = std::move(*predicate);
    return ptr<node>(std::move(cloned));
  }
  default:
    // Anything else is a bare type expression used as an alias body.
    if (const auto *as_type = dynamic_cast<const type_expr *>(&def)) {
      auto cloned = clone_type_expr(*as_type);
      if (!cloned.has_value()) {
        return std::unexpected(cloned.error());
      }
      return ptr<node>(std::move(*cloned));
    }
    return unsupported(def, "this type definition shape");
  }
}

} // namespace

auto clone_type_decl(const type_decl &decl)
    -> std::expected<ptr<type_decl>, clone_error> {
  auto cloned = make<type_decl>();
  cloned->span = decl.span;
  cloned->documentation = decl.documentation;
  cloned->visibility = decl.visibility;
  cloned->modifiers = decl.modifiers;
  cloned->name = decl.name;
  cloned->deriving = decl.deriving;

  for (const auto &param : decl.type_params) {
    auto bound_or_type = clone_optional(param.bound_or_type);
    if (!bound_or_type.has_value()) {
      return std::unexpected(bound_or_type.error());
    }
    cloned->type_params.push_back(
        type_param{.span = param.span,
                   .name = param.name,
                   .bound_or_type = std::move(*bound_or_type),
                   .is_value_param = param.is_value_param,
                   .higher_kinded_arity = param.higher_kinded_arity});
  }

  if (decl.definition != nullptr) {
    auto definition = clone_type_definition(*decl.definition);
    if (!definition.has_value()) {
      return std::unexpected(definition.error());
    }
    cloned->definition = std::move(*definition);
  }

  auto invariant = clone_optional(decl.invariant);
  if (!invariant.has_value()) {
    return std::unexpected(invariant.error());
  }
  cloned->invariant = std::move(*invariant);

  return cloned;
}

auto clone_static_decl(const static_decl &decl)
    -> std::expected<ptr<static_decl>, clone_error> {
  if (decl.decl_kind != static_decl_kind::binding) {
    return unsupported(decl, "a non-binding `static` form");
  }

  auto type_annotation = clone_optional(decl.type_annotation);
  if (!type_annotation.has_value()) {
    return std::unexpected(type_annotation.error());
  }
  auto initializer = clone_optional(decl.initializer);
  if (!initializer.has_value()) {
    return std::unexpected(initializer.error());
  }

  auto cloned = make<static_decl>();
  cloned->span = decl.span;
  cloned->documentation = decl.documentation;
  cloned->visibility = decl.visibility;
  cloned->decl_kind = static_decl_kind::binding;
  cloned->name = decl.name;
  cloned->type_annotation = std::move(*type_annotation);
  cloned->initializer = std::move(*initializer);
  return cloned;
}

namespace {

/// Deep-clones a generic-parameter list, shared by the impl/trait cloners
/// (`clone_func_decl`/`clone_type_decl` inline the same loop).
[[nodiscard]] auto clone_type_params(const std::vector<type_param> &params)
    -> std::expected<std::vector<type_param>, clone_error> {
  auto cloned = std::vector<type_param>{};
  cloned.reserve(params.size());
  for (const auto &param : params) {
    auto bound_or_type = clone_optional(param.bound_or_type);
    if (!bound_or_type.has_value()) {
      return std::unexpected(bound_or_type.error());
    }
    cloned.push_back(
        type_param{.span = param.span,
                   .name = param.name,
                   .bound_or_type = std::move(*bound_or_type),
                   .is_value_param = param.is_value_param,
                   .higher_kinded_arity = param.higher_kinded_arity});
  }
  return cloned;
}

/// Deep-clones a `where` constraint list (impl blocks carry them).
[[nodiscard]] auto
clone_where_constraints(const std::vector<where_constraint> &constraints)
    -> std::expected<std::vector<where_constraint>, clone_error> {
  auto cloned = std::vector<where_constraint>{};
  cloned.reserve(constraints.size());
  for (const auto &constraint : constraints) {
    auto subject = clone_optional(constraint.subject);
    if (!subject.has_value()) {
      return std::unexpected(subject.error());
    }
    auto bound_or_type = clone_optional(constraint.bound_or_type);
    if (!bound_or_type.has_value()) {
      return std::unexpected(bound_or_type.error());
    }
    cloned.push_back(
        where_constraint{.span = constraint.span,
                         .subject = std::move(*subject),
                         .bound_or_type = std::move(*bound_or_type)});
  }
  return cloned;
}

/// Deep-clones a `+`-joined trait bound (a trait's `requires` clause).
[[nodiscard]] auto clone_bound(const bound &b)
    -> std::expected<bound, clone_error> {
  auto cloned = bound{.span = b.span, .terms = {}};
  cloned.terms.reserve(b.terms.size());
  for (const auto &term : b.terms) {
    auto type = clone_optional(term.type);
    if (!type.has_value()) {
      return std::unexpected(type.error());
    }
    cloned.terms.push_back(
        bound_term{.span = term.span, .type = std::move(*type)});
  }
  return cloned;
}

/// Deep-clones one member item of an `impl`/`extend`/`trait` block — a method
/// (`func_decl`), a required constant (`static_decl`), an associated-type
/// declaration (trait side) or definition (impl side). Fails on anything else.
[[nodiscard]] auto clone_member_item(const node &item)
    -> std::expected<ptr<node>, clone_error> {
  switch (item.kind) {
  case node_kind::func_decl: {
    auto cloned = clone_func_decl(dynamic_cast<const func_decl &>(item));
    if (!cloned.has_value()) {
      return std::unexpected(cloned.error());
    }
    return ptr<node>(std::move(*cloned));
  }
  case node_kind::static_decl: {
    auto cloned = clone_static_decl(dynamic_cast<const static_decl &>(item));
    if (!cloned.has_value()) {
      return std::unexpected(cloned.error());
    }
    return ptr<node>(std::move(*cloned));
  }
  case node_kind::associated_type_decl_node: {
    const auto &assoc = dynamic_cast<const associated_type_decl_node &>(item);
    auto default_type = clone_optional(assoc.value.default_type);
    if (!default_type.has_value()) {
      return std::unexpected(default_type.error());
    }
    auto cloned = make<associated_type_decl_node>();
    cloned->span = assoc.span;
    cloned->value =
        associated_type_decl{.span = assoc.value.span,
                             .visibility = assoc.value.visibility,
                             .name = assoc.value.name,
                             .default_type = std::move(*default_type)};
    return ptr<node>(std::move(cloned));
  }
  case node_kind::associated_type_def_node: {
    const auto &assoc = dynamic_cast<const associated_type_def_node &>(item);
    auto type = clone_optional(assoc.value.type);
    if (!type.has_value()) {
      return std::unexpected(type.error());
    }
    auto cloned = make<associated_type_def_node>();
    cloned->span = assoc.span;
    cloned->value = associated_type_def{.span = assoc.value.span,
                                        .name = assoc.value.name,
                                        .type = std::move(*type)};
    return ptr<node>(std::move(cloned));
  }
  default:
    return unsupported(item, "this impl/trait member shape");
  }
}

/// Deep-clones a member-item list, skipping null entries.
[[nodiscard]] auto clone_member_items(const std::vector<ptr<node>> &items)
    -> std::expected<std::vector<ptr<node>>, clone_error> {
  auto cloned = std::vector<ptr<node>>{};
  cloned.reserve(items.size());
  for (const auto &item : items) {
    if (item == nullptr) {
      continue;
    }
    auto cloned_item = clone_member_item(*item);
    if (!cloned_item.has_value()) {
      return std::unexpected(cloned_item.error());
    }
    cloned.push_back(std::move(*cloned_item));
  }
  return cloned;
}

} // namespace

auto clone_impl_decl(const impl_decl &decl)
    -> std::expected<ptr<impl_decl>, clone_error> {
  auto type_params = clone_type_params(decl.type_params);
  if (!type_params.has_value()) {
    return std::unexpected(type_params.error());
  }
  auto trait_type = clone_optional(decl.trait_type);
  if (!trait_type.has_value()) {
    return std::unexpected(trait_type.error());
  }
  auto for_type = clone_optional(decl.for_type);
  if (!for_type.has_value()) {
    return std::unexpected(for_type.error());
  }
  auto where_constraints = clone_where_constraints(decl.where_constraints);
  if (!where_constraints.has_value()) {
    return std::unexpected(where_constraints.error());
  }
  auto items = clone_member_items(decl.items);
  if (!items.has_value()) {
    return std::unexpected(items.error());
  }

  auto cloned = make<impl_decl>();
  cloned->span = decl.span;
  cloned->documentation = decl.documentation;
  cloned->type_params = std::move(*type_params);
  cloned->trait_type = std::move(*trait_type);
  cloned->for_type = std::move(*for_type);
  cloned->where_constraints = std::move(*where_constraints);
  cloned->items = std::move(*items);
  return cloned;
}

auto clone_extend_decl(const extend_decl &decl)
    -> std::expected<ptr<extend_decl>, clone_error> {
  auto for_type = clone_optional(decl.for_type);
  if (!for_type.has_value()) {
    return std::unexpected(for_type.error());
  }
  auto items = clone_member_items(decl.items);
  if (!items.has_value()) {
    return std::unexpected(items.error());
  }

  auto cloned = make<extend_decl>();
  cloned->span = decl.span;
  cloned->documentation = decl.documentation;
  cloned->for_type = std::move(*for_type);
  cloned->items = std::move(*items);
  return cloned;
}

auto clone_trait_decl(const trait_decl &decl)
    -> std::expected<ptr<trait_decl>, clone_error> {
  auto type_params = clone_type_params(decl.type_params);
  if (!type_params.has_value()) {
    return std::unexpected(type_params.error());
  }
  auto items = clone_member_items(decl.items);
  if (!items.has_value()) {
    return std::unexpected(items.error());
  }

  auto cloned = make<trait_decl>();
  cloned->span = decl.span;
  cloned->documentation = decl.documentation;
  cloned->visibility = decl.visibility;
  cloned->name = decl.name;
  cloned->type_params = std::move(*type_params);
  if (decl.requires_bound.has_value()) {
    auto requires_bound = clone_bound(*decl.requires_bound);
    if (!requires_bound.has_value()) {
      return std::unexpected(requires_bound.error());
    }
    cloned->requires_bound = std::move(*requires_bound);
  }
  cloned->items = std::move(*items);
  return cloned;
}

} // namespace kira::ast
