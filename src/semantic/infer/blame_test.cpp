// Tests for blame (`spec/inference-rewrite.md` phase 5).
//
// What is under test is the *choice*: given several constraints that
// disagree, which one does the compiler call the mistake, and what does it
// say about the rest. The golden corpus in
// `src/testdata/inference_diagnostics/` holds the end-to-end bar; this file
// holds the reasoning, so a heuristic can be broken and caught without a
// whole program around it.

#include <iostream>
#include <string>

#include "src/parser/source_location.h"
#include "src/semantic/infer/blame.h"
#include "src/semantic/infer/infer_ctxt.h"
#include "src/semantic/types.h"
#include "src/testing/test_assert.h"

namespace {

using kira::semantic::type_id;
using kira::semantic::type_table;
using kira::semantic::infer::blame_report;
using kira::semantic::infer::cause;
using kira::semantic::infer::constraint_graph;
using kira::semantic::infer::explain_conflict;
using kira::semantic::infer::explain_stall;
using kira::semantic::infer::infer_ctxt;
using kira::semantic::infer::k_no_cause;
using kira::semantic::infer::stalled_variable;
using kira::testing::expect;

/// A distinct location per site. `source_location` is byte offsets, not
/// line numbers, so the tests use the offset as a stand-in identity: what
/// matters here is *which* site a report points at, not how it renders.
auto at_line(uint32_t line) -> kira::source_location {
  return kira::source_location{
      .file_id = 0, .span = kira::source_span{.start = line, .end = line + 1}};
}

struct fixture {
  type_table table;
  infer_ctxt ctx{table};
  constraint_graph graph{ctx};

  auto var(std::string name) -> type_id {
    return ctx.fresh_type(std::move(name), at_line(1));
  }

