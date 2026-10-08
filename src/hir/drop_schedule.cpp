#include "src/hir/drop_schedule.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "src/semantic/ownership_cfg.h"
#include "src/semantic/ownership_check.h"

namespace cinder::hir {
namespace {

using semantic::ownership::function_cfg;
using semantic::ownership::k_no_local;
using semantic::ownership::local_id;
using semantic::ownership::move_state;

/// The field names from a `field_path` local's root down to it.
auto field_path_of(const function_cfg &cfg, local_id part)
    -> std::vector<std::string> {
  auto path = std::vector<std::string>{};
  for (auto at = part; cfg.locals[at].parent != k_no_local;
       at = cfg.locals[at].parent) {
    const auto &name = cfg.locals[at].name;
    const auto &parent = cfg.locals[cfg.locals[at].parent].name;
    path.push_back(name.substr(parent.size() + 1));
  }
  std::ranges::reverse(path);
  return path;
}

/// A move of the whole of `a->local`. A projected move takes only a part,
/// which is its own `field_path` local's move.
auto moves_whole(const semantic::ownership::access_event *a) -> bool {
  return a != nullptr && a->kind == semantic::ownership::access_kind::move &&
         !a->projected;
}

auto root_of(const function_cfg &cfg, local_id local) -> local_id {
  while (cfg.locals[local].parent != k_no_local) {
    local = cfg.locals[local].parent;
  }
  return local;
}

/// The drop flags of one body's locals: which locals need one, and the
/// number each was given.
class flag_plan {
public:
  flag_plan(const function_cfg &cfg,
            const semantic::ownership::drop_facts &facts)
      : cfg_(cfg) {
    // Every move of a local must be an expression lowering can clear the
    // flag at, and its binding a declaration lowering can set it at.
    auto unclearable = std::set<local_id>{};
    for (const auto &block : cfg.blocks) {
      for (const auto &e : block.events) {
        const auto *a = std::get_if<semantic::ownership::access_event>(&e);
        if (moves_whole(a) && a->node == nullptr) {
          unclearable.insert(a->local);
        }
      }
    }
    const auto want = [&](local_id local) -> void {
      if (!unclearable.contains(local) &&
          cfg.locals[root_of(cfg, local)].node != nullptr) {
        wanted_.insert(local);
      }
    };
    for (const auto &exit : facts.exits) {
      for (const auto &group : exit.groups) {
        for (const auto &owned : group) {
          if (owned.state == move_state::maybe_moved) {
            want(owned.local);
          }
          for (const auto &part : owned.moved_parts) {
            if (part.state == move_state::maybe_moved) {
              want(part.local);
            }
          }
        }
      }
    }
    for (const auto &assign : facts.assignments) {
      for (auto at = assign.local; at != k_no_local;
           at = cfg.locals[at].parent) {
        want(at);
      }
      for (const auto &part : assign.moved_parts) {
        if (part.state == move_state::maybe_moved) {
          want(part.local);
        }
      }
    }
  }

  /// Numbers the flags this body needs from `next` on, and records where
  /// lowering sets and clears them.
  auto assign(drop_schedule &schedule) -> void {
    for (const auto local : wanted_) {
      if (needed(local)) {
        flags_[local] = schedule.flag_count++;
      }
    }
    for (const auto &[local, flag] : flags_) {
      schedule.binding_flags[cfg_.locals[root_of(cfg_, local)].node].push_back(
          flag);
    }
    for (const auto &block : cfg_.blocks) {
      for (const auto &e : block.events) {
        const auto *a = std::get_if<semantic::ownership::access_event>(&e);
        if (!moves_whole(a)) {
          continue;
        }
        if (const auto found = flags_.find(a->local); found != flags_.end()) {
          schedule.move_clears[a->node].push_back(found->second);
        }
      }
    }
  }

  /// Marks `local` as needing a flag: something drops it under one.
  auto require(local_id local) -> void { required_.insert(local); }

  [[nodiscard]] auto can_flag(local_id local) const -> bool {
    return wanted_.contains(local);
  }

  [[nodiscard]] auto flag(local_id local) const -> std::optional<std::size_t> {
    const auto found = flags_.find(local);
    return found != flags_.end() ? std::optional(found->second) : std::nullopt;
  }

  /// The flags of `local` and every field path below it.
  [[nodiscard]] auto flags_under(local_id local) const
      -> std::vector<std::size_t> {
    auto out = std::vector<std::size_t>{};
    if (const auto own = flag(local)) {
      out.push_back(*own);
    }
    for (const auto child : cfg_.locals[local].children) {
      const auto below = flags_under(child);
      out.insert(out.end(), below.begin(), below.end());
    }
    return out;
  }

private:
  [[nodiscard]] auto needed(local_id local) const -> bool {
    return required_.contains(local);
  }

