#include "src/hir/captures.h"

#include <unordered_set>

namespace kira::hir {

namespace {

/// Walking state: `bound` accumulates every symbol declared so far in
/// program order (parameters, `hir_let`/`hir_let_else`/`hir_while_let`/
/// `hir_match` subject bindings); `free`/`seen_free` accumulate every
/// referenced symbol not yet in `bound`, in first-reference order with no
/// duplicates. Relies on this language's "declared before referenced"
/// invariant (Decision-guaranteed by the checker) so one forward walk,
/// growing `bound` and never shrinking it, is sufficient — a symbol bound
/// in one block of the lambda can never be mistaken for free just because
/// a sibling block hasn't been walked yet, since nothing referencing it
/// could appear before its own `hir_let`.
struct walker {
  std::unordered_set<symbol_id> bound;
  std::unordered_set<symbol_id> seen_free;
  std::vector<symbol_id> free;

  auto bind(symbol_id sym) -> void { bound.insert(sym); }

  auto reference(symbol_id sym) -> void {
    if (bound.contains(sym)) {
      return;
    }
    if (seen_free.insert(sym).second) {
      free.push_back(sym);
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
      // A nested lambda's own parameters are bound only within its body,
      // not visible to sibling code in the enclosing lambda — but any free
      // variable it references that isn't one of its own parameters still
      // needs to be captured by (or already available to) *this* lambda,
      // so its body is walked with the same `bound`/`free` accumulators
      // rather than starting a fresh walk.
      const auto &node = dynamic_cast<const hir_lambda &>(expr);
      const auto outer_bound = bound;
      for (const auto &param : node.params) {
        bind(param.symbol);
      }
      for (const auto &stmt : node.body->stmts) {
        walk_stmt(*stmt);
      }
      bound = outer_bound;
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

  /// Only `hir_range_pattern` holds any `hir_expr` (its bounds); every
  /// other pattern kind is purely structural (per `hir_pattern`'s doc
  /// comment, a pattern never itself binds a name — see `hir_match_arm`).
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
    case hir_node_kind::hir_yield:
      // A lambda body shouldn't contain `yield` (it's its own call frame,
      // not part of an enclosing generator's suspension protocol — the
      // bytecode compiler rejects this case explicitly), but walk its
      // value defensively rather than falling into the `hir_expr`
      // downcast below, which `hir_yield` (an `hir_stmt`) would fail.
      walk_expr(*dynamic_cast<const hir_yield &>(node).value);
      return;
    case hir_node_kind::hir_while: {
      const auto &node2 = dynamic_cast<const hir_while &>(node);
      walk_expr(*node2.condition);
      for (const auto &stmt : node2.body->stmts) {
        walk_stmt(*stmt);
      }
      // The step (a desugared `for`'s index increment) is ordinary code
      // that reads and writes a local, so this walk must see it too.
      if (node2.step != nullptr) {
        for (const auto &stmt : node2.step->stmts) {
          walk_stmt(*stmt);
        }
      }
      return;
    }
    case hir_node_kind::hir_while_let: {
      const auto &node2 = dynamic_cast<const hir_while_let &>(node);
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
    case hir_node_kind::hir_break:
    case hir_node_kind::hir_continue:
      // Pure control transfer: no operand, so no name to capture. Listed
      // explicitly because the downcast below is unchecked, and these are
      // statements rather than expressions.
      return;
    default:
      // Every expression kind can also appear directly as a "statement"
      // node in this walk (e.g. a match compiled in statement position is
      // still a `hir_match`, an `hir_expr`) — fall back to `walk_expr` for
      // anything not already handled above as a dedicated statement kind.
      walk_expr(dynamic_cast<const hir_expr &>(node));
      return;
    }
  }
};

} // namespace

auto free_variables(const hir_lambda &lambda) -> std::vector<symbol_id> {
  auto w = walker{};
  for (const auto &param : lambda.params) {
    w.bind(param.symbol);
  }
  for (const auto &stmt : lambda.body->stmts) {
    w.walk_stmt(*stmt);
  }
  return std::move(w.free);
}

} // namespace kira::hir
