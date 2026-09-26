#pragma once

#include "src/hir/nodes.h"

namespace cinder::hir {

// Generic HIR traversal shared by the passes that rewrite or measure a
// lowered function body (`inline.cpp`, `frame_budget.cpp`).

/// Calls `f` on every non-null child slot of `node` — a `ptr<T>&` for the
/// slot's own `T` (`hir_expr`, `hir_node`, `hir_block` or `hir_pattern`), so
/// a caller can replace a child in place. Descends into a lambda's body like
/// any other child; callers that must not, check for `hir_lambda` first.
template <typename F> auto for_each_child(hir_node &node, F &&f) -> void {
  const auto visit = [&](auto &slot) -> void {
    if (slot != nullptr) {
      f(slot);
    }
  };
  const auto visit_all = [&](auto &slots) -> void {
    for (auto &slot : slots) {
      visit(slot);
    }
  };
  switch (node.kind) {
  case hir_node_kind::hir_literal:
  case hir_node_kind::hir_local_ref:
  case hir_node_kind::hir_global_ref:
  case hir_node_kind::hir_stack_buffer:
  case hir_node_kind::hir_wildcard_pattern:
  case hir_node_kind::hir_literal_pattern:
  case hir_node_kind::hir_break:
  case hir_node_kind::hir_continue:
  case hir_node_kind::hir_function:
  case hir_node_kind::hir_module:
    return;
  case hir_node_kind::hir_binary: {
    auto &n = dynamic_cast<hir_binary &>(node);
    visit(n.lhs);
    visit(n.rhs);
    return;
  }
  case hir_node_kind::hir_unary:
    visit(dynamic_cast<hir_unary &>(node).operand);
    return;
  case hir_node_kind::hir_call: {
    auto &n = dynamic_cast<hir_call &>(node);
    visit(n.callee);
    visit_all(n.args);
    return;
  }
  case hir_node_kind::hir_field:
    visit(dynamic_cast<hir_field &>(node).object);
    return;
  case hir_node_kind::hir_index: {
    auto &n = dynamic_cast<hir_index &>(node);
    visit(n.object);
    visit(n.index);
    return;
  }
  case hir_node_kind::hir_tuple:
    visit_all(dynamic_cast<hir_tuple &>(node).elements);
    return;
  case hir_node_kind::hir_struct_init:
    for (auto &field : dynamic_cast<hir_struct_init &>(node).fields) {
      visit(field.value);
    }
    return;
  case hir_node_kind::hir_array_init: {
    auto &n = dynamic_cast<hir_array_init &>(node);
    visit_all(n.elements);
    visit(n.fill_value);
    visit(n.fill_count);
    return;
  }
  case hir_node_kind::hir_cast:
    visit(dynamic_cast<hir_cast &>(node).operand);
    return;
  case hir_node_kind::hir_block:
    visit_all(dynamic_cast<hir_block &>(node).stmts);
    return;
  case hir_node_kind::hir_if: {
    auto &n = dynamic_cast<hir_if &>(node);
    for (auto &branch : n.branches) {
      visit(branch.condition);
      visit(branch.body);
    }
    visit(n.else_body);
    return;
  }
  case hir_node_kind::hir_match: {
    auto &n = dynamic_cast<hir_match &>(node);
    visit(n.subject);
    for (auto &arm : n.arms) {
      visit(arm.pattern);
      visit(arm.guard);
      visit(arm.body);
    }
    return;
  }
  case hir_node_kind::hir_lambda:
    visit(dynamic_cast<hir_lambda &>(node).body);
    return;
  case hir_node_kind::hir_tuple_index:
    visit(dynamic_cast<hir_tuple_index &>(node).object);
    return;
  case hir_node_kind::hir_variant_payload:
    visit(dynamic_cast<hir_variant_payload &>(node).object);
    return;
  case hir_node_kind::hir_variant_init:
    visit_all(dynamic_cast<hir_variant_init &>(node).args);
    return;
  case hir_node_kind::hir_container_data:
    visit(dynamic_cast<hir_container_data &>(node).object);
    return;
  case hir_node_kind::hir_slice_from_raw_parts: {
    auto &n = dynamic_cast<hir_slice_from_raw_parts &>(node);
    visit(n.pointer);
    visit(n.len);
    return;
  }
  case hir_node_kind::hir_container_len:
    visit(dynamic_cast<hir_container_len &>(node).object);
    return;
  case hir_node_kind::hir_generator_next:
    visit(dynamic_cast<hir_generator_next &>(node).object);
    return;
  case hir_node_kind::hir_str_decode_scalar: {
    auto &n = dynamic_cast<hir_str_decode_scalar &>(node);
    visit(n.object);
    visit(n.byte_offset);
    return;
  }
  case hir_node_kind::hir_str_scalar_width: {
    auto &n = dynamic_cast<hir_str_scalar_width &>(node);
    visit(n.object);
    visit(n.byte_offset);
    return;
  }
  case hir_node_kind::hir_cell_set: {
    auto &n = dynamic_cast<hir_cell_set &>(node);
    visit(n.cell);
    visit(n.value);
    return;
  }
  case hir_node_kind::hir_or_pattern:
    visit_all(dynamic_cast<hir_or_pattern &>(node).alternatives);
    return;
  case hir_node_kind::hir_tuple_pattern:
    visit_all(dynamic_cast<hir_tuple_pattern &>(node).elements);
    return;
  case hir_node_kind::hir_array_pattern:
    visit_all(dynamic_cast<hir_array_pattern &>(node).elements);
    return;
  case hir_node_kind::hir_struct_pattern:
    for (auto &field : dynamic_cast<hir_struct_pattern &>(node).fields) {
      visit(field.pattern);
    }
    return;
  case hir_node_kind::hir_constructor_pattern:
    visit_all(dynamic_cast<hir_constructor_pattern &>(node).args);
    return;
  case hir_node_kind::hir_range_pattern: {
    auto &n = dynamic_cast<hir_range_pattern &>(node);
    visit(n.start);
    visit(n.end);
    return;
  }
  case hir_node_kind::hir_let:
    visit(dynamic_cast<hir_let &>(node).initializer);
    return;
  case hir_node_kind::hir_let_else: {
    auto &n = dynamic_cast<hir_let_else &>(node);
    visit(n.initializer);
    visit(n.pattern);
    visit(n.else_body);
    return;
  }
  case hir_node_kind::hir_assign: {
    auto &n = dynamic_cast<hir_assign &>(node);
    visit(n.target);
    visit(n.value);
    return;
  }
  case hir_node_kind::hir_expr_stmt:
    visit(dynamic_cast<hir_expr_stmt &>(node).expr);
    return;
  case hir_node_kind::hir_return:
    visit(dynamic_cast<hir_return &>(node).value);
    return;
  case hir_node_kind::hir_yield:
    visit(dynamic_cast<hir_yield &>(node).value);
    return;
  case hir_node_kind::hir_while: {
    auto &n = dynamic_cast<hir_while &>(node);
    visit(n.condition);
    visit(n.body);
    visit(n.step);
    return;
  }
  case hir_node_kind::hir_while_let: {
    auto &n = dynamic_cast<hir_while_let &>(node);
    visit(n.subject);
    visit(n.pattern);
    visit(n.body);
    return;
  }
  case hir_node_kind::hir_contract_check:
    visit(dynamic_cast<hir_contract_check &>(node).condition);
    return;
  }
}

/// Pre-order walk of `node`'s whole subtree, lambda bodies included.
template <typename F> auto walk(hir_node &node, F &&visit) -> void {
  visit(node);
  for_each_child(node, [&](auto &slot) -> void { walk(*slot, visit); });
}

} // namespace cinder::hir
