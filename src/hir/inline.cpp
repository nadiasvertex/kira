#include "src/hir/inline.h"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "src/hir/tail_calls.h"
#include "src/intrinsics.h"

namespace kira::hir {

namespace {

using semantic::type_kind;
using semantic::type_table;

/// Largest callee body, in HIR nodes, worth copying into a caller. `list`'s
/// `at`/`set_at`/`push` sit well under it; anything doing real work is well
/// over it, and pays for its call frame many times over anyway.
constexpr size_t k_max_callee_nodes = 64;
/// How many levels of calls-within-inlined-copies are expanded.
constexpr size_t k_max_depth = 3;
/// A caller this large stops growing: past it, more copies cost more in
/// code size than they save in frames.
constexpr size_t k_max_caller_nodes = 4000;

// --------------------------------------------------------------------------
//  Traversal
// --------------------------------------------------------------------------

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

[[nodiscard]] auto count_nodes(hir_node &node) -> size_t {
  auto count = size_t{0};
  walk(node, [&](hir_node &) -> void { ++count; });
  return count;
}

/// Every symbol a binding site under `node` introduces: `let`, `let else`,
/// `match`/`while let` subjects, and lambda parameters.
auto collect_binders(hir_node &node, std::unordered_set<symbol_id> &out)
    -> void {
  walk(node, [&](hir_node &n) -> void {
    switch (n.kind) {
    case hir_node_kind::hir_let:
      out.insert(dynamic_cast<hir_let &>(n).symbol);
      return;
    case hir_node_kind::hir_let_else:
      out.insert(dynamic_cast<hir_let_else &>(n).subject_symbol);
      return;
    case hir_node_kind::hir_match:
      out.insert(dynamic_cast<hir_match &>(n).subject_symbol);
      return;
    case hir_node_kind::hir_while_let:
      out.insert(dynamic_cast<hir_while_let &>(n).subject_symbol);
      return;
    case hir_node_kind::hir_lambda:
      for (const auto &param : dynamic_cast<hir_lambda &>(n).params) {
        out.insert(param.symbol);
      }
      return;
    default:
      return;
    }
  });
}

/// The largest symbol id `fn` mentions anywhere, bound or referenced.
[[nodiscard]] auto max_symbol(hir_function &fn) -> symbol_id {
  auto top = symbol_id{0};
  const auto note = [&](symbol_id s) -> void {
    if (s != k_invalid_symbol_id) {
      top = std::max(top, s);
    }
  };
  for (const auto &param : fn.params) {
    note(param.symbol);
  }
  auto binders = std::unordered_set<symbol_id>{};
  collect_binders(*fn.body, binders);
  for (const auto s : binders) {
    note(s);
  }
  walk(*fn.body, [&](hir_node &n) -> void {
    if (n.kind == hir_node_kind::hir_local_ref) {
      note(dynamic_cast<hir_local_ref &>(n).symbol);
    } else if (n.kind == hir_node_kind::hir_lambda) {
      const auto &lambda = dynamic_cast<hir_lambda &>(n);
      if (lambda.captures.has_value()) {
        for (const auto &capture : *lambda.captures) {
          note(capture.symbol);
        }
      }
    }
  });
  return top;
}

// --------------------------------------------------------------------------
//  Cloning
// --------------------------------------------------------------------------

/// How a clone renames symbols. With `next` null the clone is exact; with it
/// set, every distinct symbol is mapped, on first sight, to `*next` (which
/// then advances) — so references inside the copy stay consistent with each
/// other and collide with nothing already in the caller.
struct symbol_renamer {
  symbol_id *next = nullptr;
  std::unordered_map<symbol_id, symbol_id> renamed;

