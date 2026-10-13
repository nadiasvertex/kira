#pragma once

#include <type_traits>
#include <variant>

#include "src/parser/ast.h"

namespace cinder::ast {

// Generic AST traversal, modeled on `hir/traversal.h`. `for_each_child` is the
// only place that knows which fields of a node hold child nodes; a pass that
// needs to look at every node uses `walk`, and a pass that cares about a few
// kinds handles those and calls `for_each_child` for the rest. The switch
// below has no `default`, so adding a `node_kind` is a compile error here
// until the new kind's children are listed.

/// @brief Whether `kind` is a type-position node.
[[nodiscard]] constexpr auto is_type_kind(node_kind kind) noexcept -> bool {
  switch (kind) {
  case node_kind::named_type:
  case node_kind::bound_type:
  case node_kind::existential_type:
  case node_kind::tuple_type:
  case node_kind::slice_type:
  case node_kind::array_type:
  case node_kind::ref_type:
  case node_kind::ptr_type:
  case node_kind::mut_type:
  case node_kind::fn_type:
  case node_kind::quote_type:
  case node_kind::splice_type:
  case node_kind::union_type:
  case node_kind::refinement_type:
    return true;
  default:
    return false;
  }
}

/// @brief Whether `kind` is a pattern node.
[[nodiscard]] constexpr auto is_pattern_kind(node_kind kind) noexcept -> bool {
  switch (kind) {
  case node_kind::wildcard_pattern:
  case node_kind::literal_pattern:
  case node_kind::binding_pattern:
  case node_kind::constructor_pattern:
  case node_kind::tuple_pattern:
  case node_kind::struct_pattern:
  case node_kind::array_pattern:
  case node_kind::range_pattern:
  case node_kind::option_pattern:
  case node_kind::result_pattern:
  case node_kind::ref_pattern:
  case node_kind::or_pattern:
  case node_kind::group_pattern:
    return true;
  default:
    return false;
  }
}

/// @brief Whether `kind` is an expression node.
[[nodiscard]] constexpr auto is_expr_kind(node_kind kind) noexcept -> bool {
  switch (kind) {
  case node_kind::ident_expr:
  case node_kind::literal_expr:
  case node_kind::interpolated_string_expr:
  case node_kind::binary_expr:
  case node_kind::unary_expr:
  case node_kind::postfix_expr:
  case node_kind::call_expr:
  case node_kind::index_expr:
  case node_kind::field_expr:
  case node_kind::cast_expr:
  case node_kind::try_expr:
  case node_kind::tuple_expr:
  case node_kind::array_expr:
  case node_kind::struct_expr:
  case node_kind::lambda_expr:
  case node_kind::match_expr:
  case node_kind::if_expr:
  case node_kind::for_expr:
  case node_kind::await_expr:
  case node_kind::yield_expr:
  case node_kind::async_expr:
  case node_kind::par_expr:
  case node_kind::race_expr:
  case node_kind::crew_expr:
  case node_kind::on_expr:
  case node_kind::block_expr:
  case node_kind::quote_expr:
  case node_kind::splice_expr:
  case node_kind::static_expr:
  case node_kind::module_path_expr:
  case node_kind::group_expr:
  case node_kind::where_expr:
    return true;
  default:
    return false;
  }
}

/// @brief Whether `kind` is a statement node.
[[nodiscard]] constexpr auto is_stmt_kind(node_kind kind) noexcept -> bool {
  switch (kind) {
  case node_kind::let_stmt:
  case node_kind::var_stmt:
  case node_kind::assign_stmt:
  case node_kind::expr_stmt:
  case node_kind::return_stmt:
  case node_kind::break_stmt:
  case node_kind::continue_stmt:
  case node_kind::if_stmt:
  case node_kind::while_stmt:
  case node_kind::scope_stmt:
  case node_kind::for_stmt:
  case node_kind::match_stmt:
  case node_kind::crew_stmt:
  case node_kind::asm_stmt:
  case node_kind::splice_stmt:
    return true;
  default:
    return false;
  }
}

/// @brief `node` as a `T`, or null when it is not one (or `node` is null).
///
/// This is a checked cast, not a tag-only cast: the parser's recovery
/// placeholders (`error_expr`, `error_type_expr`, `error_pattern`,
/// `error_stmt`) borrow the tag of a real kind such as `ident_expr`, so the
/// tag alone does not prove the dynamic type.
template <typename T, typename N>
[[nodiscard]] auto as(N *node) noexcept
    -> std::conditional_t<std::is_const_v<N>, const T *, T *> {
  return dynamic_cast<std::conditional_t<std::is_const_v<N>, const T *, T *>>(
      node);
}

/// @brief `node` as a `T &`, for a caller that has already dispatched on the
/// kind tag and knows the dynamic type. Throws `std::bad_cast` on a mismatch,
/// which means the dispatch or a placeholder node was mishandled.
template <typename T, typename N>
[[nodiscard]] auto cast(N &node)
    -> std::conditional_t<std::is_const_v<N>, const T &, T &> {
  return dynamic_cast<std::conditional_t<std::is_const_v<N>, const T &, T &>>(
      node);
}

/// @brief Like `as`, but rejects on the tag first so a mismatch costs no RTTI.
template <typename T, typename N>
[[nodiscard]] auto as_kind(N *node, node_kind kind) noexcept
    -> std::conditional_t<std::is_const_v<N>, const T *, T *> {
  if (node == nullptr || node->kind != kind) {
    return nullptr;
  }
  return as<T>(node);
}

/// Calls `f` on every non-null child node of `n`, in source order. `f`
/// receives `node &` (`const node &` when `n` is const). Type annotations,
/// patterns, bounds and where-clauses are children too; a pass that ignores
/// them filters with `is_type_kind` / `is_pattern_kind`. Does not descend
/// into the child it passes, so a caller decides whether to recurse.
template <typename N, typename F>
auto for_each_child(N &n, F &&f) -> void
  requires std::is_base_of_v<node, std::remove_const_t<N>>
{
  constexpr bool k_const = std::is_const_v<N>;
  using node_t = std::conditional_t<k_const, const node, node>;
  const auto one = [&](const auto &slot) -> void {
    if (slot != nullptr) {
      f(static_cast<node_t &>(*slot));
    }
  };
  const auto all = [&](const auto &slots) -> void {
    for (const auto &slot : slots) {
      one(slot);
    }
  };
  const auto type_arg_children = [&](const auto &args) -> void {
    for (const auto &arg : args) {
      one(arg.value);
    }
  };
  const auto bound_children = [&](const auto &bound) -> void {
    for (const auto &term : bound.terms) {
      one(term.type);
    }
  };
  const auto type_param_children = [&](const auto &params) -> void {
    for (const auto &param : params) {
      one(param.bound_or_type);
    }
  };
  const auto where_children = [&](const auto &constraints) -> void {
    for (const auto &constraint : constraints) {
      one(constraint.subject);
      one(constraint.bound_or_type);
    }
  };
  const auto branch_children = [&](const auto &branches,
                                   const auto &else_body) -> void {
    for (const auto &branch : branches) {
      one(branch.condition);
      one(branch.let_pattern);
      one(branch.let_expr);
      all(branch.body);
    }
    all(else_body);
  };
  const auto arm_children = [&](const auto &arms) -> void {
    for (const auto &arm : arms) {
      one(arm.pattern);
      one(arm.guard);
      one(arm.body_expr);
      all(arm.body_stmts);
    }
  };
  const auto format_dimension = [&](const auto &dimension) -> void {
    if (const auto *expr_slot = std::get_if<ptr<expr>>(&dimension)) {
      one(*expr_slot);
    }
  };

  switch (n.kind) {
  // ---- Leaves ------------------------------------------------------------
  case node_kind::error_node:
  case node_kind::module_decl:
  case node_kind::dep_decl:
  case node_kind::quote_type:
  case node_kind::ident_expr:
  case node_kind::literal_expr:
  case node_kind::postfix_expr:
  case node_kind::module_path_expr:
  case node_kind::wildcard_pattern:
  case node_kind::literal_pattern:
  case node_kind::binding_pattern:
  case node_kind::break_stmt:
  case node_kind::continue_stmt:
  case node_kind::asm_stmt:
    return;

  // ---- Top level and items -----------------------------------------------
  case node_kind::file_node:
    if (const auto *t = as<file>(&n)) {
      one(t->module_decl);
      all(t->items);
    }
    return;
  case node_kind::use_decl:
    if (const auto *t = as<use_decl>(&n)) {
      type_arg_children(t->instantiation_args);
    }
    return;
  case node_kind::type_decl:
    if (const auto *t = as<type_decl>(&n)) {
      type_param_children(t->type_params);
      one(t->definition);
      one(t->invariant);
    }
    return;
  case node_kind::struct_type_def:
    if (const auto *t = as<struct_type_def>(&n)) {
      for (const auto &field : t->body.fields) {
        one(field.type);
      }
    }
    return;
  case node_kind::sum_type_def:
    if (const auto *t = as<sum_type_def>(&n)) {
      for (const auto &variant : t->body.variants) {
        all(variant.payload_types);
      }
    }
    return;
  case node_kind::trait_decl:
    if (const auto *t = as<trait_decl>(&n)) {
      type_param_children(t->type_params);
      if (t->requires_bound.has_value()) {
        bound_children(*t->requires_bound);
      }
      all(t->items);
    }
    return;
  case node_kind::signature_decl:
    if (const auto *t = as<signature_decl>(&n)) {
      all(t->items);
    }
    return;
  case node_kind::impl_decl:
    if (const auto *t = as<impl_decl>(&n)) {
      type_param_children(t->type_params);
      one(t->trait_type);
      one(t->for_type);
      where_children(t->where_constraints);
      all(t->items);
    }
    return;
  case node_kind::extend_decl:
    if (const auto *t = as<extend_decl>(&n)) {
      type_param_children(t->type_params);
      one(t->for_type);
      all(t->items);
    }
    return;
  case node_kind::concept_decl:
    if (const auto *t = as<concept_decl>(&n)) {
      for (const auto &constraint : t->constraints) {
        one(constraint.subject);
        one(constraint.bound_or_expr);
      }
    }
    return;
  case node_kind::func_decl:
    if (const auto *t = as<func_decl>(&n)) {
      one(t->modifiers.async_context);
      type_param_children(t->type_params);
      for (const auto &param : t->params) {
        one(param.pattern);
        one(param.type_annotation);
        one(param.default_value);
      }
      one(t->return_type);
      where_children(t->where_constraints);
      for (const auto &contract : t->contracts) {
        one(contract.condition);
      }
      one(t->body_expr);
      all(t->body_stmts);
    }
    return;
  case node_kind::sub_module_decl:
    if (const auto *t = as<sub_module_decl>(&n)) {
      type_param_children(t->type_params);
      all(t->items);
    }
    return;
  case node_kind::static_decl:
    if (const auto *t = as<static_decl>(&n)) {
      one(t->type_annotation);
      one(t->initializer);
      one(t->assert_condition);
      one(t->if_condition);
      all(t->if_body);
      all(t->else_body);
      all(t->for_patterns);
      one(t->for_iterable);
      one(t->for_guard);
      one(t->for_yield);
      all(t->for_body);
    }
    return;
  case node_kind::associated_type_decl_node:
    if (const auto *t = as<associated_type_decl_node>(&n)) {
      one(t->value.default_type);
    }
    return;
  case node_kind::associated_type_def_node:
    if (const auto *t = as<associated_type_def_node>(&n)) {
      one(t->value.type);
    }
    return;
  case node_kind::splice_stmt:
    if (const auto *t = as<splice_stmt>(&n)) {
      one(t->expr);
    }
    return;

  // ---- Types -------------------------------------------------------------
  case node_kind::named_type:
    if (const auto *t = as<named_type>(&n)) {
      type_arg_children(t->type_args);
    }
    return;
  case node_kind::bound_type:
    if (const auto *t = as<bound_type>(&n)) {
      bound_children(t->value);
    }
    return;
  case node_kind::existential_type:
    if (const auto *t = as<existential_type>(&n)) {
      bound_children(t->value);
    }
    return;
  case node_kind::tuple_type:
    if (const auto *t = as<tuple_type>(&n)) {
      all(t->elements);
    }
    return;
  case node_kind::slice_type:
    if (const auto *t = as<slice_type>(&n)) {
      one(t->element);
    }
    return;
  case node_kind::array_type:
    if (const auto *t = as<array_type>(&n)) {
      one(t->element);
      one(t->size);
    }
    return;
  case node_kind::ref_type:
    if (const auto *t = as<ref_type>(&n)) {
      one(t->inner);
    }
    return;
  case node_kind::ptr_type:
    if (const auto *t = as<ptr_type>(&n)) {
      one(t->inner);
    }
    return;
  case node_kind::mut_type:
    if (const auto *t = as<mut_type>(&n)) {
      one(t->inner);
    }
    return;
  case node_kind::fn_type:
    if (const auto *t = as<fn_type>(&n)) {
      all(t->param_types);
      one(t->return_type);
    }
    return;
  case node_kind::splice_type:
    if (const auto *t = as<splice_type>(&n)) {
      one(t->operand);
    }
    return;
  case node_kind::union_type:
    if (const auto *t = as<union_type>(&n)) {
      all(t->alternatives);
    }
    return;
  case node_kind::refinement_type:
    if (const auto *t = as<refinement_type>(&n)) {
      one(t->base);
      one(t->predicate);
    }
    return;

  // ---- Statements --------------------------------------------------------
  case node_kind::let_stmt:
    if (const auto *t = as<let_stmt>(&n)) {
      one(t->pattern);
      one(t->type_annotation);
      one(t->initializer);
      all(t->else_body);
    }
    return;
  case node_kind::var_stmt:
    if (const auto *t = as<var_stmt>(&n)) {
      one(t->type_annotation);
      one(t->initializer);
    }
    return;
  case node_kind::assign_stmt:
    if (const auto *t = as<assign_stmt>(&n)) {
      one(t->target);
      one(t->value);
    }
    return;
  case node_kind::expr_stmt:
    if (const auto *t = as<expr_stmt>(&n)) {
      one(t->expr);
    }
    return;
  case node_kind::return_stmt:
    if (const auto *t = as<return_stmt>(&n)) {
      one(t->value);
    }
    return;
  case node_kind::if_stmt:
    if (const auto *t = as<if_stmt>(&n)) {
      branch_children(t->branches, t->else_body);
    }
    return;
  case node_kind::while_stmt:
    if (const auto *t = as<while_stmt>(&n)) {
      one(t->condition);
      one(t->let_pattern);
      one(t->let_expr);
      all(t->body);
    }
    return;
  case node_kind::scope_stmt:
    if (const auto *t = as<scope_stmt>(&n)) {
      all(t->body);
    }
    return;
  case node_kind::for_stmt:
    if (const auto *t = as<for_stmt>(&n)) {
      all(t->patterns);
      one(t->iterable);
      one(t->guard);
      all(t->body);
    }
    return;
  case node_kind::match_stmt:
    if (const auto *t = as<match_stmt>(&n)) {
      one(t->subject);
      arm_children(t->arms);
    }
    return;
  case node_kind::crew_stmt:
    if (const auto *t = as<crew_stmt>(&n)) {
      all(t->body);
    }
    return;

  // ---- Expressions -------------------------------------------------------
  case node_kind::interpolated_string_expr:
    if (const auto *t = as<interpolated_string_expr>(&n)) {
      for (const auto &segment : t->segments) {
        if (segment.is_literal) {
          continue;
        }
        one(segment.value);
        format_dimension(segment.spec.width);
        format_dimension(segment.spec.precision);
      }
    }
    return;
  case node_kind::binary_expr:
    if (const auto *t = as<binary_expr>(&n)) {
      one(t->lhs);
      one(t->rhs);
    }
    return;
  case node_kind::unary_expr:
    if (const auto *t = as<unary_expr>(&n)) {
      one(t->operand);
    }
    return;
  case node_kind::call_expr:
    if (const auto *t = as<call_expr>(&n)) {
      one(t->callee);
      for (const auto &arg : t->args) {
        one(arg.value);
      }
    }
    return;
  case node_kind::index_expr:
    if (const auto *t = as<index_expr>(&n)) {
      one(t->object);
      one(t->index);
    }
    return;
  case node_kind::field_expr:
    if (const auto *t = as<field_expr>(&n)) {
      one(t->object);
      all(t->generic_args);
    }
    return;
  case node_kind::cast_expr:
    if (const auto *t = as<cast_expr>(&n)) {
      one(t->operand);
      one(t->target_type);
    }
    return;
  case node_kind::try_expr:
    if (const auto *t = as<try_expr>(&n)) {
      one(t->operand);
    }
    return;
  case node_kind::tuple_expr:
    if (const auto *t = as<tuple_expr>(&n)) {
      all(t->elements);
    }
    return;
  case node_kind::array_expr:
    if (const auto *t = as<array_expr>(&n)) {
      all(t->elements);
      one(t->fill_value);
      one(t->fill_count);
    }
    return;
  case node_kind::struct_expr:
    if (const auto *t = as<struct_expr>(&n)) {
      one(t->type_name);
      type_arg_children(t->type_args);
      for (const auto &field : t->fields) {
        one(field.value);
      }
    }
    return;
  case node_kind::lambda_expr:
    if (const auto *t = as<lambda_expr>(&n)) {
      for (const auto &param : t->params) {
        one(param.pattern);
        one(param.type_annotation);
      }
      one(t->return_type);
      one(t->body_expr);
      all(t->body_stmts);
    }
    return;
  case node_kind::match_expr:
    if (const auto *t = as<match_expr>(&n)) {
      one(t->subject);
      arm_children(t->arms);
    }
    return;
  case node_kind::if_expr:
    if (const auto *t = as<if_expr>(&n)) {
      branch_children(t->branches, t->else_body);
    }
    return;
  case node_kind::for_expr:
    if (const auto *t = as<for_expr>(&n)) {
      for (const auto &clause : t->clauses) {
        all(clause.patterns);
        one(clause.iterable);
      }
      one(t->guard);
      one(t->yield_expr);
    }
    return;
  case node_kind::await_expr:
    if (const auto *t = as<await_expr>(&n)) {
      one(t->operand);
    }
    return;
  case node_kind::yield_expr:
    if (const auto *t = as<yield_expr>(&n)) {
      one(t->value);
    }
    return;
  case node_kind::async_expr:
    if (const auto *t = as<async_expr>(&n)) {
      all(t->body);
    }
    return;
  case node_kind::par_expr:
    if (const auto *t = as<par_expr>(&n)) {
      all(t->branches);
    }
    return;
  case node_kind::race_expr:
    if (const auto *t = as<race_expr>(&n)) {
      all(t->branches);
    }
    return;
  case node_kind::crew_expr:
    if (const auto *t = as<crew_expr>(&n)) {
      all(t->body);
    }
    return;
  case node_kind::on_expr:
    if (const auto *t = as<on_expr>(&n)) {
      one(t->context_type);
      one(t->sender);
      all(t->body);
    }
    return;
  case node_kind::block_expr:
    if (const auto *t = as<block_expr>(&n)) {
      all(t->stmts);
    }
    return;
  case node_kind::quote_expr:
    if (const auto *t = as<quote_expr>(&n)) {
      one(t->parsed_body);
    }
    return;
  case node_kind::splice_expr:
    if (const auto *t = as<splice_expr>(&n)) {
      one(t->operand);
    }
    return;
  case node_kind::static_expr:
    if (const auto *t = as<static_expr>(&n)) {
      one(t->operand);
    }
    return;
  case node_kind::group_expr:
    if (const auto *t = as<group_expr>(&n)) {
      one(t->inner);
    }
    return;
  case node_kind::where_expr:
    if (const auto *t = as<where_expr>(&n)) {
      for (const auto &binding : t->bindings) {
        one(binding.value);
      }
      one(t->inner);
    }
    return;

  // ---- Patterns ----------------------------------------------------------
  case node_kind::constructor_pattern:
    if (const auto *t = as<constructor_pattern>(&n)) {
      all(t->args);
    }
    return;
  case node_kind::tuple_pattern:
    if (const auto *t = as<tuple_pattern>(&n)) {
      all(t->elements);
    }
    return;
  case node_kind::struct_pattern:
    if (const auto *t = as<struct_pattern>(&n)) {
      for (const auto &field : t->fields) {
        one(field.pattern);
      }
    }
    return;
  case node_kind::array_pattern:
    if (const auto *t = as<array_pattern>(&n)) {
      all(t->elements);
    }
    return;
  case node_kind::range_pattern:
    if (const auto *t = as<range_pattern>(&n)) {
      one(t->start);
      one(t->end);
    }
    return;
  case node_kind::option_pattern:
    if (const auto *t = as<option_pattern>(&n)) {
      one(t->inner);
    }
    return;
  case node_kind::result_pattern:
    if (const auto *t = as<result_pattern>(&n)) {
      one(t->inner);
    }
    return;
  case node_kind::ref_pattern:
    if (const auto *t = as<ref_pattern>(&n)) {
      one(t->inner);
    }
    return;
  case node_kind::or_pattern:
    if (const auto *t = as<or_pattern>(&n)) {
      all(t->alternatives);
    }
    return;
  case node_kind::group_pattern:
    if (const auto *t = as<group_pattern>(&n)) {
      one(t->inner);
    }
    return;
  }
}

/// Pre-order walk of `n`'s whole subtree, `n` included. `visit` returns
/// `bool`: true descends into the node's children, false skips them.
template <typename N, typename F> auto walk(N &n, F &&visit) -> void {
  if (!visit(n)) {
    return;
  }
  for_each_child(n, [&](auto &child) -> void { walk(child, visit); });
}

} // namespace cinder::ast
