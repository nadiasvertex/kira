#include "src/hir/drop_schedule.h"

#include <set>

#include "src/semantic/ownership_cfg.h"
#include "src/semantic/ownership_check.h"

namespace cinder::hir {

auto compute_drop_schedule(const ast::func_decl &decl,
                           const semantic::checked_types &checked)
    -> drop_schedule {
  auto schedule = drop_schedule{};
  for (const auto &cfg : semantic::ownership::build_function_cfgs(decl, checked)) {
    for (const auto &exit : semantic::ownership::owned_at_scope_exits(cfg)) {
      auto &drops = schedule.exits[exit.key];
      // `lowerer` finds each drop's local by name, which reaches only the
      // innermost binding of a shadowed name — so only that one is dropped,
      // and a shadowed one leaks rather than the innermost dropping twice.
      auto named = std::set<std::string_view>{};
      for (const auto &group : exit.groups) {
        for (const auto local : group) {
          const auto &info = cfg.locals[local];
          if (checked.drop_plans.contains(info.type) &&
              named.insert(info.name).second) {
            drops.push_back(pending_drop{.name = info.name, .type = info.type});
          }
        }
      }
    }
  }
  return schedule;
}

} // namespace cinder::hir