  const function_cfg &cfg_;
  std::set<local_id> wanted_;
  std::set<local_id> required_;
  std::map<local_id, std::size_t> flags_;
};

/// `parts` as moved paths: one moved on every path is skipped; one moved
/// on some paths is dropped under its flag, or skipped when it cannot have
/// one (a leak on the paths that did not move it).
auto moved_paths_of(const function_cfg &cfg, const flag_plan &plan,
                    const std::vector<semantic::ownership::moved_part> &parts)
    -> std::vector<moved_path> {
  auto out = std::vector<moved_path>{};
  for (const auto &part : parts) {
    out.push_back(moved_path{.path = field_path_of(cfg, part.local),
                             .flag = part.state == move_state::maybe_moved
                                         ? plan.flag(part.local)
                                         : std::nullopt});
  }
  return out;
}

auto require_parts(flag_plan &plan,
                   const std::vector<semantic::ownership::moved_part> &parts)
    -> void {
  for (const auto &part : parts) {
    if (part.state == move_state::maybe_moved && plan.can_flag(part.local)) {
      plan.require(part.local);
    }
  }
}

} // namespace

auto compute_drop_schedule(const ast::func_decl &decl,
                           const semantic::checked_types &checked)
    -> drop_schedule {
  auto schedule = drop_schedule{};
  for (const auto &cfg :
       semantic::ownership::build_function_cfgs(decl, checked)) {
    const auto facts = semantic::ownership::drop_facts_of(cfg);
    auto plan = flag_plan(cfg, facts);

    // First pass: which flags some drop actually reads, or some assignment
    // must set again for a later one.
    for (const auto &exit : facts.exits) {
      for (const auto &group : exit.groups) {
        for (const auto &owned : group) {
          if (!checked.drop_plans.contains(cfg.locals[owned.local].type)) {
            continue;
          }
          if (owned.state == move_state::maybe_moved &&
              plan.can_flag(owned.local)) {
            plan.require(owned.local);
          }
          require_parts(plan, owned.moved_parts);
        }
      }
    }
    for (const auto &assign : facts.assignments) {
      for (auto at = assign.local; at != k_no_local;
           at = cfg.locals[at].parent) {
        if (assign.state == move_state::maybe_moved && plan.can_flag(at)) {
          plan.require(at);
        }
      }
      require_parts(plan, assign.moved_parts);
    }
    plan.assign(schedule);

    for (const auto &local : cfg.locals) {
      if (local.whole && local.node != nullptr) {
        schedule.owned_bindings.insert(local.node);
      }
    }
    schedule.owning_patterns.insert(cfg.owning_patterns.begin(),
                                    cfg.owning_patterns.end());
    schedule.owning_subjects.insert(cfg.owning_subjects.begin(),
                                    cfg.owning_subjects.end());
    schedule.leftover_drops.insert(cfg.leftover_drops.begin(),
                                   cfg.leftover_drops.end());
    schedule.unmatched_drops.insert(cfg.unmatched_drops.begin(),
                                    cfg.unmatched_drops.end());
    schedule.owning_loops.insert(cfg.owning_loops.begin(),
                                 cfg.owning_loops.end());
    schedule.loop_handles.insert(cfg.loop_handles.begin(),
                                 cfg.loop_handles.end());
    schedule.consuming_calls.insert(cfg.consuming_calls.begin(),
                                    cfg.consuming_calls.end());
    schedule.temporary_ends.insert(cfg.temporary_ends.begin(),
                                   cfg.temporary_ends.end());
    schedule.owned_captures.insert(cfg.owned_captures.begin(),
                                   cfg.owned_captures.end());

    for (const auto &assign : facts.assignments) {
      auto drop = assignment_drop{};
      drop.drop_old = assign.state != move_state::moved;
      if (assign.local != k_no_local) {
        // Moved on some paths: the old value is dropped when the flags of
        // the target and of everything it is a field of are all set. One
        // that cannot have a flag leaks rather than drop a moved-from value.
        for (auto at = assign.local;
             at != k_no_local && assign.state == move_state::maybe_moved;
             at = cfg.locals[at].parent) {
          const auto flag = plan.flag(at);
          if (!flag.has_value()) {
            drop.drop_old = false;
            break;
          }
          drop.when.insert(drop.when.begin(), *flag);
        }
        drop.moved_paths = moved_paths_of(cfg, plan, assign.moved_parts);
        drop.sets = plan.flags_under(assign.local);
      }
      schedule.assignments[assign.key] = std::move(drop);
    }

    for (const auto &exit : facts.exits) {
      auto &drops = schedule.exits[exit.key];
      for (const auto &group : exit.groups) {
        for (const auto &owned : group) {
          const auto &info = cfg.locals[owned.local];
          if (!checked.drop_plans.contains(info.type) ||
              owned.state == move_state::moved) {
            continue;
          }
          auto flag = std::optional<std::size_t>{};
          if (owned.state == move_state::maybe_moved) {
            flag = plan.flag(owned.local);
            if (!flag.has_value()) {
              continue;
            }
          }
          drops.push_back(pending_drop{
              .node = info.node,
              .name = info.name,
              .type = info.type,
              .flag = flag,
              .moved_paths = moved_paths_of(cfg, plan, owned.moved_parts)});
        }
      }
    }
  }
  return schedule;
}

} // namespace cinder::hir
