#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "src/semantic/infer/infer_ctxt.h"
#include "src/semantic/types.h"

namespace kira::semantic::infer {

// ==========================================================================
//  The unifier
//
//  Phase 2 of `spec/inference-rewrite.md`: one `unify` covering every
//  `type_kind`, replacing the 28 `unify_rigid` call sites and the three
//  string-keyed matchers beside them.
//
//  Three rules, in order:
//
//   1. **Rigid-rigid** — structural descent; two different constructors are
//      a mismatch and nothing is invented.
//   2. **Variable-headed applications** — `F[A] ~ option[int32]` solves
//      `F := option, A := int32` by matching the outermost nominal
//      constructor. That is ch. 37's "rigid pattern-matching only" rule, and
//      it is Miller's pattern fragment: decidable, unitary (one most-general
//      solution, so no backtracking and no ambiguity errors).
//   3. **Value slots** — `vec[T, n + 1] ~ vec[T, 3]`. Phase 2 decides
//      satisfiability and postpones the rest; phase 3 solves it.
//
//  Anything outside the fragment **postpones** rather than fails. Ch. 37's
//  letter says a flex head against a non-application "fails", but that
//  conflates "cannot be in this fragment" with "cannot ever be": the shape
//  may still be unknown. The distinction is the difference between a wrong
//  error and a later right one.
// ==========================================================================

/// Why a `unify` was refused. Every one of these is a *definite* answer —
/// a constraint that might yet be satisfiable postpones instead.
enum class unify_failure : uint8_t {
  mismatch,     ///< Two different rigid constructors: `list` against `option`.
  arity,        ///< The same constructor at different argument counts.
  mutability,   ///< `&mut T` against `&T`.
  value,        ///< A value equation with no solution: `n + 1 ~ 0` for usize.
  variable,     ///< A `bind` was refused (sort, arity, or occurs check).
  out_of_scope, ///< A flex head against something it can never match.
};

/// A refused unification, carrying both the outermost pair the caller asked
/// about and the innermost pair that actually clashed.
///
/// Both, because they answer different questions and a good diagnostic wants
/// each: the outer pair is what the user wrote (`fn(int32) -> list[str]`
/// against `fn(int32) -> list[int32]`), the inner pair is where it went wrong
/// (`str` against `int32`). Reporting only the outer buries the fault;
/// reporting only the inner loses the context.
struct unify_error {
  unify_failure failure = unify_failure::mismatch;
  type_id expected = k_unknown_type;
  type_id found = k_unknown_type;
  type_id expected_part = k_unknown_type;
  type_id found_part = k_unknown_type;
  cause_id why = k_no_cause;
  /// A rendered explanation of this specific refusal.
  std::string detail;
};

/// A constraint that could not be decided yet and must be retried once more
/// is known. Phase 4's obligation queue takes ownership of these; until then
/// `unifier::retry_deferred` drives them directly.
struct deferred_constraint {
  type_id left = k_unknown_type;
  type_id right = k_unknown_type;
  cause_id why = k_no_cause;
  /// What is missing, for the "cannot infer" diagnostic if it never arrives.
  std::string waiting_for;
};

/// Unification over one `infer_ctxt`.
///
/// Stateless apart from the deferred list: every solution lives in the store,
/// so two unifiers over one store would be indistinguishable from one. The
/// class exists to own the deferred constraints and to keep the recursion's
/// helpers off the store's interface.
class unifier {
public:
  unifier(type_table &table, infer_ctxt &ctx);

  /// Requires `expected` and `found` to denote the same type.
  ///
  /// The two sides are treated symmetrically — unification is an equation,
  /// not a direction — but they are *named* asymmetrically because a
  /// diagnostic needs to know which one the context required and which one
  /// the expression offered.
  ///
  /// Succeeds having recorded solutions, postponed what it could not decide,
  /// or returns the refusal. It never reports a diagnostic itself: who
  /// reports, and which constraint is to blame, is phase 5.
  auto unify(type_id expected, type_id found, cause_id why)
      -> std::expected<void, unify_error>;

  /// Re-runs every deferred constraint once. Returns whether any of them
  /// became decidable, so a caller can loop to fixpoint; the ones still
  /// undecided stay deferred.
  auto retry_deferred() -> std::expected<bool, unify_error>;

  /// The constraints still waiting. Non-empty at the end of a function body
  /// means something was never pinned down.
  [[nodiscard]] auto deferred() const
      -> const std::vector<deferred_constraint> &;

private:
  /// The recursive step. `root_expected`/`root_found` are carried untouched
  /// so an error reports both the outer pair and the inner one.
  auto step(type_id expected, type_id found, cause_id why,
            type_id root_expected, type_id root_found)
      -> std::expected<void, unify_error>;
  /// Rule 2: a `param_app` whose head is an unsolved constructor variable.
  auto unify_flex_app(type_id app, type_id other, bool app_is_expected,
                      cause_id why, type_id root_expected, type_id root_found)
      -> std::expected<void, unify_error>;
  /// Rule 3: two value slots, or a value variable and a value.
  auto unify_values(type_id expected, type_id found, cause_id why,
                    type_id root_expected, type_id root_found)
      -> std::expected<void, unify_error>;
  /// Records a constraint to retry later.
  auto postpone(type_id left, type_id right, cause_id why,
                std::string waiting_for) -> void;
  /// Builds an error, filling in both pairs.
  [[nodiscard]] auto refuse(unify_failure failure, type_id expected,
                            type_id found, type_id root_expected,
                            type_id root_found, cause_id why,
                            std::string detail) const -> unify_error;

  type_table *table_;
  infer_ctxt *ctx_;
  std::vector<deferred_constraint> deferred_;
};

} // namespace kira::semantic::infer
