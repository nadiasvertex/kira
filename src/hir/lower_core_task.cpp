#include <utility>

#include "src/hir/lower_core_lowerer.h"

/// Generators and tasks: suspension points and what a cancelled coroutine
/// runs.
namespace cinder::hir {

auto core_lowerer::cancel_check(const void *key, source_span span,
                                ptr_vec<hir_expr> awaited,
                                ptr_vec<hir_node> drops)
    -> core_result<ptr<hir_node>> {
  auto exit = ptr_vec<hir_node>{};
  for (auto &task : awaited) {
    exit.push_back(hir::make<hir_expr_stmt>(
        span, hir::make<hir_coroutine_cancel>(span, k_unknown_type,
                                              std::move(task))));
  }
  for (auto &drop : drops) {
    exit.push_back(std::move(drop));
  }
  if (auto dropped = drops_.emit_jump_drops(key, jump_exit::function, span,
                                            exit);
      !dropped.has_value()) {
    return refuse_drop(dropped.error());
  }
  exit.push_back(hir::make<hir_return>(span, nullptr));
  auto branches = std::vector<hir_if_branch>{};
  branches.push_back(hir_if_branch{
      .condition = hir::make<hir_coroutine_cancelled>(
          span, checked_.types.bool_type()),
      .body = hir::make<hir_block>(span, k_unknown_type, std::move(exit))});
  return ptr<hir_node>(hir::make<hir_expr_stmt>(
      span, hir::make<hir_if>(span, k_unknown_type, std::move(branches),
                              nullptr)));
}

} // namespace cinder::hir
