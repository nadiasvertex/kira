#include "src/hir/tail_calls.h"

#include <unordered_set>

#include "src/intrinsics.h"

namespace kira::hir {

namespace {

/// Recursively records every symbol a `let`/pattern binding introduces
/// anywhere in `node`'s subtree — never descending into a nested
/// `hir_lambda`'s body, which is a separate scope (see `mark_tail_calls`'s
/// doc comment). Over-collecting a symbol bound in a sibling or nested
/// block that isn't actually visible at a given call site is harmless: it
/// only ever makes a call *less* eligible, never wrongly eligible, since a
/// real function reference's symbol id can never coincide with one this
/// pass hands out for a `let`/parameter binding.
auto collect_bound_symbols(const hir_node &node,
                           std::unordered_set<symbol_id> &bound) -> void {
  switch (node.kind) {
  case hir_node_kind::hir_block: {
    const auto &block = static_cast<const hir_block &>(node);
    for (const auto &stmt : block.stmts) {
      collect_bound_symbols(*stmt, bound);
    }
    return;
  }
  case hir_node_kind::hir_let: {
    const auto &let = static_cast<const hir_let &>(node);
    bound.insert(let.symbol);
    collect_bound_symbols(*let.initializer, bound);
    return;
  }
  case hir_node_kind::hir_let_else: {
    const auto &let_else = static_cast<const hir_let_else &>(node);
    bound.insert(let_else.subject_symbol);
    collect_bound_symbols(*let_else.initializer, bound);
    collect_bound_symbols(*let_else.else_body, bound);
    return;
  }
  case hir_node_kind::hir_assign: {
    const auto &assign = static_cast<const hir_assign &>(node);
    collect_bound_symbols(*assign.target, bound);
    collect_bound_symbols(*assign.value, bound);
    return;
  }
  case hir_node_kind::hir_expr_stmt: {
    collect_bound_symbols(*static_cast<const hir_expr_stmt &>(node).expr,
                          bound);
    return;
  }
  case hir_node_kind::hir_return: {
    const auto &ret = static_cast<const hir_return &>(node);
    if (ret.value != nullptr) {
      collect_bound_symbols(*ret.value, bound);
    }
    return;
  }
  case hir_node_kind::hir_yield: {
    collect_bound_symbols(*static_cast<const hir_yield &>(node).value, bound);
    return;
  }
  case hir_node_kind::hir_while: {
    const auto &loop = static_cast<const hir_while &>(node);
    collect_bound_symbols(*loop.condition, bound);
    collect_bound_symbols(*loop.body, bound);
    if (loop.step != nullptr) {
      collect_bound_symbols(*loop.step, bound);
    }
    return;
  }
  case hir_node_kind::hir_while_let: {
    const auto &loop = static_cast<const hir_while_let &>(node);
    bound.insert(loop.subject_symbol);
    collect_bound_symbols(*loop.subject, bound);
    collect_bound_symbols(*loop.body, bound);
    return;
  }
  case hir_node_kind::hir_list_push: {
    const auto &push = static_cast<const hir_list_push &>(node);
    collect_bound_symbols(*push.target, bound);
    collect_bound_symbols(*push.value, bound);
    return;
  }
  case hir_node_kind::hir_contract_check: {
    collect_bound_symbols(
        *static_cast<const hir_contract_check &>(node).condition, bound);
    return;
  }
  case hir_node_kind::hir_if: {
    const auto &iff = static_cast<const hir_if &>(node);
    for (const auto &branch : iff.branches) {
      collect_bound_symbols(*branch.condition, bound);
      collect_bound_symbols(*branch.body, bound);
    }
    if (iff.else_body != nullptr) {
      collect_bound_symbols(*iff.else_body, bound);
    }
    return;
  }
  case hir_node_kind::hir_match: {
    const auto &match = static_cast<const hir_match &>(node);
    bound.insert(match.subject_symbol);
    collect_bound_symbols(*match.subject, bound);
    for (const auto &arm : match.arms) {
      if (arm.guard != nullptr) {
        collect_bound_symbols(*arm.guard, bound);
      }
      collect_bound_symbols(*arm.body, bound);
    }
    return;
  }
  case hir_node_kind::hir_call: {
    const auto &call = static_cast<const hir_call &>(node);
    collect_bound_symbols(*call.callee, bound);
    for (const auto &arg : call.args) {
      collect_bound_symbols(*arg, bound);
    }
    return;
  }
  case hir_node_kind::hir_binary: {
    const auto &bin = static_cast<const hir_binary &>(node);
    collect_bound_symbols(*bin.lhs, bound);
    collect_bound_symbols(*bin.rhs, bound);
    return;
  }
  case hir_node_kind::hir_unary: {
    collect_bound_symbols(*static_cast<const hir_unary &>(node).operand, bound);
    return;
  }
  case hir_node_kind::hir_cast: {
    collect_bound_symbols(*static_cast<const hir_cast &>(node).operand, bound);
    return;
  }
  case hir_node_kind::hir_field: {
    collect_bound_symbols(*static_cast<const hir_field &>(node).object, bound);
    return;
  }
  case hir_node_kind::hir_index: {
    const auto &idx = static_cast<const hir_index &>(node);
    collect_bound_symbols(*idx.object, bound);
    collect_bound_symbols(*idx.index, bound);
    return;
  }
  case hir_node_kind::hir_tuple: {
    for (const auto &elem : static_cast<const hir_tuple &>(node).elements) {
      collect_bound_symbols(*elem, bound);
    }
    return;
  }
  case hir_node_kind::hir_struct_init: {
    for (const auto &field :
         static_cast<const hir_struct_init &>(node).fields) {
      collect_bound_symbols(*field.value, bound);
    }
    return;
  }
  case hir_node_kind::hir_array_init: {
    const auto &arr = static_cast<const hir_array_init &>(node);
    for (const auto &elem : arr.elements) {
      collect_bound_symbols(*elem, bound);
    }
    if (arr.fill_value != nullptr) {
      collect_bound_symbols(*arr.fill_value, bound);
    }
    if (arr.fill_count != nullptr) {
      collect_bound_symbols(*arr.fill_count, bound);
    }
    return;
  }
  case hir_node_kind::hir_tuple_index: {
    collect_bound_symbols(*static_cast<const hir_tuple_index &>(node).object,
                          bound);
    return;
  }
  case hir_node_kind::hir_variant_payload: {
    collect_bound_symbols(
        *static_cast<const hir_variant_payload &>(node).object, bound);
    return;
  }
  case hir_node_kind::hir_variant_init: {
    for (const auto &arg : static_cast<const hir_variant_init &>(node).args) {
      collect_bound_symbols(*arg, bound);
    }
    return;
  }
  case hir_node_kind::hir_container_len: {
    collect_bound_symbols(*static_cast<const hir_container_len &>(node).object,
                          bound);
    return;
  }
  case hir_node_kind::hir_str_decode_scalar: {
    const auto &n = static_cast<const hir_str_decode_scalar &>(node);
    collect_bound_symbols(*n.object, bound);
    collect_bound_symbols(*n.byte_offset, bound);
    return;
  }
  case hir_node_kind::hir_str_scalar_width: {
    const auto &n = static_cast<const hir_str_scalar_width &>(node);
    collect_bound_symbols(*n.object, bound);
    collect_bound_symbols(*n.byte_offset, bound);
    return;
  }
  case hir_node_kind::hir_generator_next: {
    collect_bound_symbols(*static_cast<const hir_generator_next &>(node).object,
                          bound);
    return;
  }
  // A lambda is a separate scope compiled as its own function — see
  // `mark_tail_calls`'s doc comment on why its body is never descended
  // into. Its own captures/params contribute nothing to this function's
  // bound-symbol set.
  case hir_node_kind::hir_lambda:
  case hir_node_kind::hir_local_ref:
  case hir_node_kind::hir_global_ref:
  case hir_node_kind::hir_literal:
  case hir_node_kind::hir_break:
  case hir_node_kind::hir_wildcard_pattern:
  case hir_node_kind::hir_literal_pattern:
  case hir_node_kind::hir_or_pattern:
  case hir_node_kind::hir_tuple_pattern:
  case hir_node_kind::hir_struct_pattern:
  case hir_node_kind::hir_constructor_pattern:
  case hir_node_kind::hir_range_pattern:
  case hir_node_kind::hir_array_pattern:
  case hir_node_kind::hir_continue:
  case hir_node_kind::hir_function:
  case hir_node_kind::hir_module:
    return;
  }
}

[[nodiscard]] auto
is_eligible_tail_callee(const hir_expr &callee,
                        const std::unordered_set<symbol_id> &bound) -> bool {
  if (callee.kind != hir_node_kind::hir_local_ref) {
    // Not a bare name — an indirect/closure call (computed callee
    // expression, immediately-invoked lambda, etc.). Decision 2 excludes
    // every shape but a direct, statically-resolved call.
    return false;
  }
  const auto &ref = static_cast<const hir_local_ref &>(callee);
  if (bound.contains(ref.symbol)) {
    // Bound as a parameter or `let` within this function — a local
    // variable (possibly holding a closure), never a direct function call.
    return false;
  }
  // Every other `hir_local_ref` in call position resolves to a real
  // module-level function by construction (mirrors `compile_call` in both
  // backends) — except an `intrinsic def` name, which never gets a
  // `musttail`/frame-reuse treatment (it has no HIR body/frame of its own
  // to reuse into).
  return !kira::intrinsic_index_of(ref.name).has_value();
}

auto mark_tail_block(hir_block &block,
                     const std::unordered_set<symbol_id> &bound) -> void;

auto mark_tail_expr(hir_expr &expr, const std::unordered_set<symbol_id> &bound)
    -> void {
  switch (expr.kind) {
  case hir_node_kind::hir_call: {
    auto &call = static_cast<hir_call &>(expr);
    call.is_tail_call = is_eligible_tail_callee(*call.callee, bound);
    return;
  }
  case hir_node_kind::hir_if: {
    auto &iff = static_cast<hir_if &>(expr);
    for (auto &branch : iff.branches) {
      mark_tail_block(*branch.body, bound);
    }
    if (iff.else_body != nullptr) {
      mark_tail_block(*iff.else_body, bound);
    }
    return;
  }
  case hir_node_kind::hir_match: {
    auto &match = static_cast<hir_match &>(expr);
    for (auto &arm : match.arms) {
      mark_tail_block(*arm.body, bound);
    }
    return;
  }
  case hir_node_kind::hir_block: {
    mark_tail_block(static_cast<hir_block &>(expr), bound);
    return;
  }
  default:
    // Every other expression shape either can't be a call (a literal, a
    // field access) or, if it does contain one, that call is an operand
    // feeding a larger expression — non-tail by Decision 2's own rule
    // ("only the outermost call of `f(g(x))` could be tail, and it is
    // not, since its result feeds another operation").
    return;
  }
}

auto mark_tail_block(hir_block &block,
                     const std::unordered_set<symbol_id> &bound) -> void {
  if (block.stmts.empty()) {
    return;
  }
  auto &last = *block.stmts.back();
  switch (last.kind) {
  case hir_node_kind::hir_return: {
    auto &ret = static_cast<hir_return &>(last);
    if (ret.value != nullptr) {
      mark_tail_expr(*ret.value, bound);
    }
    return;
  }
  case hir_node_kind::hir_expr_stmt: {
    mark_tail_expr(*static_cast<hir_expr_stmt &>(last).expr, bound);
    return;
  }
  case hir_node_kind::hir_if: {
    auto &iff = static_cast<hir_if &>(last);
    for (auto &branch : iff.branches) {
      mark_tail_block(*branch.body, bound);
    }
    if (iff.else_body != nullptr) {
      mark_tail_block(*iff.else_body, bound);
    }
    return;
  }
  case hir_node_kind::hir_match: {
    auto &match = static_cast<hir_match &>(last);
    for (auto &arm : match.arms) {
      mark_tail_block(*arm.body, bound);
    }
    return;
  }
  case hir_node_kind::hir_block: {
    mark_tail_block(static_cast<hir_block &>(last), bound);
    return;
  }
  default:
    // A `let`, `var`, assignment, loop, `break`/`continue`, contract
    // check, or bare non-tail statement as the last line of a block puts
    // nothing in tail position.
    return;
  }
}

} // namespace

auto mark_tail_calls(hir_function &fn) -> void {
  if (fn.is_generator) {
    // This body doubles as the generator step function's body — excluded
    // outright (Decision 2 / Future extensions item 4).
    return;
  }

  auto bound = std::unordered_set<symbol_id>{};
  for (const auto &param : fn.params) {
    bound.insert(param.symbol);
  }
  collect_bound_symbols(*fn.body, bound);

  mark_tail_block(*fn.body, bound);
}

} // namespace kira::hir
