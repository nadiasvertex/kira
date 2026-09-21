#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "src/parser/source_location.h"
#include "src/semantic/infer/infer_ctxt.h"
#include "src/semantic/types.h"

namespace kira::semantic::infer {

// ==========================================================================
//  Blame
//
//  Phase 5 of `spec/inference-rewrite.md`. A constraint solver's natural
//  failure output is "expected X, found Y" at whichever constraint happened
//  to fail last, and "type annotations needed" when nothing failed but
//  nothing was determined either. Neither is acceptable in a compiler whose
//  stated job is to teach, so the machinery for doing better is built here,
//  first, against a golden corpus of wrong programs that was written before
//  it (`src/testdata/inference_diagnostics/`).
//
//  Two things are needed that a solver does not produce on its own:
//
//  1. A *retained* record of the constraints, so a failure can be explained
//     by the decisions that led to it rather than by the one that tripped
//     over them. Discarding a constraint once it has been solved is what
//     forces a compiler to reconstruct the story at failure time, which it
//     cannot do.
//  2. A rule for choosing *which* of several disagreeing constraints is the
//     mistake. When four branches say `int32` and one says `str`, the `str`
//     one is the error and the other four are the evidence — pointing at
//     whichever came last is a coin flip that reads as authoritative.
//
//  This file is deliberately free of `diagnostic.h`: it produces the
//  *content* of a message — where to point, what to say, what else to show —
//  and the checker renders it. That keeps the heuristics testable as
//  functions of a constraint graph rather than of a compiler session.
// ==========================================================================

/// One recorded constraint, kept after it is solved.
struct constraint_edge {
  type_id left = k_unknown_type;
  type_id right = k_unknown_type;
  cause_id why = k_no_cause;
};

/// Index into a `constraint_graph`.
using edge_id = uint32_t;

/// What a constraint asks of one particular variable.
///
/// `demand` is the other side of the constraint, zonked. A constraint that
/// does not resolve to `var` on either side has no demand of it and is not
/// counted.
struct demand {
  edge_id edge = 0;
  type_id required = k_unknown_type;
};

/// The constraints, retained, and the queries the heuristics run over them.
class constraint_graph {
public:
  explicit constraint_graph(infer_ctxt &ctx);

  /// Records that `left` and `right` were required to agree. Called for every
  /// constraint, whether or not it succeeds — a constraint that succeeded is
  /// precisely the evidence that makes a later one an outlier.
  auto record(type_id left, type_id right, cause_id why) -> edge_id;

  [[nodiscard]] auto size() const -> size_t;
  [[nodiscard]] auto at(edge_id id) const -> const constraint_edge &;

  /// Every constraint that mentions `var`'s class on one side, with what it
  /// required of it. In record order, so a tie is broken by source order
  /// rather than by a hash.
  [[nodiscard]] auto demands_on(type_id var) -> std::vector<demand>;

  /// The constraints on `var` whose demand is in the minority.
  ///
  /// Empty when there is no disagreement, and — deliberately — empty when the
  /// disagreement is an even split. A heuristic that invents a winner from a
  /// tie is worse than no heuristic: it produces a confident message pointing
  /// at an arbitrary one of two equally good candidates, and the reader has
  /// no way to tell that is what happened. `explain_conflict` shows both
  /// sides instead.
  [[nodiscard]] auto outliers(type_id var) -> std::vector<demand>;

private:
  infer_ctxt *ctx_;
  std::vector<constraint_edge> edges_;
};

/// A site worth showing alongside the primary one.
struct blame_note {
  source_location where;
  std::string message;
};

/// The content of one inference diagnostic.
///
/// Every field that a reader needs is separate, because the compiler's
/// standard is that a message says what was expected, what was found, why,
/// and how to fix it — and a single pre-joined string cannot be re-rendered
/// by a caller that wants to show them differently.
struct blame_report {
  /// Where the caret goes.
  source_location primary;
  /// The `error:` line.
  std::string headline;
  /// The label beside the caret.
  std::string label;
  /// The `help:` line — how to fix it. Never empty in a report this file
  /// produces; a message without a way out is the failure mode phase 5
  /// exists to prevent.
  std::string help;
  /// Supporting sites: the agreeing constraints that made this one the
  /// outlier, the enclosing frames, the obligations left waiting.
  std::vector<blame_note> notes;
};

/// Explains a disagreement about `var`.
///
/// `var` is the variable the conflicting constraints share. When one demand
/// is in the minority, it is blamed and the majority becomes the evidence;
/// when the split is even, neither is blamed and both are shown, because a
/// tie genuinely does not say which side is wrong.
///
/// Returns nothing when the constraints on `var` do not actually disagree —
/// a caller that reaches for blame without a conflict has a bug, and a
/// fabricated message would hide it.
[[nodiscard]] auto explain_conflict(constraint_graph &graph, infer_ctxt &ctx,
                                    const type_table &table, type_id var)
    -> std::optional<blame_report>;

/// What a variable is still waiting for.
struct stalled_variable {
  type_id var = k_unknown_type;
  /// The variable's `origin`, so the message can name it as the source wrote
  /// it ("`T` of `push`") rather than as `?4`.
  std::string origin;
  source_location where;
  /// The goals that cannot be decided until it is known, phrased as the
  /// obligation queue phrased them.
  std::vector<std::string> blocked_goals;
  /// What *is* known about it — the constraints it already participates in.
  /// A stall with facts in scope is a different message from a stall with
  /// none, and conflating them is how "type annotations needed" happens.
  std::vector<std::string> facts;
};

/// Explains why inference stopped without an answer.
///
/// This is the function that exists so that "type annotations needed" never
/// has to be printed. It names the variable, where it came from, what is
/// already known about it, and what decision is blocked on it — which
/// together are an explanation, where the annotation request is only an
/// instruction.
[[nodiscard]] auto explain_stall(const stalled_variable &stalled)
    -> blame_report;

} // namespace kira::semantic::infer
