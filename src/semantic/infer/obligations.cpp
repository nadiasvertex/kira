#include "src/semantic/infer/obligations.h"

#include <algorithm>
#include <format>
#include <utility>

namespace cinder::semantic::infer {

auto obligation_kind_name(obligation_kind kind) -> std::string_view {
  switch (kind) {
  case obligation_kind::method_call:
    return "method call";
  case obligation_kind::trait_bound:
    return "trait bound";
  case obligation_kind::refinement:
    return "refinement";
  case obligation_kind::defaulting:
    return "defaulting";
  }
  return "obligation";
}

obligation_queue::obligation_queue(infer_ctxt &ctx, unifier &engine)
    : ctx_(&ctx), engine_(&engine) {}

auto obligation_queue::set_resolver(obligation_kind kind,
                                    obligation_resolver resolver) -> void {
  resolvers_.insert_or_assign(kind, std::move(resolver));
}

auto obligation_queue::add(obligation goal) -> obligation_id {
  const auto id = static_cast<obligation_id>(records_.size());
  records_.push_back(record{.goal = std::move(goal)});
  index_watches(id);
  dirty_.push_back(id);
  return id;
}

auto obligation_queue::at(obligation_id id) const -> const obligation & {
  return records_[id].goal;
}

auto obligation_queue::waiting_for(obligation_id id) const
    -> const std::string & {
  return records_[id].waiting_for;
}

auto obligation_queue::stalled() const -> std::vector<obligation_id> {
  auto open = std::vector<obligation_id>{};
  for (size_t i = 0; i < records_.size(); ++i) {
    if (!records_[i].discharged) {
      open.push_back(static_cast<obligation_id>(i));
    }
  }
  return open;
}

/// Re-points the watches at their current representatives.
///
/// Called on every attempt that leaves the obligation waiting, because a
/// merge in between would otherwise leave it filed under a variable that no
/// longer receives solutions — a wake index that silently stops waking is
/// worse than none, since the symptom is a stall with no explanation.
auto obligation_queue::index_watches(obligation_id id) -> void {
  for (const auto watch : records_[id].goal.watches) {
    auto &watchers = wake_index_[ctx_->find(watch)];
    if (std::ranges::find(watchers, id) == watchers.end()) {
      watchers.push_back(id);
    }
  }
}

auto obligation_queue::wake_from(const std::vector<type_id> &solved) -> void {
  for (const auto id : solved) {
    for (const auto key : {id, ctx_->find(id)}) {
      const auto it = wake_index_.find(key);
      if (it == wake_index_.end()) {
        continue;
      }
      for (const auto watcher : it->second) {
        if (!records_[watcher].discharged &&
            std::ranges::find(dirty_, watcher) == dirty_.end()) {
          dirty_.push_back(watcher);
        }
      }
    }
  }
}

auto obligation_queue::attempt(obligation_id id)
    -> std::expected<bool, obligation_failure> {
  auto &entry = records_[id];
  if (entry.discharged) {
    return false;
  }
  const auto resolver = resolvers_.find(entry.goal.kind);
  if (resolver == resolvers_.end()) {
    // A kind nobody discharges waits forever rather than crashing, so a
    // partially-wired caller gets a stall it can read.
    entry.waiting_for =
        std::format("no resolver is registered for {} obligations",
                    obligation_kind_name(entry.goal.kind));
    index_watches(id);
    return false;
  }

  const auto report = resolver->second(entry.goal);
  switch (report.outcome) {
  case obligation_outcome::discharged:
    entry.discharged = true;
    entry.waiting_for.clear();
    return true;
  case obligation_outcome::waiting:
    entry.waiting_for = report.detail;
    index_watches(id);
    return false;
  case obligation_outcome::failed:
    break;
  }
  return std::unexpected(obligation_failure{.id = id, .detail = report.detail});
}

/// Retry woken obligations and postponed constraints until neither moves.
///
/// The two are interleaved deliberately. A deferred `F[A] ~ G[B]` and a
/// waiting `T: ord` can unblock each other, and draining them in separate
/// loops would make the answer depend on which drained first — which is
/// exactly the order-dependent inference this rewrite exists to remove.
auto obligation_queue::run_fixpoint()
    -> std::expected<void, obligation_failure> {
  while (true) {
    auto progress = false;

    auto pending = std::vector<obligation_id>{};
    pending.swap(dirty_);
    for (const auto id : pending) {
      // Defaulting is not part of this loop at all. It is attempted only by
      // `flush`, once this fixpoint has settled — a default asked here would
      // answer before the constraint that was about to arrive, which is
      // indistinguishable from a wrong inference and impossible to explain.
      if (records_[id].goal.kind == obligation_kind::defaulting) {
        continue;
      }
      auto attempted = attempt(id);
      if (!attempted.has_value()) {
        return std::unexpected(std::move(attempted.error()));
      }
      progress = progress || *attempted;
    }

    auto retried = engine_->retry_deferred();
    if (!retried.has_value()) {
      return std::unexpected(
          obligation_failure{.detail = retried.error().detail,
                             .unification = std::move(retried.error())});
    }
    progress = progress || *retried;

    // Anything solved during this pass — by a resolver or by the unifier —
    // wakes whoever was waiting on it.
    const auto solved = ctx_->take_newly_solved();
    if (!solved.empty()) {
      wake_from(solved);
      progress = true;
    }

    // No progress means every remaining obligation has been asked and is
    // still waiting, and no constraint became decidable. Stop rather than
    // spin: this is the stall, and `stalled()` is what reports it.
    if (!progress) {
      return {};
    }
  }
}

auto obligation_queue::flush() -> std::expected<void, obligation_failure> {
  return flush([](type_id) -> bool { return true; });
}

auto obligation_queue::flush(const std::function<bool(type_id)> &may_default)
    -> std::expected<void, obligation_failure> {
  auto settled = run_fixpoint();
  if (!settled.has_value()) {
    return settled;
  }

  // Only now, with everything else stalled, may a default be applied — and
  // one at a time, each followed by another full fixpoint. A default that
  // won a race against a constraint which had not yet arrived would be
  // indistinguishable from a wrong inference, and impossible to explain.
  while (true) {
    auto applied = false;
    for (size_t i = 0; i < records_.size(); ++i) {
      const auto id = static_cast<obligation_id>(i);
      if (records_[i].discharged ||
          records_[i].goal.kind != obligation_kind::defaulting ||
          records_[i].goal.watches.empty() ||
          !may_default(records_[i].goal.watches.front())) {
        continue;
      }
      auto attempted = attempt(id);
      if (!attempted.has_value()) {
        return std::unexpected(std::move(attempted.error()));
      }
      if (*attempted) {
        applied = true;
        break;
      }
    }
    if (!applied) {
      return {};
    }
    const auto solved = ctx_->take_newly_solved();
    wake_from(solved);
    auto again = run_fixpoint();
    if (!again.has_value()) {
      return again;
    }
  }
}

} // namespace cinder::semantic::infer
