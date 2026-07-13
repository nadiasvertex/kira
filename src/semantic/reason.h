#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "src/semantic/linear_poly.h"

namespace kira::semantic {

// ==========================================================================
//  Reasoning — the constraint solver
//
//  The second of compile time's two machineries (`kira-reference.md`
//  §Compile-Time Semantics): *Evaluation* runs Kira on values it knows,
//  *Reasoning* proves facts about values it doesn't. This is Reasoning. It
//  discharges refinement predicates, dependent-type obligations, and static
//  contract conditions, and it does exactly one thing: decide whether a set
//  of facts entails a goal.
//
//  Its fragment is fixed, small, and non-negotiable: quantifier-free linear
//  integer arithmetic over uninterpreted atoms. See
//  `spec/dependent-types-design.md` §4 — that document, not this header, is
//  where the boundary moves if it ever moves. The fragment is chosen so that
//  deciding is *total*: `solve` always terminates, with no timeout, no
//  search, and no configuration. It is incomplete in exchange, and that is
//  the deal — an honest "cannot prove" that routes the user to `try_from`
//  beats a prover that sometimes hangs.
// ==========================================================================

/// How a constraint's polynomial relates to zero. `<` and `<=` have no
/// entries of their own: they normalize by negating the polynomial, and `>`
/// normalizes to `>= 1` because the fragment is over *integers*. Two
/// relations, therefore, plus disequality.
enum class relation : uint8_t {
  eq, ///< `poly == 0`
  ne, ///< `poly != 0`
  ge, ///< `poly >= 0`
};

/// One constraint, `poly REL 0`, over solver atoms.
///
/// `label` carries the constraint's *source* spelling (`i < len(v)`,
/// `n >= 0`) purely so a failed obligation can show the user what it knew and
/// what it wanted in the language they wrote, not in the solver's normalized
/// form. Nothing in the decision procedure reads it — but the diagnostic is
/// the product here, so it is not optional either.
struct constraint {
  linear_poly poly;
  relation rel = relation::ge;
  std::string label;
};

/// A conjunction of constraints — what the facts in scope amount to, and what
/// one alternative of a goal amounts to.
using fact_set = std::vector<constraint>;

/// A goal in disjunctive normal form: proved when *any* alternative holds.
/// Disjunction only ever arrives from a predicate the user wrote with `or`;
/// most goals are a single alternative holding a single constraint.
using goal_form = std::vector<fact_set>;

/// What the solver could establish about an obligation.
enum class proof_result : uint8_t {
  /// The facts entail the goal. The obligation is discharged: a narrowing
  /// coercion is accepted, a bounds check is elided, a contract check is
  /// compiled away.
  proved,
  /// The facts entail the goal's *negation* — the code is not merely
  /// unproven, it is wrong on every execution that reaches it. Always an
  /// error; saying so is the compiler's job.
  refuted,
  /// Neither. The graceful path: report what was needed and what was known,
  /// and point at the two ways out (`spec/dependent-types-design.md` §6).
  unknown,
};

/// Decides `facts |- goal` in the linear fragment.
///
/// Sound in both directions and total in every case. `proved` and `refuted`
/// are established by showing a system of linear constraints has no solution
/// over the *rationals*, which implies it has none over the integers — so
/// neither answer is ever wrong. The reverse implication does not hold, which
/// is precisely where `unknown` comes from.
[[nodiscard]] auto solve(std::span<const constraint> facts,
                         const goal_form &goal) -> proof_result;

/// The most facts an obligation may mention before the solver declines to
/// decide it and answers `unknown`. Fourier-Motzkin elimination is
/// exponential in the number of variables in the worst case; a limit the user
/// can hit is better than a compiler that stops responding, and the escape
/// hatch (`try_from`) is always available.
inline constexpr size_t k_atom_limit = 16;
/// The same guard, on the intermediate constraint count elimination produces.
inline constexpr size_t k_constraint_limit = 512;

} // namespace kira::semantic
