#include "src/hir/live_across_yield.h"

#include <unordered_map>
#include <unordered_set>

namespace kira::hir {

namespace {

/// Whether `block` (or anything it structurally contains, not descending
/// into a nested lambda's own body) has a `hir_yield` anywhere inside it.
/// Used to decide whether a loop's condition/subject must be treated as
/// re-evaluated after a suspension — see `live_across_yield.h`'s doc
/// comment on the conservative loop treatment.
auto block_contains_yield(const hir_block &block) -> bool;

auto stmt_contains_yield(const hir_node &node) -> bool {
  switch (node.kind) {
  case hir_node_kind::hir_yield:
    return true;
  case hir_node_kind::hir_let_else:
    return block_contains_yield(
        *dynamic_cast<const hir_let_else &>(node).else_body);
  case hir_node_kind::hir_while:
    return block_contains_yield(*dynamic_cast<const hir_while &>(node).body);
  case hir_node_kind::hir_while_let:
    return block_contains_yield(
        *dynamic_cast<const hir_while_let &>(node).body);
  case hir_node_kind::hir_if: {
    const auto &node2 = dynamic_cast<const hir_if &>(node);
    for (const auto &branch : node2.branches) {
      if (block_contains_yield(*branch.body)) {
        return true;
      }
    }
    return node2.else_body != nullptr && block_contains_yield(*node2.else_body);
  }
  case hir_node_kind::hir_match: {
    const auto &node2 = dynamic_cast<const hir_match &>(node);
    for (const auto &arm : node2.arms) {
      if (block_contains_yield(*arm.body)) {
        return true;
      }
    }
    return false;
  }
  case hir_node_kind::hir_block:
    return block_contains_yield(dynamic_cast<const hir_block &>(node));
  default:
    return false;
  }
}

auto block_contains_yield(const hir_block &block) -> bool {
  for (const auto &stmt : block.stmts) {
    if (stmt_contains_yield(*stmt)) {
      return true;
    }
  }
  return false;
}

/// Walking state: `declared_at` records the generation (see below) each
/// symbol was bound at; `live`/`seen_live` accumulate every symbol
/// referenced at a strictly later generation than its own binding, in
/// first-bound order, deduplicated. `generation` starts at 0 and increments
/// once per `hir_yield` walked, in program order — a symbol referenced
/// while `generation` is greater than the generation it was bound at must
/// have survived at least one suspension to be read again.
struct walker {
  std::unordered_map<symbol_id, int> declared_at;
  std::unordered_set<symbol_id> seen_live;
  std::vector<symbol_id> live;
  int generation = 0;

  auto bind(symbol_id sym) -> void {
    // A later binding of the same symbol_id can't happen in this language
    // (each `hir_let`/param mints a fresh id), so this never overwrites a
    // symbol already live at an earlier generation.
    declared_at.try_emplace(sym, generation);
  }

  auto reference(symbol_id sym) -> void {
    const auto it = declared_at.find(sym);
    if (it == declared_at.end() || it->second >= generation) {
      return;
    }
    if (seen_live.insert(sym).second) {
      live.push_back(sym);
    }
  }

