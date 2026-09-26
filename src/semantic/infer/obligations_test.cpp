// Tests for the obligation queue (`spec/inference-rewrite.md` phase 4).
//
// The resolvers below are stand-ins — the real ones are the existing impl
// lookup and `reason.cpp`, plugged in behind the same signature in phase 7.
// What is under test here is the *queue*: that an obligation is retried when
// (and only when) something it watches becomes known, that the loop settles
// rather than spins, that a merge does not strand a watcher, and that a
// default can never beat a real constraint to the answer.

#include <iostream>
#include <string>
#include <vector>

#include "src/parser/source_location.h"
#include "src/semantic/infer/infer_ctxt.h"
#include "src/semantic/infer/obligations.h"
#include "src/semantic/infer/unify.h"
#include "src/semantic/types.h"
#include "src/testing/test_assert.h"

namespace {

using cinder::semantic::type_id;
using cinder::semantic::type_table;
using cinder::semantic::infer::infer_ctxt;
using cinder::semantic::infer::k_no_cause;
using cinder::semantic::infer::obligation;
using cinder::semantic::infer::obligation_kind;
using cinder::semantic::infer::obligation_outcome;
using cinder::semantic::infer::obligation_queue;
using cinder::semantic::infer::obligation_report;
using cinder::semantic::infer::unifier;
using cinder::testing::expect;

auto nowhere() -> cinder::source_location { return {}; }

struct fixture {
  type_table table;
  infer_ctxt ctx{table};
  unifier engine{table, ctx};
  obligation_queue queue{ctx, engine};

