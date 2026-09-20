// Tests for value solving (`spec/inference-rewrite.md` phase 3).
//
// The behaviour under test is the one ch. 33 records as missing: "the
// compiler does not *solve for* `n` and propagate it". These assert that it
// now does, that it refuses exactly the equations that have no solution, and
// that it declines to guess at the ones that do not determine an answer.

#include <iostream>

#include "src/semantic/infer/value_solver.h"
#include "src/semantic/linear_poly.h"
#include "src/testing/test_assert.h"

namespace {

using kira::semantic::linear_poly;
using kira::semantic::poly_add;
using kira::semantic::poly_constant;
using kira::semantic::poly_scale;
using kira::semantic::poly_sub;
using kira::semantic::poly_variable;
using kira::semantic::infer::solve_value_equation;
using kira::semantic::infer::value_answer;
using kira::testing::expect;

auto n_plus(int64_t k) -> linear_poly {
  return poly_add(poly_variable("n"), poly_constant(k));
}

/// The headline case: `n + 1 = 3` yields `n := 2`.
auto test_solves_for_one_unknown() -> void {
  const auto solution = solve_value_equation(n_plus(1), poly_constant(3), true);
  expect(solution.answer == value_answer::solved, "expected a solution");
  expect(solution.var == "n", "expected `n` to be the unknown solved");
  expect(solution.value == poly_constant(2), "expected `n := 2`");

  // Direction does not matter: an equation has no sides.
  const auto flipped = solve_value_equation(poly_constant(3), n_plus(1), true);
  expect(flipped.answer == value_answer::solved && flipped.var == "n" &&
             flipped.value == poly_constant(2),
         "expected the same answer with the sides swapped");

  // A non-unit coefficient divides exactly: `2n = 6`.
  const auto scaled = solve_value_equation(poly_scale(poly_variable("n"), 2),
                                           poly_constant(6), true);
  expect(scaled.answer == value_answer::solved &&
             scaled.value == poly_constant(3),
         "expected `2n = 6` to solve to 3");

  // And it works on both sides at once: `n + 2 = m` where `m` is gone.
  const auto both = solve_value_equation(n_plus(2), n_plus(2), true);
  expect(both.answer == value_answer::agreed,
         "expected identical sides to agree without solving anything");
}

/// The unsigned domain is what refutes `n + 1 = 0`, and so what rejects
/// `head` on an empty vector.
auto test_unsigned_domain_refutes() -> void {
  const auto refused = solve_value_equation(n_plus(1), poly_constant(0), true);
  expect(refused.answer == value_answer::unsatisfiable,
         "expected `n + 1 = 0` to have no unsigned solution");
  expect(!refused.detail.empty(), "expected an explanation to report");

  // Over a signed slot the same equation is perfectly solvable.
  const auto signed_solution =
      solve_value_equation(n_plus(1), poly_constant(0), false);
  expect(signed_solution.answer == value_answer::solved &&
             signed_solution.value == poly_constant(-1),
         "expected `n + 1 = 0` to solve to -1 over a signed slot");
}

/// The gcd criterion is complete for integer solvability, and catches an
/// equation no amount of sign reasoning would.
auto test_gcd_criterion() -> void {
  // `2n = 5` has no whole-number solution.
  const auto refused = solve_value_equation(poly_scale(poly_variable("n"), 2),
                                            poly_constant(5), false);
  expect(refused.answer == value_answer::unsatisfiable,
         "expected `2n = 5` to be refused");

  // `2m + 4n = 5` likewise, with two unknowns and no sign argument
  // available — gcd(2, 4) = 2 does not divide 5.
  const auto two_unknowns =
      solve_value_equation(poly_add(poly_scale(poly_variable("m"), 2),
                                    poly_scale(poly_variable("n"), 4)),
                           poly_constant(5), false);
  expect(two_unknowns.answer == value_answer::unsatisfiable,
         "expected the gcd criterion to refute a two-unknown equation");
}

/// An equation that does not determine an answer must not invent one.
auto test_underdetermined_never_guesses() -> void {
  const auto split = solve_value_equation(
      poly_add(poly_variable("m"), poly_variable("n")), poly_constant(5), true);
  expect(split.answer == value_answer::underdetermined,
         "expected `m + n = 5` to determine neither unknown");
  expect(split.var.empty(), "expected nothing to have been invented");
}

} // namespace

auto main() -> int {
  test_solves_for_one_unknown();
  test_unsigned_domain_refutes();
  test_gcd_criterion();
  test_underdetermined_never_guesses();
  std::cout << "value_solver_test passed\n";
  return 0;
}