  auto because(std::string reason, uint32_t line) -> uint32_t {
    return ctx.add_cause(
        cause{.where = at_line(line), .reason = std::move(reason)});
  }
};

/// Four constraints say `int32` and one says `str`. The `str` one is the
/// mistake; the other four are why.
auto test_the_minority_is_blamed() -> void {
  auto f = fixture{};
  const auto result = f.var("the value of this `match`");
  const auto int32 = f.table.builtin("int32");
  const auto str = f.table.builtin("str");

  f.graph.record(result, int32, f.because("the first arm", 10));
  f.graph.record(result, int32, f.because("the second arm", 11));
  f.graph.record(result, int32, f.because("the third arm", 12));
  f.graph.record(result, str, f.because("the fourth arm", 13));
  f.graph.record(result, int32, f.because("the fifth arm", 14));

  const auto odd = f.graph.outliers(result);
  expect(odd.size() == 1, "expected exactly one outlier");
  expect(odd.front().required == str, "expected the `str` demand to be odd");

  const auto report = explain_conflict(f.graph, f.ctx, f.table, result);
  expect(report.has_value(), "expected a conflict to be explained");
  expect(report->primary.span.start == 13,
         "expected the caret on the minority constraint, not the last one");
  expect(report->headline == "expected `int32`, found `str`",
         std::string("unexpected headline: ") + report->headline);
  expect(report->label == "the fourth arm is `str`",
         std::string("unexpected label: ") + report->label);
  expect(report->help.contains("4 others here require `int32`"),
         std::string("expected the majority to be counted: ") + report->help);

  auto agreeing = 0;
  for (const auto &note : report->notes) {
    if (note.message == "this requires `int32`") {
      ++agreeing;
    }
  }
  expect(agreeing == 4,
         "expected every agreeing site to be shown as the evidence");
}

/// One against one. Neither is the mistake, and the compiler must say so
/// rather than pick. This is the assertion that stops the heuristic from
/// becoming a coin flip with a confident voice.
auto test_an_even_split_blames_neither() -> void {
  auto f = fixture{};
  const auto result = f.var("the value of this `if`");
  f.graph.record(result, f.table.builtin("int32"),
                 f.because("the `then` branch", 5));
  f.graph.record(result, f.table.builtin("str"),
                 f.because("the `else` branch", 7));

  expect(f.graph.outliers(result).empty(),
         "expected a tie to produce no outlier");

  const auto report = explain_conflict(f.graph, f.ctx, f.table, result);
  expect(report.has_value(), "expected the tie to still be explained");
  expect(report->headline ==
             "`int32` and `str` disagree, and nothing here says which is meant",
         std::string("unexpected headline: ") + report->headline);
  expect(report->help.contains("will not guess"),
         std::string("expected the message to admit the tie: ") + report->help);
  expect(report->notes.front().where.span.start == 5,
         "expected the other side to be shown too");
}

/// Constraints that all agree are not a conflict, and asking for blame
/// anyway must produce nothing rather than a fabricated message.
auto test_agreement_is_not_a_conflict() -> void {
  auto f = fixture{};
  const auto a = f.var("a");
  const auto int32 = f.table.builtin("int32");
  f.graph.record(a, int32, f.because("one", 1));
  f.graph.record(a, int32, f.because("two", 2));

  expect(f.graph.outliers(a).empty(), "expected no outlier among equals");
  expect(!explain_conflict(f.graph, f.ctx, f.table, a).has_value(),
         "expected no report where there is no disagreement");
}

/// A constraint recorded against a variable that was later merged into
/// another class must still count as a demand on it. Without this, the
/// evidence for a blame decision silently shrinks after a merge and the
/// majority can flip.
auto test_merged_classes_share_their_constraints() -> void {
  auto f = fixture{};
  const auto a = f.var("a");
  const auto b = f.var("b");
  const auto int32 = f.table.builtin("int32");
  const auto str = f.table.builtin("str");

  f.graph.record(a, int32, f.because("through `a`", 1));
  f.graph.record(b, int32, f.because("through `b`", 2));
  f.graph.record(b, str, f.because("the odd one", 3));
  expect(f.ctx.bind(a, b, k_no_cause).has_value(), "expected the merge");

  const auto demands = f.graph.demands_on(a);
  expect(demands.size() == 3,
         "expected constraints on both halves of the merged class");
  const auto odd = f.graph.outliers(a);
  expect(odd.size() == 1 && odd.front().required == str,
         "expected the merged evidence to pick the same outlier");
}

/// `var ~ var` asks nothing of the variable and must not be counted as a
/// demand — counting it would let a reflexive constraint tip a majority.
auto test_self_constraints_are_not_demands() -> void {
  auto f = fixture{};
  const auto a = f.var("a");
  f.graph.record(a, a, f.because("reflexive", 1));
  f.graph.record(a, f.table.builtin("int32"), f.because("real", 2));
  const auto demands = f.graph.demands_on(a);
  expect(demands.size() == 1,
         "expected the reflexive constraint to be ignored");
}

/// A stall with facts in scope and a stall with none are different mistakes
/// and must read differently. Collapsing them is how a compiler ends up
/// saying "type annotations needed" to someone who wrote five constraints.
auto test_a_stall_explains_rather_than_demands() -> void {
  const auto bare =
      explain_stall(stalled_variable{.var = 0,
                                     .origin = "`T` of `collect`",
                                     .where = at_line(9),
                                     .blocked_goals = {"`T: from_iter`"},
                                     .facts = {}});
  expect(bare.headline == "nothing so far determines `T` of `collect`",
         std::string("unexpected headline: ") + bare.headline);
  expect(bare.help.contains("Nothing in this program says"),
         std::string("unexpected help: ") + bare.help);
  expect(bare.notes.size() == 1 &&
             bare.notes.front().message == "waiting on it: `T: from_iter`",
         "expected the blocked goal to be named");

  const auto informed = explain_stall(stalled_variable{
      .var = 0,
      .origin = "`T` of `collect`",
      .where = at_line(9),
      .blocked_goals = {"`T: from_iter`"},
      .facts = {"`T: iterator`", "`T` is the element type of `xs`"}});
  expect(informed.help != bare.help,
         "expected a stall with facts to read differently from one without");
  expect(informed.help.contains("none of it is enough to choose"),
         std::string("unexpected help: ") + informed.help);

  auto listed = 0;
  for (const auto &note : informed.notes) {
    if (note.message.starts_with("known: ")) {
      ++listed;
    }
  }
  expect(listed == 2, "expected every known fact to be listed");

  // The phrase this whole phase exists to make unnecessary.
  expect(!bare.help.contains("type annotations needed") &&
             !informed.help.contains("type annotations needed"),
         "expected no message to fall back to `type annotations needed`");
}

/// A cause's own phrasing wins over the type's spelling, and the enclosing
/// frames are carried through as context. `usize` does not explain why a
/// length had to be one; "the length of `xs`" does.
auto test_causes_supply_the_wording() -> void {
  auto f = fixture{};
  const auto n = f.var("n");
  const auto four = f.table.builtin("int32");
  const auto five = f.table.builtin("str");

  const auto outer = f.ctx.add_cause(
      cause{.where = at_line(2), .reason = "the call to `zip`"});
  f.graph.record(
      n, four,
      f.ctx.add_cause(
          cause{.where = at_line(3), .reason = "argument 1", .parent = outer}));
  f.graph.record(
      n, four,
      f.ctx.add_cause(
          cause{.where = at_line(4), .reason = "argument 2", .parent = outer}));
  f.graph.record(n, five,
                 f.ctx.add_cause(cause{.where = at_line(5),
                                       .reason = "argument 3",
                                       .expected_desc = "the length of `xs`",
                                       .found_desc = "the length of `ys`",
                                       .parent = outer}));

  const auto report = explain_conflict(f.graph, f.ctx, f.table, n);
  expect(report.has_value(), "expected a conflict");
  expect(report->headline ==
             "expected the length of `xs` (`int32`), found the length of `ys` "
             "(`str`)",
         std::string("unexpected headline: ") + report->headline);

  auto saw_context = false;
  for (const auto &note : report->notes) {
    if (note.message == "while checking the call to `zip`") {
      saw_context = true;
      expect(note.where.span.start == 2,
             "expected the enclosing frame's location");
    }
  }
  expect(saw_context, "expected the enclosing cause to be carried through");
}

} // namespace

auto main() -> int {
  test_the_minority_is_blamed();
  test_an_even_split_blames_neither();
  test_agreement_is_not_a_conflict();
  test_merged_classes_share_their_constraints();
  test_self_constraints_are_not_demands();
  test_a_stall_explains_rather_than_demands();
  test_causes_supply_the_wording();
  std::cout << "blame_test passed\n";
  return 0;
}
