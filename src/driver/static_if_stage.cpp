#include "static_if_stage.h"

#include <utility>

#include "src/comptime/eval.h"
#include "src/comptime/value.h"

namespace cinder::driver {

namespace {

/// Whether any item in `items` is a `use` declaration — the marker that makes
/// a `static if` block import-gating (and therefore something the module graph
/// must see the selection of before it is built).
[[nodiscard]] auto
branch_contains_use(const std::vector<ast::ptr<ast::node>> &items) -> bool {
  for (const auto &item : items) {
    if (item != nullptr && item->kind == ast::node_kind::use_decl) {
      return true;
    }
  }
  return false;
}

/// Recursively folds every module-scope `static if` in `items` whose
/// condition is early-evaluable (a compile-time literal, resolvable with no
/// name resolution), returning the rewritten item list. This runs *before*
/// scope/symbol building, so it is also what makes a `static if` selecting
/// between top-level item names (e.g. two conflicting `type word` decls)
/// register the winning branch's names at all — module-scope symbol
/// registration never looks inside an unfolded `static if` node. A foldable
/// block's taken branch replaces the block (itself folded, so a nested
/// `static if` is handled); an unfoldable one is left in place for
/// check-time branch selection (`checker::resolve_static_if_branch`), unless
/// it gates a `use`, in which case it is kept with a restriction diagnostic
/// since the module graph has no later chance to see the selection. Consumes
/// `items`.
[[nodiscard]] auto fold_items(std::vector<ast::ptr<ast::node>> items,
                              file_id_type file_id, diagnostic_bag &diagnostics)
    -> std::vector<ast::ptr<ast::node>> {
  auto out = std::vector<ast::ptr<ast::node>>{};
  out.reserve(items.size());
  for (auto &item : items) {
    auto *stat = item != nullptr ? dynamic_cast<ast::static_decl *>(item.get())
                                 : nullptr;
    if (stat == nullptr ||
        stat->decl_kind != ast::static_decl_kind::conditional_compilation) {
      if (auto *sub_module =
              item != nullptr ? dynamic_cast<ast::sub_module_decl *>(item.get())
                              : nullptr;
          sub_module != nullptr && !sub_module->is_functor() &&
          !sub_module->items.empty()) {
        sub_module->items =
            fold_items(std::move(sub_module->items), file_id, diagnostics);
      }
      out.push_back(std::move(item));
      continue;
    }

    const bool import_gating = branch_contains_use(stat->if_body) ||
                               branch_contains_use(stat->else_body);

    // Evaluate the condition with a standalone, resolution-free evaluator —
    // literals only, no globals registered — into a scratch bag so a failure
    // yields our own targeted diagnostic rather than the evaluator's generic
    // "not a compile-time constant" one.
    auto scratch = diagnostic_bag{};
    auto evaluator = comptime::evaluator(scratch, file_id);
    const auto condition =
        (stat->if_condition != nullptr && !stat->if_condition->has_error)
            ? evaluator.evaluate(*stat->if_condition)
            : comptime::value::make_error();

    if (condition.kind != comptime::value_kind::boolean) {
      if (import_gating) {
        auto diag = diagnostic(
            diagnostic_level::error,
            "a `static if` that gates a `use` must have an early-evaluable "
            "condition",
            file_id);
        diag.with_label(stat->span, "this condition selects an import");
        diag.with_help(
            "Because it decides which modules are imported, the module graph "
            "is built from it before any names are resolved. Restrict the "
            "condition to compile-time literals (e.g. `static if true:`); a "
            "condition that needs name resolution cannot be used to gate a "
            "`use`.");
        diagnostics.emit(diag);
      }
      // Not import-gating: leave the node for check-time branch selection,
      // which has full name/type resolution available.
      out.push_back(std::move(item));
      continue;
    }

    auto &taken = condition.is_true() ? stat->if_body : stat->else_body;
    auto folded = fold_items(std::move(taken), file_id, diagnostics);
    for (auto &node : folded) {
      out.push_back(std::move(node));
    }
    // The `static if` node itself is dropped (not pushed) — its selected
    // branch now lives inline among the file's items.
  }
  return out;
}

} // namespace

auto fold_static_if_imports(std::vector<parsed_input> &inputs,
                            diagnostic_bag &diagnostics) -> void {
  for (auto &input : inputs) {
    if (input.ast_file == nullptr) {
      continue;
    }
    input.ast_file->items = fold_items(std::move(input.ast_file->items),
                                       input.file_id, diagnostics);
  }
}

} // namespace cinder::driver
