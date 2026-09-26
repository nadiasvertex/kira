#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "src/semantic/infer/infer_ctxt.h"
#include "src/semantic/infer/unify.h"
#include "src/semantic/types.h"

namespace cinder::semantic::infer {

// ==========================================================================
//  The obligation queue
//
//  Phase 4 of `spec/inference-rewrite.md`. Method resolution, trait
//  selection and refinement narrowing are *not* unification and must not
//  touch the substitution — but all three share one shape: a decision that
//  cannot be made yet, must be retried when something it depends on becomes
//  known, and must be reported if it never does.
//
//  So they are one mechanism with several candidate sources, not three
//  mechanisms with better types. If this file ever grows a
//  `method_obligation`, a `trait_obligation` and an `operator_obligation`,
//  the rewrite has rebuilt the state it set out to replace.
// ==========================================================================

/// Which candidate source discharges an obligation. The kinds differ only in
/// who is asked; the queue treats them identically.
enum class obligation_kind : uint8_t {
  /// A method call: candidate assembly over the existing impl lookup.
  method_call,
  /// A `where` bound: the same, plus the declared clauses when abstract.
  trait_bound,
  /// A refinement predicate, discharged by `reason.cpp`'s `solve(facts,
  /// goal)` — already written, and plugged in behind this kind rather than
  /// reimplemented.
  refinement,
  /// A last-resort candidate, Idris 2 `%defaulthint` style. Deliberately
  /// *not* a rule inside the solver: an unconstrained integer literal
  /// becomes `int32` because a declaration says that candidate loses to
  /// every real constraint, which is visible in source and explicable in a
  /// diagnostic.
  defaulting,
};

/// The obligation's name for a reader.
[[nodiscard]] auto obligation_kind_name(obligation_kind kind)
    -> std::string_view;

/// Index into an `obligation_queue`.
using obligation_id = uint32_t;

/// One decision waiting to be made.
struct obligation {
  obligation_kind kind = obligation_kind::method_call;
  /// The types this obligation depends on. It is retried when any of them
  /// gains a solution, and never otherwise — this is what keeps the fixpoint
  /// from being a sweep over everything on every pass.
  std::vector<type_id> watches;
  /// What is being required, phrased for a diagnostic: "`push` on `?a`",
  /// "`T: ord`", "`i < n`".
  std::string goal;
  cause_id why = k_no_cause;
  /// An opaque handle for the resolver — an index into whatever table the
  /// caller assembles candidates from. The queue never interprets it, which
  /// is what keeps candidate assembly on the checker's side of the line.
  uint64_t payload = 0;
};

/// What an attempt to discharge one came to.
enum class obligation_outcome : uint8_t {
  discharged, ///< Decided. The resolver has recorded whatever it decided.
  waiting,    ///< Not yet decidable; retry when a watch is solved.
  failed,     ///< Decidably wrong. A real diagnostic.
};

/// An attempt's answer.
struct obligation_report {
  obligation_outcome outcome = obligation_outcome::waiting;
  /// For `failed`: why, phrased for a diagnostic. For `waiting`: what is
  /// missing, so a stall can say so.
  std::string detail;
};

/// A resolver is handed the obligation and answers. It closes over whatever
/// it needs — the checker, the store, the impl tables — so the queue depends
/// on none of them.
using obligation_resolver =
    std::function<obligation_report(const obligation &)>;

/// An obligation that could not be met.
struct obligation_failure {
  obligation_id id = 0;
  std::string detail;
  /// Set when the failure came from unification rather than a resolver.
  std::optional<unify_error> unification;
};

/// The queue, its wake index, and the fixpoint loop.
class obligation_queue {
public:
  /// Borrows the store and the unifier. The unifier is here because its
  /// postponed constraints are part of the same fixpoint: a deferred
  /// `F[A] ~ G[B]` and a waiting `T: ord` unblock each other, and running
  /// them in separate loops would need one of them to be retried after the
  /// other had finished — which is the shape that produces order-dependent
  /// inference.
  obligation_queue(infer_ctxt &ctx, unifier &engine);

  /// Registers who discharges a kind. A kind with no resolver always waits,
  /// which is what a partially-wired caller should get rather than a crash.
  auto set_resolver(obligation_kind kind, obligation_resolver resolver) -> void;

  /// Records an obligation. It is attempted on the next `flush`.
  auto add(obligation goal) -> obligation_id;

  /// Runs to fixpoint: retry woken obligations and postponed constraints
  /// until neither makes progress.
  ///
  /// Defaulting runs only once everything else has stalled, and then one
  /// candidate at a time, each followed by another full fixpoint. That
  /// ordering is the whole meaning of "last resort": a default must never
  /// win a race against a real constraint that had not yet arrived.
  auto flush() -> std::expected<void, obligation_failure>;

  /// The obligations still undischarged after a `flush` — the ones a
  /// "cannot infer" diagnostic reports.
  [[nodiscard]] auto stalled() const -> std::vector<obligation_id>;
  [[nodiscard]] auto at(obligation_id id) const -> const obligation &;
  /// What the last attempt on `id` said it was waiting for.
  [[nodiscard]] auto waiting_for(obligation_id id) const -> const std::string &;

private:
  /// One attempt at one obligation, updating the wake index.
  auto attempt(obligation_id id) -> std::expected<bool, obligation_failure>;
  /// Points `id`'s watches at their current representatives, so a merge does
  /// not strand it under a name nothing will ever solve.
  auto index_watches(obligation_id id) -> void;
  /// Marks every obligation watching a newly-solved variable as dirty.
  auto wake_from(const std::vector<type_id> &solved) -> void;
  /// Retries postponed constraints and obligations until neither moves.
  auto run_fixpoint() -> std::expected<void, obligation_failure>;

  struct record {
    obligation goal;
    bool discharged = false;
    std::string waiting_for;
  };

  infer_ctxt *ctx_;
  unifier *engine_;
  std::vector<record> records_;
  std::unordered_map<obligation_kind, obligation_resolver> resolvers_;
  /// Watched representative -> the obligations watching it.
  std::unordered_map<type_id, std::vector<obligation_id>> wake_index_;
  /// Obligations to attempt on the next pass, in registration order so the
  /// result does not depend on hash iteration.
  std::vector<obligation_id> dirty_;
};

} // namespace cinder::semantic::infer
