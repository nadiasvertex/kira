#include "ast_clone.h"

#include <format>

namespace kira::ast {

namespace {

[[nodiscard]] auto unsupported(const node &n, std::string_view what)
    -> std::unexpected<clone_error> {
  return std::unexpected(clone_error{
      .span = n.span,
      .message = std::format("cannot clone this construct for trait-default "
                             "monomorphization: {}",
                             what)});
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
/// parse time (`ast.h`: "disambiguate later in semantic analysis"), but
/// every argument `std.io`'s trait-default bodies actually use is a type
/// (`result[usize, io_error]`, `list[byte]`, ...), never a value argument —
/// so this only needs to recognize "is this a `type_expr`" and delegate.
[[nodiscard]] auto clone_type_arg_value(const node &n)
    -> std::expected<ptr<node>, clone_error> {
  if (const auto *as_type = dynamic_cast<const type_expr *>(&n)) {
    auto cloned = clone_type_expr(*as_type);
    if (!cloned.has_value()) {
      return std::unexpected(cloned.error());
    }
    return ptr<node>(std::move(*cloned));
  }
  return unsupported(n, "a non-type generic argument");
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
  default:
    return unsupported(t, "this type-expression shape");
  }
}

[[nodiscard]] auto clone_pattern(const pattern &p)
    -> std::expected<ptr<pattern>, clone_error> {
  if (p.kind != node_kind::binding_pattern) {
    return unsupported(p, "this pattern shape");
  }
  const auto &binding = dynamic_cast<const binding_pattern &>(p);
  auto cloned = make<binding_pattern>();
  cloned->span = binding.span;
  cloned->name = binding.name;
  cloned->is_mut = binding.is_mut;
  return ptr<pattern>(std::move(cloned));
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

[[nodiscard]] auto clone_expr_impl(const expr &e)
    -> std::expected<ptr<expr>, clone_error> {
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
    auto cloned = make<if_stmt>();
    cloned->span = if_s.span;
    for (const auto &branch : if_s.branches) {
      if (branch.let_pattern != nullptr || branch.let_expr != nullptr) {
        return unsupported(n, "an `if let` branch");
      }
      auto condition = clone_optional(branch.condition);
      if (!condition.has_value()) {
        return std::unexpected(condition.error());
      }
      auto body = clone_node_list(branch.body);
      if (!body.has_value()) {
        return std::unexpected(body.error());
      }
      cloned->branches.push_back(if_branch{.span = branch.span,
                                           .condition = std::move(*condition),
                                           .let_pattern = nullptr,
                                           .let_expr = nullptr,
                                           .body = std::move(*body)});
    }
    auto else_body = clone_node_list(if_s.else_body);
    if (!else_body.has_value()) {
      return std::unexpected(else_body.error());
    }
    cloned->else_body = std::move(*else_body);
    return ptr<node>(std::move(cloned));
  }
  case node_kind::while_stmt: {
    const auto &while_s = dynamic_cast<const while_stmt &>(n);
    if (while_s.let_pattern != nullptr || while_s.let_expr != nullptr) {
      return unsupported(n, "a `while let` loop");
    }
    auto condition = clone_optional(while_s.condition);
    if (!condition.has_value()) {
      return std::unexpected(condition.error());
    }
    auto body = clone_node_list(while_s.body);
    if (!body.has_value()) {
      return std::unexpected(body.error());
    }
    auto cloned = make<while_stmt>();
    cloned->span = while_s.span;
    cloned->condition = std::move(*condition);
    cloned->body = std::move(*body);
    return ptr<node>(std::move(cloned));
  }
  default:
    // Not a statement — most likely a plain expression reached through a
    // generic `ptr<node>` slot rather than a block-body list (e.g.
    // `array_type::size`, a fixed-array length expression, which is
    // typically just a `literal_expr`).
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
  if (!decl.type_params.empty()) {
    return unsupported(decl, "a generic function");
  }
  if (!decl.where_constraints.empty()) {
    return unsupported(decl, "a `where` clause");
  }
  if (!decl.contracts.empty()) {
    return unsupported(decl, "a contract clause");
  }
  if (decl.modifiers.async_context != nullptr || decl.modifiers.is_async) {
    return unsupported(decl, "an async function");
  }

  auto cloned = make<func_decl>();
  cloned->span = decl.span;
  cloned->visibility = decl.visibility;
  cloned->modifiers = func_modifiers{.is_pure = decl.modifiers.is_pure,
                                     .is_async = false,
                                     .is_machine = decl.modifiers.is_machine,
                                     .is_static = decl.modifiers.is_static,
                                     .is_intrinsic = false,
                                     .async_context = nullptr};
  cloned->name = decl.name;

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

} // namespace kira::ast
