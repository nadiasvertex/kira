#include "src/hir/drop_schedule.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "src/semantic/ownership_cfg.h"
#include "src/semantic/ownership_check.h"

namespace cinder::hir {
namespace {

/// The field names from a `field_path` local's root down to it.
auto field_path_of(const semantic::ownership::function_cfg &cfg,
                   semantic::ownership::local_id part)
    -> std::vector<std::string> {
  auto path = std::vector<std::string>{};
  for (auto at = part; cfg.locals[at].parent != semantic::ownership::k_no_local;
       at = cfg.locals[at].parent) {
    const auto &name = cfg.locals[at].name;
    const auto &parent = cfg.locals[cfg.locals[at].parent].name;
    path.push_back(name.substr(parent.size() + 1));
  }
  std::ranges::reverse(path);
  return path;
}

} // namespace

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
        for (const auto &owned : group) {
          const auto &info = cfg.locals[owned.local];
          const auto depth = seen[info.name]++;
          if (checked.drop_plans.contains(info.type)) {
            auto moved_paths = std::vector<std::vector<std::string>>{};
            for (const auto part : owned.moved_parts) {
              moved_paths.push_back(field_path_of(cfg, part));
            }
            drops.push_back(pending_drop{.name = info.name,
                                         .type = info.type,
                                         .shadow_depth = depth,
                                         .moved_paths = std::move(moved_paths)});
          }
        }
      }
    }
  }
  return schedule;
}

} // namespace cinder::hir
