#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "src/semantic/linear_poly.h"

namespace cinder::semantic::infer {

// ==========================================================================
//  Value solving
//
//  The one place an equation between two value slots is decided and solved.
//  Unifying `vec[T, n + 1]` against `vec[T, 3]` yields `n := 2`, and a
//  single linear equation over the integers is decided *and* solved exactly.
//
//  Every value parameter reaches it through the unifier (`unify_values`):
//  inference's own unknowns directly, and a call's through `match_pattern`,
//  which is how the checker reads a callee's `n` off its arguments. There
//  used to be a second solver in the checker for the latter; it could not
//  tell the callee's unknowns from the caller's fixed values, and a value
//  solved one way on one path and another way on the other.
// ==========================================================================

/// Which of an equation's variables it may solve for.
///
/// An equation between two value slots is not symmetric in its variables: at
/// a call, the callee's `n` is an unknown to be found, while the caller's own
/// `m` is a fixed-but-unknown quantity the answer may be written in. Treating
/// both as unknowns is what left `n + 1 = m + 1` "underdetermined" instead of
/// solving `n := m`, and what let a store solve a generic body's own `n` as
/// though the body could choose it.
using flexible_vars = std::function<bool(std::string_view var)>;

/// What an equation between two value slots came to.
enum class value_answer : uint8_t {
  /// The two sides already denote the same value; nothing to record.
  agreed,
  /// Exactly one flexible unknown, and it is now known: `var := value`.
  solved,
  /// Satisfiable, but the equation does not pin any single unknown down.
  /// `m + n = 5` determines neither `m` nor `n`, and inventing a split would
  /// be a guess — so the caller waits for another constraint instead.
  underdetermined,
  /// No solution exists, over the integers or (for an unsigned slot) over
  /// the non-negative integers — or none that holds for every value of the
  /// rigid variables. This is a real type error.
  unsatisfiable,
};

/// The answer, plus whatever it determined.
struct value_solution {
  value_answer answer = value_answer::underdetermined;
  /// For `solved`: the unknown's name, as it appears in the polynomial.
  std::string var;
  /// For `solved`: what it must equal — a constant, or a polynomial over the
  /// rigid variables (`n := m + 1`).
  linear_poly value;
  /// For `unsatisfiable`: why, phrased for a diagnostic.
  std::string detail;
};

/// Solves `a = b` for its flexible variables, holding the rigid ones fixed.
///
/// A rigid variable is universally quantified: an answer must hold for every
/// value it could take. So `n = m + 1` with only `n` flexible solves to
/// `n := m + 1`, `n = n + 1` with `n` rigid is refused, and `2n = m` is
/// refused too — no whole-number `n` works for every `m`.
///
/// `unsigned_domain` says the unknowns range over an unsigned type, so every
/// one of them is `>= 0` — the fact that refutes `n + 1 = 0` and so rejects
/// `head` on an empty vector. It refutes a solution only when that solution
/// is a negative constant: `n := m - 1` is not provably non-negative, but
/// each instance of the generic body it appears in fixes `m`, and checks it
/// there.
///
/// Deliberately stops at one flexible unknown. The general two-unknown linear
/// Diophantine equation has a parametric family of solutions rather than an
/// answer, and ch. 33 already takes the position that a site which does not
/// pin a value down uniquely should leave it open rather than guess. What
/// extended Euclid *is* used for here is the exact refutation: the equation
/// has an integer solution only if the gcd of its flexible coefficients
/// divides everything else, which is a complete criterion and catches
/// `2n = 5` without enumerating anything.
[[nodiscard]] auto
solve_value_equation(const linear_poly &a, const linear_poly &b,
                     bool unsigned_domain, const flexible_vars &is_flexible)
    -> value_solution;

} // namespace cinder::semantic::infer
