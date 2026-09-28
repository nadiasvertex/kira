// Tests for value solving.
//
// The behaviour under test is the one ch. 33 records as missing: "the
// compiler does not *solve for* `n` and propagate it". These assert that it
// now does, that it refuses exactly the equations that have no solution, and
// that it declines to guess at the ones that do not determine an answer.

#include <iostream>
#include <string_view>

#include "src/semantic/infer/value_solver.h"
#include "src/semantic/linear_poly.h"
#include "src/testing/test_assert.h"

namespace {

using cinder::semantic::linear_poly;
using cinder::semantic::poly_add;
using cinder::semantic::poly_constant;
using cinder::semantic::poly_scale;
using cinder::semantic::poly_sub;
using cinder::semantic::poly_variable;
using cinder::semantic::infer::flexible_vars;
using cinder::semantic::infer::value_answer;
using cinder::semantic::infer::value_solution;
using cinder::testing::expect;

auto n_plus(int64_t k) -> linear_poly {
  return poly_add(poly_variable("n"), poly_constant(k));
}

/// Every variable is an unknown — the one-sided equations below have no
/// caller to hold anything fixed.
auto solve_value_equation(const linear_poly &a, const linear_poly &b,
                          bool unsigned_domain) -> value_solution {
  return cinder::semantic::infer::solve_value_equation(
      a, b, unsigned_domain, [](std::string_view) -> bool { return true; });
}

/// Only `n` is an unknown; every other variable is rigid.
auto solve_for_n(const linear_poly &a, const linear_poly &b) -> value_solution {
  const flexible_vars only_n = [](std::string_view var) -> bool {
    return var == "n";
  };
  return cinder::semantic::infer::solve_value_equation(a, b, true, only_n);
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

/// A rigid variable is a fixed quantity the answer may be written in, not a
/// second unknown: `n + 1 = m + 1` solves `n := m` rather than stalling.
auto test_rigid_variables_are_held_fixed() -> void {
  const auto m = poly_variable("m");
  const auto shifted = solve_for_n(n_plus(1), poly_add(m, poly_constant(1)));
  expect(shifted.answer == value_answer::solved && shifted.var == "n" &&
             shifted.value == m,
         "expected `n + 1 = m + 1` to solve `n := m`");

  // The answer may be any polynomial over the rigid variables, and one that
  // is not provably non-negative is left for each instance to check.
  const auto minus = solve_for_n(n_plus(1), m);
  expect(minus.answer == value_answer::solved &&
             minus.value == poly_sub(m, poly_constant(1)),
         "expected `n + 1 = m` to solve `n := m - 1`");

  // Rigid on both sides and different: no choice of anything makes `m` and
  // `m + 1` the same value.
  const auto rigid = solve_for_n(m, poly_add(m, poly_constant(1)));
  expect(rigid.answer == value_answer::unsatisfiable && !rigid.detail.empty(),
         "expected `m = m + 1` with `m` rigid to be refused");

  // `2n = m` has a whole-number solution for some `m` but not for every one.
  const auto halved = solve_for_n(poly_scale(poly_variable("n"), 2), m);
  expect(halved.answer == value_answer::unsatisfiable,
         "expected `2n = m` to be refused for a rigid `m`");

  // A known value turns the underdetermined `m + n = 5` into `3 + n = 5` —
  // exactly what an explicit `split[3](a)` supplies.
  const auto split = solve_for_n(n_plus(3), poly_constant(5));
  expect(split.answer == value_answer::solved &&
             split.value == poly_constant(2),
         "expected `3 + n = 5` to solve `n := 2`");
}

} // namespace

auto main() -> int {
  test_solves_for_one_unknown();
  test_unsigned_domain_refutes();
  test_gcd_criterion();
  test_underdetermined_never_guesses();
  test_rigid_variables_are_held_fixed();
  std::cout << "value_solver_test passed\n";
  return 0;
}