  auto walk_expr(const hir_expr &expr) -> void {
    switch (expr.kind) {
    case hir_node_kind::hir_literal:
      return;
    case hir_node_kind::hir_local_ref:
      reference(dynamic_cast<const hir_local_ref &>(expr).symbol);
      return;
    case hir_node_kind::hir_binary: {
      const auto &node = dynamic_cast<const hir_binary &>(expr);
      walk_expr(*node.lhs);
      walk_expr(*node.rhs);
      return;
    }
    case hir_node_kind::hir_unary:
      walk_expr(*dynamic_cast<const hir_unary &>(expr).operand);
      return;
    case hir_node_kind::hir_call: {
      const auto &node = dynamic_cast<const hir_call &>(expr);
      walk_expr(*node.callee);
      for (const auto &arg : node.args) {
        walk_expr(*arg);
      }
      return;
    }
    case hir_node_kind::hir_field:
      walk_expr(*dynamic_cast<const hir_field &>(expr).object);
      return;
    case hir_node_kind::hir_index: {
      const auto &node = dynamic_cast<const hir_index &>(expr);
      walk_expr(*node.object);
      walk_expr(*node.index);
      return;
    }
    case hir_node_kind::hir_tuple:
      for (const auto &elem : dynamic_cast<const hir_tuple &>(expr).elements) {
        walk_expr(*elem);
      }
      return;
    case hir_node_kind::hir_struct_init:
      for (const auto &field :
           dynamic_cast<const hir_struct_init &>(expr).fields) {
        walk_expr(*field.value);
      }
      return;
    case hir_node_kind::hir_array_init: {
      const auto &node = dynamic_cast<const hir_array_init &>(expr);
      for (const auto &elem : node.elements) {
        walk_expr(*elem);
      }
      if (node.fill_value != nullptr) {
        walk_expr(*node.fill_value);
      }
      if (node.fill_count != nullptr) {
        walk_expr(*node.fill_count);
      }
      return;
    }
    case hir_node_kind::hir_cast:
      walk_expr(*dynamic_cast<const hir_cast &>(expr).operand);
      return;
    case hir_node_kind::hir_block:
      for (const auto &stmt : dynamic_cast<const hir_block &>(expr).stmts) {
        walk_stmt(*stmt);
      }
      return;
    case hir_node_kind::hir_if: {
      const auto &node = dynamic_cast<const hir_if &>(expr);
      for (const auto &branch : node.branches) {
        walk_expr(*branch.condition);
        for (const auto &stmt : branch.body->stmts) {
          walk_stmt(*stmt);
        }
      }
      if (node.else_body != nullptr) {
        for (const auto &stmt : node.else_body->stmts) {
          walk_stmt(*stmt);
        }
      }
      return;
    }
    case hir_node_kind::hir_match: {
      const auto &node = dynamic_cast<const hir_match &>(expr);
      walk_expr(*node.subject);
      bind(node.subject_symbol);
      for (const auto &arm : node.arms) {
        walk_pattern(*arm.pattern);
        if (arm.guard != nullptr) {
          walk_expr(*arm.guard);
        }
        for (const auto &stmt : arm.body->stmts) {
          walk_stmt(*stmt);
        }
      }
      return;
    }
    case hir_node_kind::hir_lambda: {
      // A generator body can't contain a nested `yield` inside a lambda
      // literal (a lambda is its own call frame, not part of the
      // generator's own suspension), so this only needs to track ordinary
      // captures, not generation bumps — walked with the same accumulators
      // as `free_variables` does, purely so any outer name it reads still
      // registers as an ordinary (non-yield-crossing) reference.
      const auto &node = dynamic_cast<const hir_lambda &>(expr);
      const auto outer_bound = declared_at;
      for (const auto &param : node.params) {
        bind(param.symbol);
      }
      for (const auto &stmt : node.body->stmts) {
        walk_stmt(*stmt);
      }
      declared_at = outer_bound;
      return;
    }
    case hir_node_kind::hir_tuple_index:
      walk_expr(*dynamic_cast<const hir_tuple_index &>(expr).object);
      return;
    case hir_node_kind::hir_variant_payload:
      walk_expr(*dynamic_cast<const hir_variant_payload &>(expr).object);
      return;
    case hir_node_kind::hir_variant_init:
      for (const auto &arg :
           dynamic_cast<const hir_variant_init &>(expr).args) {
        walk_expr(*arg);
      }
      return;
    case hir_node_kind::hir_container_len:
      walk_expr(*dynamic_cast<const hir_container_len &>(expr).object);
      return;
    default:
      return;
    }
  }

