#pragma once

#include <cstdint>
#include <string>

#include "src/semantic/linear_poly.h"

namespace cinder::semantic::infer {

// ==========================================================================
//  Value solving
//
//  Phase 3 of `spec/inference-rewrite.md`. Ch. 33 says outright that the
//  compiler "does not *solve for* `n` and propagate it" — an
//  unresolved-but-satisfiable value slot is merely carried along. That is
//  todo item 20 in the dependent fragment, and it is what produced
//  `solve_value_params`, `bind_generic_constant` and the const-generic
//  special cases: every consumer downstream of a value slot had to guess at
//  an `n` nobody had solved.
//
//  Unifying `vec[T, n + 1]` against `vec[T, 3]` should yield `n := 2`, and a
//  single linear equation over the integers is decided *and* solved exactly.
// ==========================================================================

/// What an equation between two value slots came to.
enum class value_answer : uint8_t {
  /// The two sides already denote the same value; nothing to record.
  agreed,
  /// Exactly one unknown, and it is now known: `var := value`.
  solved,
  /// Satisfiable, but the equation does not pin any single unknown down.
  /// `m + n = 5` determines neither `m` nor `n`, and inventing a split would
  /// be a guess — so the caller waits for another constraint instead.
  underdetermined,
  /// No solution exists, over the integers or (for an unsigned slot) over
  /// the non-negative integers. This is a real type error.
  unsatisfiable,
};

/// The answer, plus whatever it determined.
struct value_solution {
  value_answer answer = value_answer::underdetermined;
  /// For `solved`: the unknown's name, as it appears in the polynomial.
  std::string var;
  /// For `solved`: what it must equal. Constant in practice for a
  /// single-unknown equation, but carried as a polynomial so a later caller
  /// can substitute it without a special case.
  linear_poly value;
  /// For `unsatisfiable`: why, phrased for a diagnostic.
  std::string detail;
};

/// Solves `a = b` for the unknowns appearing in either side.
///
/// `unsigned_domain` says the unknowns range over an unsigned type, so every
/// one of them is `>= 0` — the fact that refutes `n + 1 = 0` and so rejects
/// `head` on an empty vector.
///
/// Deliberately stops at one unknown. The general two-unknown linear
/// Diophantine equation has a parametric family of solutions rather than an
/// answer, and ch. 33 already takes the position that a site which does not
/// pin a value down uniquely should leave it open rather than guess. What
/// extended Euclid *is* used for here is the exact refutation: the equation
/// has an integer solution only if the gcd of its coefficients divides its
/// constant, which is a complete criterion and catches `2n = 5` without
/// enumerating anything.
[[nodiscard]] auto solve_value_equation(const linear_poly &a,
                                        const linear_poly &b,
                                        bool unsigned_domain) -> value_solution;

} // namespace cinder::semantic::infer