  auto var(std::string name) -> type_id {
    return ctx.fresh_type(std::move(name), nowhere());
  }
};

/// An obligation is attempted once, and retried only when a variable it
/// watches gains a solution.
auto test_wakes_only_on_a_solution() -> void {
  auto f = fixture{};
  const auto a = f.var("a");
  const auto b = f.var("b");
  auto attempts = 0;

  f.queue.set_resolver(obligation_kind::method_call,
                       [&](const obligation &goal) -> obligation_report {
                         ++attempts;
                         if (f.ctx.solution(goal.watches.front()).has_value()) {
                           return {.outcome = obligation_outcome::discharged};
                         }
                         return {.outcome = obligation_outcome::waiting,
                                 .detail = "the receiver type is not known"};
                       });

  const auto id = f.queue.add(obligation{.kind = obligation_kind::method_call,
                                         .watches = {a},
                                         .goal = "`push` on `?a`"});
  expect(f.queue.flush().has_value(), "expected the flush to settle");
  expect(attempts == 1, "expected exactly one attempt while nothing is known");
  expect(f.queue.stalled().size() == 1, "expected it to be stalled");
  expect(f.queue.waiting_for(id) == "the receiver type is not known",
         "expected the stall to say what is missing");

  // Solving an *unrelated* variable must not wake it.
  expect(f.ctx.bind(b, f.table.builtin("int32"), k_no_cause).has_value(),
         "expected the bind");
  expect(f.queue.flush().has_value(), "expected the flush to settle");
  expect(attempts == 1, "expected an unrelated solution not to wake it");

  // Solving the watched one does.
  expect(f.ctx.bind(a, f.table.builtin("str"), k_no_cause).has_value(),
         "expected the bind");
  expect(f.queue.flush().has_value(), "expected the flush to settle");
  expect(attempts == 2,
         "expected the watched solution to wake it exactly once");
  expect(f.queue.stalled().empty(), "expected it to be discharged");
}

/// A watcher must survive its variable being merged into another class.
/// Without re-indexing, the obligation would sit filed under a variable that
/// never receives another solution — a stall with no explanation.
auto test_merge_does_not_strand_a_watcher() -> void {
  auto f = fixture{};
  const auto a = f.var("a");
  const auto b = f.var("b");
  auto discharged = false;

  f.queue.set_resolver(obligation_kind::trait_bound,
                       [&](const obligation &goal) -> obligation_report {
                         if (f.ctx.solution(goal.watches.front()).has_value()) {
                           discharged = true;
                           return {.outcome = obligation_outcome::discharged};
                         }
                         return {.outcome = obligation_outcome::waiting,
                                 .detail = "`T` is not known"};
                       });

  f.queue.add(obligation{.kind = obligation_kind::trait_bound,
                         .watches = {a},
                         .goal = "`T: ord`"});
  expect(f.queue.flush().has_value(), "expected the flush to settle");
  expect(!discharged, "expected it to wait");

  expect(f.ctx.bind(a, b, k_no_cause).has_value(), "expected the merge");
  expect(f.queue.flush().has_value(), "expected the flush to settle");
  expect(f.ctx.bind(b, f.table.builtin("int32"), k_no_cause).has_value(),
         "expected the solution on the other side of the merge");
  expect(f.queue.flush().has_value(), "expected the flush to settle");
  expect(discharged, "expected the merged-away watch to still wake it");
}

/// A resolver that discharges one obligation by solving a variable must wake
/// the obligation waiting on it, in the same flush.
auto test_chained_obligations_settle_in_one_flush() -> void {
  auto f = fixture{};
  const auto a = f.var("a");
  const auto b = f.var("b");
  const auto int32 = f.table.builtin("int32");

  f.queue.set_resolver(
      obligation_kind::method_call,
      [&](const obligation &goal) -> obligation_report {
        const auto watched = goal.watches.front();
        if (!f.ctx.solution(watched).has_value()) {
          return {.outcome = obligation_outcome::waiting, .detail = "unknown"};
        }
        // Discharging this one teaches the store something new.
        if (watched == a && !f.ctx.solution(b).has_value()) {
          expect(f.ctx.bind(b, f.ctx.zonk(a), k_no_cause).has_value(),
                 "expected the resolver's own bind to hold");
        }
        return {.outcome = obligation_outcome::discharged};
      });

  f.queue.add(obligation{
      .kind = obligation_kind::method_call, .watches = {a}, .goal = "first"});
  f.queue.add(obligation{
      .kind = obligation_kind::method_call, .watches = {b}, .goal = "second"});
  expect(f.ctx.bind(a, int32, k_no_cause).has_value(), "expected the bind");

  expect(f.queue.flush().has_value(), "expected the flush to settle");
  expect(f.queue.stalled().empty(),
         "expected the second obligation to be woken by the first");
  expect(f.ctx.zonk(b) == int32, "expected the chained solution");
}

/// A default may only apply once everything else has stalled.
///
/// Both resolvers below bind *unconditionally* and report whether their bind
/// took. That is deliberate: if either checked `solution(...)` first, the
/// resolver would be enforcing last-resort itself and the test would pass
/// however the queue ordered them — which is exactly the blind assertion
/// this file is supposed to avoid. Ordering must be the only thing that
/// decides the answer.
auto test_defaulting_is_last_resort() -> void {
  // With nothing else to say, the default is what answers.
  auto f = fixture{};
  const auto a = f.var("a");
  const auto int32 = f.table.builtin("int32");
  f.queue.set_resolver(obligation_kind::defaulting,
                       [&](const obligation &) -> obligation_report {
                         expect(f.ctx.bind(a, int32, k_no_cause).has_value(),
                                "expected the default to bind");
                         return {.outcome = obligation_outcome::discharged};
                       });
  f.queue.add(obligation{.kind = obligation_kind::defaulting,
                         .watches = {a},
                         .goal = "an integer literal defaults to `int32`"});
  expect(f.queue.flush().has_value(), "expected the flush to settle");
  expect(f.ctx.zonk(a) == int32,
         "expected the default to apply once nothing else can");

  // Now with a real constraint that only becomes available *during* the
  // fixpoint: a method-call obligation, woken by `c` being known, whose
  // resolution is what pins `b` down. The default must lose the race.
  auto g = fixture{};
  const auto b = g.var("b");
  const auto c = g.var("c");
  const auto str = g.table.builtin("str");
  auto default_took = false;

  g.queue.set_resolver(obligation_kind::method_call,
                       [&](const obligation &) -> obligation_report {
                         // Discharging the call is what teaches `b` its type.
                         (void)g.ctx.bind(b, str, k_no_cause);
                         return {.outcome = obligation_outcome::discharged};
                       });
  g.queue.set_resolver(
      obligation_kind::defaulting,
      [&](const obligation &) -> obligation_report {
        default_took =
            g.ctx.bind(b, g.table.builtin("int32"), k_no_cause).has_value();
        return {.outcome = obligation_outcome::discharged};
      });

  g.queue.add(obligation{.kind = obligation_kind::defaulting,
                         .watches = {b},
                         .goal = "an integer literal defaults to `int32`"});
  g.queue.add(obligation{.kind = obligation_kind::method_call,
                         .watches = {c},
                         .goal = "`describe` on `?c`"});
  expect(g.ctx.bind(c, str, k_no_cause).has_value(),
         "expected the receiver to be known");

  expect(g.queue.flush().has_value(), "expected the flush to settle");
  expect(g.ctx.zonk(b) == str,
         "expected the real constraint to decide, not the default");
  expect(!default_took, "expected the default to have had nothing left to do");
}

/// A failing resolver stops the flush and names the obligation, rather than
/// being retried forever.
auto test_failure_is_reported() -> void {
  auto f = fixture{};
  const auto a = f.var("a");
  f.queue.set_resolver(obligation_kind::refinement,
                       [](const obligation &) -> obligation_report {
                         return {.outcome = obligation_outcome::failed,
                                 .detail = "`i < n` does not follow"};
                       });
  const auto id = f.queue.add(obligation{
      .kind = obligation_kind::refinement, .watches = {a}, .goal = "`i < n`"});

  const auto result = f.queue.flush();
  expect(!result.has_value(), "expected the failure to stop the flush");
  expect(result.error().id == id,
         "expected the failing obligation to be named");
  expect(result.error().detail == "`i < n` does not follow",
         "expected the resolver's explanation to survive");
}

/// A kind nobody discharges stalls readably instead of crashing.
auto test_missing_resolver_stalls() -> void {
  auto f = fixture{};
  const auto id = f.queue.add(obligation{.kind = obligation_kind::trait_bound,
                                         .watches = {f.var("a")},
                                         .goal = "`T: ord`"});
  expect(f.queue.flush().has_value(), "expected the flush to settle");
  expect(f.queue.stalled().size() == 1, "expected it to stall");
  expect(f.queue.waiting_for(id).contains("no resolver"),
         "expected the stall to say why");
}

/// The queue drives the unifier's postponed constraints in the same fixpoint,
/// so the two cannot deadlock on each other.
auto test_deferred_constraints_are_part_of_the_fixpoint() -> void {
  auto f = fixture{};
  const auto int32 = f.table.builtin("int32");
  const auto head = f.ctx.fresh_ctor(1, "F", nowhere());
  const auto other = f.ctx.fresh_ctor(1, "G", nowhere());
  const auto left = f.table.param_app(head, {f.var("A")});
  const auto right = f.table.param_app(other, {int32});

  expect(f.engine.unify(left, right, k_no_cause).has_value(),
         "expected the pair to postpone");
  expect(f.engine.deferred().size() == 1, "expected one deferred constraint");

  // An obligation whose resolver solves the head is what unblocks it.
  f.queue.set_resolver(
      obligation_kind::trait_bound,
      [&](const obligation &) -> obligation_report {
        expect(f.ctx
                   .bind(other, f.table.ctor_ref("option", "", nullptr, 1),
                         k_no_cause)
                   .has_value(),
               "expected the head to solve");
        return {.outcome = obligation_outcome::discharged};
      });
  f.queue.add(obligation{.kind = obligation_kind::trait_bound,
                         .watches = {other},
                         .goal = "`G: functor`"});

  expect(f.queue.flush().has_value(), "expected the flush to settle");
  expect(f.engine.deferred().empty(),
         "expected the postponed constraint to have been retried and resolved");
  expect(f.ctx.zonk(left) == f.table.builtin_generic("option", {int32}),
         "expected both sides to have become the same type");
}

} // namespace

auto main() -> int {
  test_wakes_only_on_a_solution();
  test_merge_does_not_strand_a_watcher();
  test_chained_obligations_settle_in_one_flush();
  test_defaulting_is_last_resort();
  test_failure_is_reported();
  test_missing_resolver_stalls();
  test_deferred_constraints_are_part_of_the_fixpoint();
  std::cout << "obligations_test passed\n";
  return 0;
}