  auto walk_pattern(const hir_pattern &pattern) -> void {
    switch (pattern.kind) {
    case hir_node_kind::hir_or_pattern:
      for (const auto &alt :
           dynamic_cast<const hir_or_pattern &>(pattern).alternatives) {
        walk_pattern(*alt);
      }
      return;
    case hir_node_kind::hir_tuple_pattern:
      for (const auto &elem :
           dynamic_cast<const hir_tuple_pattern &>(pattern).elements) {
        walk_pattern(*elem);
      }
      return;
    case hir_node_kind::hir_array_pattern:
      for (const auto &elem :
           dynamic_cast<const hir_array_pattern &>(pattern).elements) {
        walk_pattern(*elem);
      }
      return;
    case hir_node_kind::hir_struct_pattern:
      for (const auto &field :
           dynamic_cast<const hir_struct_pattern &>(pattern).fields) {
        walk_pattern(*field.pattern);
      }
      return;
    case hir_node_kind::hir_constructor_pattern:
      for (const auto &arg :
           dynamic_cast<const hir_constructor_pattern &>(pattern).args) {
        walk_pattern(*arg);
      }
      return;
    case hir_node_kind::hir_range_pattern: {
      const auto &node = dynamic_cast<const hir_range_pattern &>(pattern);
      if (node.start != nullptr) {
        walk_expr(*node.start);
      }
      if (node.end != nullptr) {
        walk_expr(*node.end);
      }
      return;
    }
    default:
      return;
    }
  }

  auto walk_stmt(const hir_node &node) -> void {
    switch (node.kind) {
    case hir_node_kind::hir_let: {
      const auto &let = dynamic_cast<const hir_let &>(node);
      walk_expr(*let.initializer);
      bind(let.symbol);
      return;
    }
    case hir_node_kind::hir_let_else: {
      const auto &node2 = dynamic_cast<const hir_let_else &>(node);
      walk_expr(*node2.initializer);
      bind(node2.subject_symbol);
      walk_pattern(*node2.pattern);
      for (const auto &stmt : node2.else_body->stmts) {
        walk_stmt(*stmt);
      }
      return;
    }
    case hir_node_kind::hir_assign: {
      const auto &node2 = dynamic_cast<const hir_assign &>(node);
      walk_expr(*node2.target);
      walk_expr(*node2.value);
      return;
    }
    case hir_node_kind::hir_expr_stmt:
      walk_expr(*dynamic_cast<const hir_expr_stmt &>(node).expr);
      return;
    case hir_node_kind::hir_return: {
      const auto &node2 = dynamic_cast<const hir_return &>(node);
      if (node2.value != nullptr) {
        walk_expr(*node2.value);
      }
      return;
    }
    case hir_node_kind::hir_yield: {
      walk_expr(*dynamic_cast<const hir_yield &>(node).value);
      ++generation;
      return;
    }
    case hir_node_kind::hir_while: {
      const auto &node2 = dynamic_cast<const hir_while &>(node);
      if (block_contains_yield(*node2.body)) {
        // The condition is re-evaluated on every iteration, including
        // after a suspension inside the body — bump the generation before
        // walking it so anything it (or the body) reads from outside the
        // loop is conservatively treated as surviving a yield.
        ++generation;
      }
      walk_expr(*node2.condition);
      for (const auto &stmt : node2.body->stmts) {
        walk_stmt(*stmt);
      }
      return;
    }
    case hir_node_kind::hir_while_let: {
      const auto &node2 = dynamic_cast<const hir_while_let &>(node);
      if (block_contains_yield(*node2.body)) {
        ++generation;
      }
      walk_expr(*node2.subject);
      bind(node2.subject_symbol);
      walk_pattern(*node2.pattern);
      for (const auto &stmt : node2.body->stmts) {
        walk_stmt(*stmt);
      }
      return;
    }
    case hir_node_kind::hir_list_push: {
      const auto &node2 = dynamic_cast<const hir_list_push &>(node);
      walk_expr(*node2.target);
      walk_expr(*node2.value);
      return;
    }
    case hir_node_kind::hir_contract_check:
      // A contract condition is ordinary code — it reads locals and calls
      // pure functions like any other expression, and this walk must see
      // those reads. (It can't fall through to the `hir_expr` downcast
      // below: `hir_contract_check` is a statement.)
      walk_expr(*dynamic_cast<const hir_contract_check &>(node).condition);
      return;
    default:
      walk_expr(dynamic_cast<const hir_expr &>(node));
      return;
    }
  }
};

} // namespace

auto live_across_yield(const hir_function &fn) -> std::vector<symbol_id> {
  auto w = walker{};
  for (const auto &param : fn.params) {
    w.bind(param.symbol);
  }
  if (fn.body != nullptr) {
    for (const auto &stmt : fn.body->stmts) {
      w.walk_stmt(*stmt);
    }
  }
  return std::move(w.live);
}

} // namespace kira::hir
