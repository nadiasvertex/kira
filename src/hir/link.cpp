#include "src/hir/link.h"

#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace kira::hir {

namespace {

/// Collects every `owner_module` referenced by a `hir_local_ref` reachable
/// from one function body — mirrors `captures.cpp`'s `walker` shape (same
/// node kinds, same recursion), but has no notion of bound/free: every
/// reference counts, since a module a function's body mentions at all is a
/// module that function needs available at compile time.
struct collector {
  std::vector<std::string> modules;
  std::unordered_set<std::string> seen;

  auto note(const std::optional<std::string> &owner_module) -> void {
    if (owner_module.has_value() && seen.insert(*owner_module).second) {
      modules.push_back(*owner_module);
    }
  }

  auto walk_expr(const hir_expr &expr) -> void {
    switch (expr.kind) {
    case hir_node_kind::hir_literal:
      return;
    case hir_node_kind::hir_local_ref:
      note(dynamic_cast<const hir_local_ref &>(expr).owner_module);
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
      const auto &node = dynamic_cast<const hir_lambda &>(expr);
      for (const auto &stmt : node.body->stmts) {
        walk_stmt(*stmt);
      }
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
    case hir_node_kind::hir_generator_next:
      walk_expr(*dynamic_cast<const hir_generator_next &>(expr).object);
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
    case hir_node_kind::hir_let:
      walk_expr(*dynamic_cast<const hir_let &>(node).initializer);
      return;
    case hir_node_kind::hir_let_else: {
      const auto &node2 = dynamic_cast<const hir_let_else &>(node);
      walk_expr(*node2.initializer);
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
      // Pure control transfer: no operand, so nothing here can reference
      // another module. Listed explicitly because the downcast below is
      // unchecked, and these are statements rather than expressions.
      return;
    default:
      walk_expr(dynamic_cast<const hir_expr &>(node));
      return;
    }
  }
};

} // namespace

auto find_reachable_modules(const hir_module &entry,
                            const ptr_vec<hir_module> &all_modules)
    -> std::vector<const hir_module *> {
  auto by_name = std::unordered_map<std::string, const hir_module *>{};
  for (const auto &module : all_modules) {
    if (module != nullptr) {
      by_name.emplace(module->module_name, module.get());
    }
  }

  auto result = std::vector<const hir_module *>{&entry};
  auto visited = std::unordered_set<std::string>{entry.module_name};
  auto queue = std::deque<const hir_module *>{&entry};

  while (!queue.empty()) {
    const auto *current = queue.front();
    queue.pop_front();

    auto found = collector{};
    for (const auto &fn : current->functions) {
      if (fn != nullptr && fn->body != nullptr) {
        for (const auto &stmt : fn->body->stmts) {
          found.walk_stmt(*stmt);
        }
      }
    }

    for (const auto &module_name : found.modules) {
      if (!visited.insert(module_name).second) {
        continue;
      }
      const auto it = by_name.find(module_name);
      if (it == by_name.end()) {
        continue; // referenced module wasn't part of this compile session
      }
      result.push_back(it->second);
      queue.push_back(it->second);
    }
  }

  return result;
}

} // namespace kira::hir