  [[nodiscard]] auto operator()(symbol_id s) -> symbol_id {
    if (next == nullptr || s == k_invalid_symbol_id) {
      return s;
    }
    const auto [it, inserted] = renamed.try_emplace(s, *next);
    if (inserted) {
      ++*next;
    }
    return it->second;
  }
};

[[nodiscard]] auto clone_node(const hir_node &node, symbol_renamer &rename)
    -> ptr<hir_node>;

template <typename T>
[[nodiscard]] auto clone_as(const T &node, symbol_renamer &rename) -> ptr<T> {
  auto copy = clone_node(node, rename);
  return ptr<T>(dynamic_cast<T *>(copy.release()));
}

template <typename T>
[[nodiscard]] auto clone_opt(const ptr<T> &node, symbol_renamer &rename)
    -> ptr<T> {
  return node == nullptr ? nullptr : clone_as(*node, rename);
}

template <typename T>
[[nodiscard]] auto clone_all(const ptr_vec<T> &nodes, symbol_renamer &rename)
    -> ptr_vec<T> {
  auto out = ptr_vec<T>{};
  out.reserve(nodes.size());
  for (const auto &node : nodes) {
    out.push_back(clone_as(*node, rename));
  }
  return out;
}

[[nodiscard]] auto clone_params(const std::vector<hir_param> &params,
                                symbol_renamer &rename)
    -> std::vector<hir_param> {
  auto out = std::vector<hir_param>{};
  out.reserve(params.size());
  for (const auto &param : params) {
    out.push_back(hir_param{
        .symbol = rename(param.symbol), .name = param.name, .type = param.type});
  }
  return out;
}

/// Copies a pattern's own `subject_type` onto its clone — every pattern
/// kind carries one, set by lowering only where a backend cannot derive it.
template <typename T>
[[nodiscard]] auto with_subject(ptr<T> pattern, const hir_pattern &from)
    -> ptr<hir_node> {
  pattern->subject_type = from.subject_type;
  return pattern;
}

auto clone_node(const hir_node &node, symbol_renamer &rename)
    -> ptr<hir_node> {
  const auto span = node.span;
  const auto type = node.type;
  switch (node.kind) {
  case hir_node_kind::hir_literal: {
    const auto &n = dynamic_cast<const hir_literal &>(node);
    return hir::make<hir_literal>(span, type, n.lit_kind, n.value);
  }
  case hir_node_kind::hir_local_ref: {
    const auto &n = dynamic_cast<const hir_local_ref &>(node);
    return hir::make<hir_local_ref>(span, type, rename(n.symbol), n.name,
                               n.owner_module);
  }
  case hir_node_kind::hir_global_ref: {
    const auto &n = dynamic_cast<const hir_global_ref &>(node);
    return hir::make<hir_global_ref>(span, type, n.name, n.owner_module);
  }
  case hir_node_kind::hir_binary: {
    const auto &n = dynamic_cast<const hir_binary &>(node);
    return hir::make<hir_binary>(span, type, n.op, clone_as(*n.lhs, rename),
                            clone_as(*n.rhs, rename));
  }
  case hir_node_kind::hir_unary: {
    const auto &n = dynamic_cast<const hir_unary &>(node);
    return hir::make<hir_unary>(span, type, n.op, clone_as(*n.operand, rename));
  }
  case hir_node_kind::hir_call: {
    const auto &n = dynamic_cast<const hir_call &>(node);
    auto copy = hir::make<hir_call>(span, type, clone_as(*n.callee, rename),
                               clone_all(n.args, rename));
    copy->is_tail_call = n.is_tail_call;
    return copy;
  }
  case hir_node_kind::hir_field: {
    const auto &n = dynamic_cast<const hir_field &>(node);
    return hir::make<hir_field>(span, type, clone_as(*n.object, rename),
                           n.field_name);
  }
  case hir_node_kind::hir_index: {
    const auto &n = dynamic_cast<const hir_index &>(node);
    return hir::make<hir_index>(span, type, clone_as(*n.object, rename),
                           clone_as(*n.index, rename));
  }
  case hir_node_kind::hir_tuple: {
    const auto &n = dynamic_cast<const hir_tuple &>(node);
    return hir::make<hir_tuple>(span, type, clone_all(n.elements, rename));
  }
  case hir_node_kind::hir_struct_init: {
    const auto &n = dynamic_cast<const hir_struct_init &>(node);
    auto fields = std::vector<hir_struct_init_field>{};
    fields.reserve(n.fields.size());
    for (const auto &field : n.fields) {
      fields.push_back(hir_struct_init_field{
          .name = field.name, .value = clone_as(*field.value, rename)});
    }
    return hir::make<hir_struct_init>(span, type, std::move(fields));
  }
  case hir_node_kind::hir_array_init: {
    const auto &n = dynamic_cast<const hir_array_init &>(node);
    return hir::make<hir_array_init>(span, type, clone_all(n.elements, rename),
                                clone_opt(n.fill_value, rename),
                                clone_opt(n.fill_count, rename));
  }
  case hir_node_kind::hir_cast: {
    const auto &n = dynamic_cast<const hir_cast &>(node);
    return hir::make<hir_cast>(span, type, clone_as(*n.operand, rename));
  }
  case hir_node_kind::hir_block: {
    const auto &n = dynamic_cast<const hir_block &>(node);
    return hir::make<hir_block>(span, type, clone_all(n.stmts, rename));
  }
  case hir_node_kind::hir_if: {
    const auto &n = dynamic_cast<const hir_if &>(node);
    auto branches = std::vector<hir_if_branch>{};
    branches.reserve(n.branches.size());
    for (const auto &branch : n.branches) {
      branches.push_back(
          hir_if_branch{.condition = clone_as(*branch.condition, rename),
                        .body = clone_as(*branch.body, rename)});
    }
    return hir::make<hir_if>(span, type, std::move(branches),
                        clone_opt(n.else_body, rename));
  }
  case hir_node_kind::hir_match: {
    const auto &n = dynamic_cast<const hir_match &>(node);
    auto subject = clone_as(*n.subject, rename);
    const auto subject_symbol = rename(n.subject_symbol);
    auto arms = std::vector<hir_match_arm>{};
    arms.reserve(n.arms.size());
    for (const auto &arm : n.arms) {
      arms.push_back(hir_match_arm{.pattern = clone_as(*arm.pattern, rename),
                                   .guard = clone_opt(arm.guard, rename),
                                   .body = clone_as(*arm.body, rename)});
    }
    return hir::make<hir_match>(span, type, std::move(subject), subject_symbol,
                           std::move(arms));
  }
  case hir_node_kind::hir_lambda: {
    const auto &n = dynamic_cast<const hir_lambda &>(node);
    auto params = clone_params(n.params, rename);
    auto captures = std::optional<std::vector<hir_capture>>{};
    if (n.captures.has_value()) {
      captures.emplace();
      for (const auto &capture : *n.captures) {
        captures->push_back(
            hir_capture{.symbol = rename(capture.symbol), .mode = capture.mode});
      }
    }
    return hir::make<hir_lambda>(span, type, std::move(params), n.return_type,
                            clone_as(*n.body, rename), std::move(captures));
  }
  case hir_node_kind::hir_tuple_index: {
    const auto &n = dynamic_cast<const hir_tuple_index &>(node);
    return hir::make<hir_tuple_index>(span, type, clone_as(*n.object, rename),
                                 n.index);
  }
  case hir_node_kind::hir_variant_payload: {
    const auto &n = dynamic_cast<const hir_variant_payload &>(node);
    return hir::make<hir_variant_payload>(span, type, clone_as(*n.object, rename),
                                     n.variant_name, n.index);
  }
  case hir_node_kind::hir_variant_init: {
    const auto &n = dynamic_cast<const hir_variant_init &>(node);
    return hir::make<hir_variant_init>(span, type, n.variant_name,
                                  clone_all(n.args, rename));
  }
  case hir_node_kind::hir_stack_buffer: {
    const auto &n = dynamic_cast<const hir_stack_buffer &>(node);
    return hir::make<hir_stack_buffer>(span, type, n.byte_size, n.align_bytes);
  }
  case hir_node_kind::hir_container_data: {
    const auto &n = dynamic_cast<const hir_container_data &>(node);
    return hir::make<hir_container_data>(span, type, clone_as(*n.object, rename));
  }
  case hir_node_kind::hir_slice_from_raw_parts: {
    const auto &n = dynamic_cast<const hir_slice_from_raw_parts &>(node);
    return hir::make<hir_slice_from_raw_parts>(span, type,
                                          clone_as(*n.pointer, rename),
                                          clone_as(*n.len, rename));
  }
  case hir_node_kind::hir_container_len: {
    const auto &n = dynamic_cast<const hir_container_len &>(node);
    return hir::make<hir_container_len>(span, type, clone_as(*n.object, rename));
  }
  case hir_node_kind::hir_generator_next: {
    const auto &n = dynamic_cast<const hir_generator_next &>(node);
    return hir::make<hir_generator_next>(span, type, clone_as(*n.object, rename));
  }
  case hir_node_kind::hir_str_decode_scalar: {
    const auto &n = dynamic_cast<const hir_str_decode_scalar &>(node);
    return hir::make<hir_str_decode_scalar>(span, type, clone_as(*n.object, rename),
                                       clone_as(*n.byte_offset, rename));
  }
  case hir_node_kind::hir_str_scalar_width: {
    const auto &n = dynamic_cast<const hir_str_scalar_width &>(node);
    return hir::make<hir_str_scalar_width>(span, type, clone_as(*n.object, rename),
                                      clone_as(*n.byte_offset, rename));
  }
  case hir_node_kind::hir_cell_set: {
    const auto &n = dynamic_cast<const hir_cell_set &>(node);
    return hir::make<hir_cell_set>(span, type, clone_as(*n.cell, rename),
                              clone_as(*n.value, rename));
  }
  case hir_node_kind::hir_wildcard_pattern:
    return with_subject(hir::make<hir_wildcard_pattern>(span),
                        dynamic_cast<const hir_pattern &>(node));
  case hir_node_kind::hir_literal_pattern: {
    const auto &n = dynamic_cast<const hir_literal_pattern &>(node);
    return with_subject(hir::make<hir_literal_pattern>(span, n.lit_kind, n.value),
                        n);
  }
  case hir_node_kind::hir_or_pattern: {
    const auto &n = dynamic_cast<const hir_or_pattern &>(node);
    return with_subject(
        hir::make<hir_or_pattern>(span, clone_all(n.alternatives, rename)), n);
  }
  case hir_node_kind::hir_tuple_pattern: {
    const auto &n = dynamic_cast<const hir_tuple_pattern &>(node);
    return with_subject(
        hir::make<hir_tuple_pattern>(span, clone_all(n.elements, rename)), n);
  }
  case hir_node_kind::hir_array_pattern: {
    const auto &n = dynamic_cast<const hir_array_pattern &>(node);
    return with_subject(
        hir::make<hir_array_pattern>(span, clone_all(n.elements, rename)), n);
  }
  case hir_node_kind::hir_struct_pattern: {
    const auto &n = dynamic_cast<const hir_struct_pattern &>(node);
    auto fields = std::vector<hir_struct_pattern_field>{};
    fields.reserve(n.fields.size());
    for (const auto &field : n.fields) {
      fields.push_back(hir_struct_pattern_field{
          .name = field.name, .pattern = clone_as(*field.pattern, rename)});
    }
    return with_subject(hir::make<hir_struct_pattern>(span, std::move(fields)), n);
  }
  case hir_node_kind::hir_constructor_pattern: {
    const auto &n = dynamic_cast<const hir_constructor_pattern &>(node);
    return with_subject(hir::make<hir_constructor_pattern>(
                            span, n.variant_name, clone_all(n.args, rename)),
                        n);
  }
  case hir_node_kind::hir_range_pattern: {
    const auto &n = dynamic_cast<const hir_range_pattern &>(node);
    return with_subject(hir::make<hir_range_pattern>(span,
                                                clone_opt(n.start, rename),
                                                clone_opt(n.end, rename),
                                                n.inclusive),
                        n);
  }
  case hir_node_kind::hir_let: {
    const auto &n = dynamic_cast<const hir_let &>(node);
    auto initializer = clone_as(*n.initializer, rename);
    return hir::make<hir_let>(span, rename(n.symbol), n.name,
                         std::move(initializer), n.is_mut);
  }
  case hir_node_kind::hir_let_else: {
    const auto &n = dynamic_cast<const hir_let_else &>(node);
    auto initializer = clone_as(*n.initializer, rename);
    const auto subject_symbol = rename(n.subject_symbol);
    return hir::make<hir_let_else>(span, subject_symbol, std::move(initializer),
                              clone_as(*n.pattern, rename),
                              clone_as(*n.else_body, rename));
  }
  case hir_node_kind::hir_assign: {
    const auto &n = dynamic_cast<const hir_assign &>(node);
    return hir::make<hir_assign>(span, n.op, clone_as(*n.target, rename),
                            clone_as(*n.value, rename));
  }
  case hir_node_kind::hir_expr_stmt: {
    const auto &n = dynamic_cast<const hir_expr_stmt &>(node);
    return hir::make<hir_expr_stmt>(span, clone_as(*n.expr, rename));
  }
  case hir_node_kind::hir_return: {
    const auto &n = dynamic_cast<const hir_return &>(node);
    return hir::make<hir_return>(span, clone_opt(n.value, rename));
  }
  case hir_node_kind::hir_yield: {
    const auto &n = dynamic_cast<const hir_yield &>(node);
    return hir::make<hir_yield>(span, clone_as(*n.value, rename));
  }
  case hir_node_kind::hir_while: {
    const auto &n = dynamic_cast<const hir_while &>(node);
    return hir::make<hir_while>(span, clone_as(*n.condition, rename),
                           clone_as(*n.body, rename),
                           clone_opt(n.step, rename));
  }
  case hir_node_kind::hir_while_let: {
    const auto &n = dynamic_cast<const hir_while_let &>(node);
    auto subject = clone_as(*n.subject, rename);
    const auto subject_symbol = rename(n.subject_symbol);
    return hir::make<hir_while_let>(span, std::move(subject), subject_symbol,
                               clone_as(*n.pattern, rename),
                               clone_as(*n.body, rename));
  }
  case hir_node_kind::hir_break:
    return hir::make<hir_break>(span);
  case hir_node_kind::hir_continue:
    return hir::make<hir_continue>(span);
  case hir_node_kind::hir_contract_check: {
    const auto &n = dynamic_cast<const hir_contract_check &>(node);
    return hir::make<hir_contract_check>(span, clone_as(*n.condition, rename),
                                    n.kind, n.message);
  }
  case hir_node_kind::hir_function:
  case hir_node_kind::hir_module:
    break;
  }
  // A function or module never appears inside a body.
  return nullptr;
}

// --------------------------------------------------------------------------
//  Turning a body's `return`s into a block value
// --------------------------------------------------------------------------

[[nodiscard]] auto contains_return(hir_node &node) -> bool {
  auto found = false;
  walk(node, [&](hir_node &n) -> void {
    found = found || n.kind == hir_node_kind::hir_return;
  });
  return found;
}

/// The `if` a statement is, whether it sits in the block bare or wrapped in
/// a `hir_expr_stmt` — lowering produces both.
[[nodiscard]] auto as_if(hir_node &node) -> hir_if * {
  if (node.kind == hir_node_kind::hir_if) {
    return &dynamic_cast<hir_if &>(node);
  }
  if (node.kind == hir_node_kind::hir_expr_stmt) {
    auto &expr = *dynamic_cast<hir_expr_stmt &>(node).expr;
    if (expr.kind == hir_node_kind::hir_if) {
      return &dynamic_cast<hir_if &>(expr);
    }
  }
  return nullptr;
}

[[nodiscard]] auto always_returns(const hir_block &block) -> bool;

/// Whether control can never fall out the bottom of `node` — every path
/// through it ends in a `return`.
[[nodiscard]] auto always_returns(hir_node &node) -> bool {
  if (node.kind == hir_node_kind::hir_return) {
    return true;
  }
  if (node.kind == hir_node_kind::hir_block) {
    return always_returns(dynamic_cast<const hir_block &>(node));
  }
  const auto *iff = as_if(node);
  if (iff == nullptr || iff->else_body == nullptr ||
      !always_returns(*iff->else_body)) {
    return false;
  }
  return std::ranges::all_of(iff->branches, [](const hir_if_branch &branch) -> bool {
    return always_returns(*branch.body);
  });
}

auto always_returns(const hir_block &block) -> bool {
  return !block.stmts.empty() && always_returns(*block.stmts.back());
}

[[nodiscard]] auto unit_value(source_span span, type_id unit)
    -> ptr<hir_expr> {
  return hir::make<hir_literal>(span, unit, token_kind::kw_unit, "");
}

/// Rewrites a function body's statements so that, run as a *block*, they
/// produce what the function would have returned — `return x` becomes the
/// tail value `x`. `nullopt` when that isn't possible without duplicating
/// code, in which case the function is simply not inlined.
///
/// The one non-trivial shape is an `if` whose every branch returns: the
/// statements after it can only run when no branch was taken, so they move
/// into its `else`, and the whole `if` becomes the block's value. That is
/// the guard clause — `if i >= self.len: panic(...)` needs nothing, but
/// `if i >= self.len: return @none` followed by `return @some(...)` becomes
/// `if i >= self.len { @none } else { @some(...) }`.
[[nodiscard]] auto to_block_value(ptr_vec<hir_node> stmts, type_id result,
                                  const type_table &types)
    -> std::optional<ptr_vec<hir_node>> {
  auto out = ptr_vec<hir_node>{};
  for (size_t i = 0; i < stmts.size(); ++i) {
    auto &stmt = stmts[i];
    if (stmt->kind == hir_node_kind::hir_return) {
      auto &ret = dynamic_cast<hir_return &>(*stmt);
      auto value = ret.value != nullptr ? std::move(ret.value)
                                        : unit_value(ret.span, result);
      out.push_back(hir::make<hir_expr_stmt>(ret.span, std::move(value)));
      // Anything after an unconditional `return` is unreachable.
      return out;
    }
    if (!contains_return(*stmt)) {
      out.push_back(std::move(stmt));
      continue;
    }
    auto *iff = as_if(*stmt);
    if (iff == nullptr ||
        !std::ranges::all_of(iff->branches, [](const hir_if_branch &branch) -> bool {
          return always_returns(*branch.body);
        })) {
      return std::nullopt;
    }
    auto else_stmts = ptr_vec<hir_node>{};
    auto else_span = iff->span;
    auto rest_is_dead = false;
    if (iff->else_body != nullptr) {
      else_span = iff->else_body->span;
      rest_is_dead = always_returns(*iff->else_body);
      else_stmts = std::move(iff->else_body->stmts);
    }
    if (!rest_is_dead) {
      for (size_t j = i + 1; j < stmts.size(); ++j) {
        else_stmts.push_back(std::move(stmts[j]));
      }
    }
    auto branches = std::vector<hir_if_branch>{};
    branches.reserve(iff->branches.size());
    for (auto &branch : iff->branches) {
      auto body =
          to_block_value(std::move(branch.body->stmts), result, types);
      if (!body.has_value()) {
        return std::nullopt;
      }
      branches.push_back(hir_if_branch{
          .condition = std::move(branch.condition),
          .body = hir::make<hir_block>(branch.body->span, result, std::move(*body))});
    }
    auto else_body = to_block_value(std::move(else_stmts), result, types);
    if (!else_body.has_value()) {
      return std::nullopt;
    }
    const auto span = iff->span;
    out.push_back(hir::make<hir_expr_stmt>(
        span, hir::make<hir_if>(span, result, std::move(branches),
                           hir::make<hir_block>(else_span, result,
                                           std::move(*else_body)))));
    return out;
  }
  // Control falls out the bottom: the tail expression, if any, is the value.
  const auto *last =
      out.empty() || out.back()->kind != hir_node_kind::hir_expr_stmt
          ? nullptr
          : dynamic_cast<const hir_expr_stmt *>(out.back().get());
  const auto tail_matches = last != nullptr && last->expr->type == result;
  if (types.is_unit(result)) {
    if (!tail_matches) {
      const auto span = out.empty() ? source_span{} : out.back()->span;
      out.push_back(hir::make<hir_expr_stmt>(span, unit_value(span, result)));
    }
    return out;
  }
  if (!tail_matches) {
    return std::nullopt;
  }
  return out;
}

// --------------------------------------------------------------------------
//  Callees
// --------------------------------------------------------------------------

/// An inlinable function's body, prepared once: `return`s already turned
/// into a block value, bare-name references to other functions already
/// qualified with this function's module, tail-call marks cleared. Every
/// call site copies it with fresh symbols.
struct inline_template {
  const hir_function *fn = nullptr;
  std::vector<hir_param> params;
  ptr_vec<hir_node> stmts;
  /// Every symbol the body binds, parameters included — what, in a copy,
  /// becomes a local of the caller.
  std::unordered_set<symbol_id> bound;
};

[[nodiscard]] auto disqualifying_node(const hir_node &node) -> bool {
  switch (node.kind) {
  case hir_node_kind::hir_lambda:
  case hir_node_kind::hir_yield:
  case hir_node_kind::hir_contract_check:
  case hir_node_kind::hir_stack_buffer:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] auto build_template(hir_function &fn,
                                  const std::string &module_name,
                                  const type_table &types)
    -> std::optional<inline_template> {
  if (fn.is_generator || fn.body == nullptr ||
      count_nodes(*fn.body) > k_max_callee_nodes) {
    return std::nullopt;
  }
  auto params = std::unordered_set<symbol_id>{};
  for (const auto &param : fn.params) {
    params.insert(param.symbol);
  }
  auto eligible = true;
  walk(*fn.body, [&](hir_node &n) -> void {
    if (disqualifying_node(n)) {
      eligible = false;
    } else if (n.kind == hir_node_kind::hir_assign) {
      // A parameter the callee reassigns may be passed by reference
      // (`mut self` on a scalar); a copy bound with `let` would silently
      // stop writing back.
      const auto &target = *dynamic_cast<hir_assign &>(n).target;
      if (target.kind == hir_node_kind::hir_local_ref &&
          params.contains(dynamic_cast<const hir_local_ref &>(target).symbol)) {
        eligible = false;
      }
    }
  });
  if (!eligible) {
    return std::nullopt;
  }

  auto bound = params;
  collect_binders(*fn.body, bound);

  auto exact = symbol_renamer{};
  auto body = clone_all(fn.body->stmts, exact);
  for (auto &stmt : body) {
    walk(*stmt, [&](hir_node &n) -> void {
      if (n.kind == hir_node_kind::hir_local_ref) {
        auto &ref = dynamic_cast<hir_local_ref &>(n);
        if (!bound.contains(ref.symbol) && !ref.owner_module.has_value()) {
          ref.owner_module = module_name;
        }
      } else if (n.kind == hir_node_kind::hir_call) {
        dynamic_cast<hir_call &>(n).is_tail_call = false;
      }
    });
  }
  auto stmts = to_block_value(std::move(body), fn.return_type, types);
  if (!stmts.has_value()) {
    return std::nullopt;
  }
  return inline_template{.fn = &fn,
                         .params = fn.params,
                         .stmts = std::move(*stmts),
                         .bound = std::move(bound)};
}

/// Whether binding an argument of type `arg` with `let` reproduces passing
/// it to a parameter of type `param` on both backends: the same type, or the
/// same heap-boxed aggregate seen through a reference or not — a pointer
/// either way.
[[nodiscard]] auto same_representation(const type_table &types, type_id arg,
                                       type_id param) -> bool {
  if (arg == param) {
    return true;
  }
  const auto strip = [&](type_id id) -> type_id {
    while (types.entry(id).kind == type_kind::ref_kind) {
      id = types.entry(id).result;
    }
    return id;
  };
  const auto base = strip(arg);
  if (base != strip(param)) {
    return false;
  }
  switch (types.entry(base).kind) {
  case type_kind::struct_kind:
  case type_kind::sum_kind:
  case type_kind::tuple_kind:
  case type_kind::array_kind:
    return true;
  default:
    return false;
  }
}

// --------------------------------------------------------------------------
//  Callers
// --------------------------------------------------------------------------

using template_table =
    std::map<std::pair<std::string, std::string>, inline_template>;

struct caller_state {
  const std::string *module_name = nullptr;
  symbol_id next_symbol = 0;
  /// Symbols bound as locals anywhere in the caller, copies included: a
  /// call through one of these is a closure call, never inlined.
  std::unordered_set<symbol_id> bound;
  size_t nodes = 0;
  std::vector<const hir_function *> expanding;
  size_t inlined = 0;
};

class inliner {
public:
  inliner(const template_table &templates, const type_table &types)
      : templates_(templates), types_(types) {}

  auto rewrite(hir_node &node, caller_state &state, size_t depth) -> void {
    if (node.kind == hir_node_kind::hir_lambda) {
      return;
    }
    for_each_child(node, [&](auto &slot) -> void {
      rewrite(*slot, state, depth);
      if constexpr (std::is_same_v<std::remove_cvref_t<decltype(slot)>,
                                   ptr<hir_expr>>) {
        if (auto expansion = expand(*slot, state, depth);
            expansion != nullptr) {
          slot = std::move(expansion);
        }
      }
    });
  }

private:
  [[nodiscard]] auto find_template(const hir_expr &expr,
                                   const caller_state &state) const
      -> const inline_template * {
    if (expr.kind != hir_node_kind::hir_call) {
      return nullptr;
    }
    const auto &call = dynamic_cast<const hir_call &>(expr);
    if (call.callee->kind != hir_node_kind::hir_local_ref) {
      return nullptr;
    }
    const auto &ref = dynamic_cast<const hir_local_ref &>(*call.callee);
    // A local holding a closure, or an intrinsic — both backends check for
    // the intrinsic name first, so a user function can't shadow one.
    if (state.bound.contains(ref.symbol) ||
        kira::intrinsic_index_of(ref.name).has_value()) {
      return nullptr;
    }
    const auto found = templates_.find(
        {ref.owner_module.value_or(*state.module_name), ref.name});
    if (found == templates_.end()) {
      return nullptr;
    }
    const auto &candidate = found->second;
    if (call.args.size() != candidate.params.size()) {
      return nullptr;
    }
    for (size_t i = 0; i < call.args.size(); ++i) {
      if (!same_representation(types_, call.args[i]->type,
                               candidate.params[i].type)) {
        return nullptr;
      }
    }
    return &candidate;
  }

  /// The block replacing `expr`, or null to leave it a call.
  [[nodiscard]] auto expand(hir_expr &expr, caller_state &state, size_t depth)
      -> ptr<hir_expr> {
    const auto *candidate = find_template(expr, state);
    if (candidate == nullptr || depth >= k_max_depth ||
        state.nodes > k_max_caller_nodes ||
        std::ranges::contains(state.expanding, candidate->fn)) {
      return nullptr;
    }
    auto &call = dynamic_cast<hir_call &>(expr);

    auto rename = symbol_renamer{.next = &state.next_symbol, .renamed = {}};
    auto stmts = ptr_vec<hir_node>{};
    stmts.reserve(candidate->params.size() + candidate->stmts.size());
    for (size_t i = 0; i < candidate->params.size(); ++i) {
      const auto &param = candidate->params[i];
      const auto span = call.args[i]->span;
      stmts.push_back(hir::make<hir_let>(span, rename(param.symbol), param.name,
                                    std::move(call.args[i])));
    }
    const auto body_start = stmts.size();
    for (const auto &stmt : candidate->stmts) {
      stmts.push_back(clone_node(*stmt, rename));
    }
    for (const auto symbol : candidate->bound) {
      if (const auto found = rename.renamed.find(symbol);
          found != rename.renamed.end()) {
        state.bound.insert(found->second);
      }
    }

    state.expanding.push_back(candidate->fn);
    for (size_t i = body_start; i < stmts.size(); ++i) {
      rewrite(*stmts[i], state, depth + 1);
    }
    state.expanding.pop_back();

    // A statement-only call (`xs[i] = v`'s `set_at`) is lowered without a
    // result type of its own; the block still needs one.
    const auto type =
        call.type == k_unknown_type ? candidate->fn->return_type : call.type;
    auto block = hir::make<hir_block>(call.span, type, std::move(stmts));
    state.nodes += count_nodes(*block);
    ++state.inlined;
    return block;
  }

  const template_table &templates_;
  const type_table &types_;
};

} // namespace

auto inline_small_calls(ptr_vec<hir_module> &modules, const type_table &types)
    -> inline_stats {
  // Every template is built before any caller changes, so what gets copied
  // is always a function as lowered, whatever order callers are visited in.
  auto templates = template_table{};
  for (auto &module : modules) {
    for (auto &fn : module->functions) {
      if (auto built = build_template(*fn, module->module_name, types);
          built.has_value()) {
        templates.emplace(std::pair{module->module_name, fn->name},
                          std::move(*built));
      }
    }
  }

  auto stats = inline_stats{};
  auto pass = inliner(templates, types);
  for (auto &module : modules) {
    for (auto &fn : module->functions) {
      if (fn->is_generator || fn->body == nullptr) {
        continue;
      }
      auto state = caller_state{.module_name = &module->module_name,
                                .next_symbol = max_symbol(*fn) + 1,
                                .bound = {},
                                .nodes = count_nodes(*fn->body),
                                .expanding = {fn.get()},
                                .inlined = 0};
      for (const auto &param : fn->params) {
        state.bound.insert(param.symbol);
      }
      collect_binders(*fn->body, state.bound);
      pass.rewrite(*fn->body, state, 0);
      if (state.inlined > 0) {
        mark_tail_calls(*fn);
        stats.call_sites += state.inlined;
      }
    }
  }
  return stats;
}

} // namespace kira::hir
