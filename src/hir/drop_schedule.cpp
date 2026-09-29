#include "src/hir/drop_schedule.h"

#include <map>

#include "src/semantic/ownership_cfg.h"
#include "src/semantic/ownership_check.h"

namespace cinder::hir {

auto compute_drop_schedule(const ast::func_decl &decl,
                           const semantic::checked_types &checked)
    -> drop_schedule {
  auto schedule = drop_schedule{};
  for (const auto &cfg :
       semantic::ownership::build_function_cfgs(decl, checked)) {
    for (const auto &exit : semantic::ownership::owned_at_scope_exits(cfg)) {
      auto &drops = schedule.exits[exit.key];
      // Groups run innermost scope first, each in reverse declaration order,
      // so the first time a name shows up it is the binding a lookup finds
      // and each later one is one shadow level deeper. Every whole binding
      // counts, droppable or not, because `lowerer` counts them all.
      auto seen = std::map<std::string_view, std::size_t>{};
      for (const auto &group : exit.groups) {
        for (const auto local : group) {
          const auto &info = cfg.locals[local];
          const auto depth = seen[info.name]++;
          if (checked.drop_plans.contains(info.type)) {
            drops.push_back(pending_drop{
                .name = info.name, .type = info.type, .shadow_depth = depth});
          }
        }
      }
    }
  }
  return schedule;
}

} // namespace cinder::hir
