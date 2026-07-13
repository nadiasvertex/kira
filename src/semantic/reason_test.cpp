#include <format>
#include <string>
#include <vector>

#include "src/semantic/linear_poly.h"
#include "src/semantic/reason.h"
#include "src/testing/test_assert.h"

namespace {

using kira::semantic::constraint;
using kira::semantic::fact_set;
using kira::semantic::goal_form;
using kira::semantic::linear_poly;
using kira::semantic::poly_add;
using kira::semantic::poly_constant;
using kira::semantic::poly_negate;
using kira::semantic::poly_scale;
using kira::semantic::poly_sub;
using kira::semantic::poly_variable;
using kira::semantic::proof_result;
using kira::semantic::relation;
using kira::semantic::solve;
using kira::testing::expect;

/// `poly >= 0`.
auto at_least_zero(linear_poly poly) -> constraint {
  return constraint{.poly = std::move(poly), .rel = relation::ge, .label = {}};
}

/// `poly == 0`.
auto equals_zero(linear_poly poly) -> constraint {
  return constraint{.poly = std::move(poly), .rel = relation::eq, .label = {}};
}

/// A goal of one alternative holding one constraint — the common shape.
auto single(constraint item) -> goal_form {
  return goal_form{fact_set{std::move(item)}};
}

/// The variable `name` minus `value`, i.e. the polynomial behind `name >=
/// value` once written as `poly >= 0`.
auto var_minus(const std::string &name, int64_t value) -> linear_poly {
  return poly_sub(poly_variable(name), poly_constant(value));
}

// ==========================================================================
//  Canonicalization
//
//  The type table interns a symbolic value on its polynomial's `key()`, so
//  two spellings of the same value *must* produce the same key or id-equality
//  silently splits one type into two. This is the invariant that risk item 2
//  of the implementation plan is about, so it is tested directly rather than
//  inferred from the checker's behavior.
// ==========================================================================

auto test_commuted_polynomials_are_identical() -> void {
  const auto m_plus_n = poly_add(poly_variable("m"), poly_variable("n"));
  const auto n_plus_m = poly_add(poly_variable("n"), poly_variable("m"));
  expect(m_plus_n == n_plus_m, "m + n and n + m must be the same polynomial");
  expect(m_plus_n.key() == n_plus_m.key(), "commuted sums must intern alike");
}

auto test_reassociated_polynomials_are_identical() -> void {
  // (n + 1) + m  vs  n + (m + 1)
  const auto left = poly_add(poly_add(poly_variable("n"), poly_constant(1)),
                             poly_variable("m"));
  const auto right = poly_add(poly_variable("n"),
                              poly_add(poly_variable("m"), poly_constant(1)));
  expect(left.key() == right.key(), "reassociated sums must intern alike");
}

auto test_like_terms_combine_and_cancel() -> void {
  const auto two_n = poly_add(poly_variable("n"), poly_variable("n"));
  expect(two_n.coefficient_of("n") == 2, "n + n must have coefficient 2");
  expect(two_n.key() == poly_scale(poly_variable("n"), 2).key(),
         "n + n and 2*n must intern alike");

  const auto cancelled = poly_sub(poly_variable("n"), poly_variable("n"));
  expect(cancelled.is_constant(), "n - n must have no terms left");
  expect(cancelled.constant == 0, "n - n must be zero");
}

auto test_display_reads_like_source() -> void {
  expect(poly_add(poly_variable("n"), poly_constant(1)).display() == "n + 1",
         "n + 1 should display as written");
  expect(
      poly_sub(poly_scale(poly_variable("m"), 2), poly_constant(3)).display() ==
          "2*m - 3",
      "2*m - 3 should display as written");
  expect(poly_constant(0).display() == "0", "a constant displays as itself");
}

// ==========================================================================
//  The decision procedure
// ==========================================================================

auto test_proves_from_a_matching_fact() -> void {
  // n >= 0  |-  n >= 0
  const auto facts = fact_set{at_least_zero(poly_variable("n"))};
  expect(solve(facts, single(at_least_zero(poly_variable("n")))) ==
             proof_result::proved,
         "a fact must discharge itself");
}

auto test_proves_by_transitivity() -> void {
  // i < n  and  n <= 10   |-   i < 10
  // as polynomials: n - i - 1 >= 0, 10 - n >= 0  |-  10 - i - 1 >= 0
  const auto facts = fact_set{
      at_least_zero(poly_sub(poly_sub(poly_variable("n"), poly_variable("i")),
                             poly_constant(1))),
      at_least_zero(poly_sub(poly_constant(10), poly_variable("n"))),
  };
  const auto goal =
      single(at_least_zero(poly_sub(poly_constant(9), poly_variable("i"))));
  expect(solve(facts, goal) == proof_result::proved,
         "the solver must chain two bounds transitively");
}

auto test_proves_index_in_bounds() -> void {
  // The load-bearing case: `v[i]` where `i: index[n]` and `v: array[T, n]`.
  // facts:  i < n  (the refinement),  len(v) = n  (the array's length)
  // goal:   i < len(v)   =>   len(v) - i - 1 >= 0
  const auto facts = fact_set{
      at_least_zero(poly_sub(poly_sub(poly_variable("n"), poly_variable("i")),
                             poly_constant(1))),
      equals_zero(poly_sub(poly_variable("len(v)"), poly_variable("n"))),
  };
  const auto goal = single(at_least_zero(
      poly_sub(poly_sub(poly_variable("len(v)"), poly_variable("i")),
               poly_constant(1))));
  expect(solve(facts, goal) == proof_result::proved,
         "a refined index into an array of matching length must be provable");
}

auto test_refutes_an_impossible_goal() -> void {
  // n >= 0  |-  n < 0  is refuted, not merely unproven.
  const auto facts = fact_set{at_least_zero(poly_variable("n"))};
  const auto goal = single(at_least_zero(
      poly_sub(poly_negate(poly_variable("n")), poly_constant(1))));
  expect(solve(facts, goal) == proof_result::refuted,
         "a goal contradicting the facts must be refuted, not unknown");
}

auto test_unknown_when_nothing_is_known() -> void {
  const auto facts = fact_set{};
  expect(solve(facts, single(at_least_zero(var_minus("i", 0)))) ==
             proof_result::unknown,
         "an unconstrained value must be neither proved nor refuted");
}

auto test_integer_tightening_sharpens_a_bound() -> void {
  // `2n >= 1` gives only `n >= 0.5` over the rationals, which does not entail
  // `n >= 1`. Over the integers it does, and the gcd tightening in
  // `reason.cpp` is what sees that. Without it this comes back `unknown`.
  const auto facts = fact_set{at_least_zero(
      poly_sub(poly_scale(poly_variable("n"), 2), poly_constant(1)))};
  const auto goal = single(at_least_zero(var_minus("n", 1)));
  expect(solve(facts, goal) == proof_result::proved,
         "integer tightening must sharpen 2n >= 1 into n >= 1");
}

auto test_irrelevant_facts_do_not_count_against_the_limit() -> void {
  // A real function's fact environment is mostly noise for any one
  // obligation. Facts unconnected to the goal are dropped before the size
  // limit is applied, so a provable goal stays provable in a big scope.
  auto facts = fact_set{at_least_zero(var_minus("i", 0))};
  for (size_t i = 0; i < kira::semantic::k_atom_limit + 8; ++i) {
    facts.push_back(at_least_zero(var_minus(std::format("unrelated{}", i), 0)));
  }
  expect(solve(facts, single(at_least_zero(var_minus("i", 0)))) ==
             proof_result::proved,
         "unrelated facts must not push a simple goal past the atom limit");
}

auto test_disjunctive_goal_proved_by_either_side() -> void {
  // facts: n >= 5.  goal: n < 0  or  n >= 3.
  const auto facts = fact_set{at_least_zero(var_minus("n", 5))};
  const auto goal = goal_form{
      fact_set{at_least_zero(
          poly_sub(poly_negate(poly_variable("n")), poly_constant(1)))},
      fact_set{at_least_zero(var_minus("n", 3))},
  };
  expect(solve(facts, goal) == proof_result::proved,
         "a disjunctive goal is discharged by proving either disjunct");
}

auto test_conjunctive_goal_needs_both_sides() -> void {
  // facts: n >= 5.  goal: n >= 3 and n >= 100 — only half of which follows.
  const auto facts = fact_set{at_least_zero(var_minus("n", 5))};
  const auto goal = goal_form{fact_set{
      at_least_zero(var_minus("n", 3)),
      at_least_zero(var_minus("n", 100)),
  }};
  expect(solve(facts, goal) == proof_result::unknown,
         "a conjunctive goal is not discharged by proving only one conjunct");
}

auto test_uninterpreted_atoms_are_opaque_but_equal_to_themselves() -> void {
  // A pure call `f(x)` is an atom: nothing is known about it except that it
  // equals itself. So `f(x) > 0 |- f(x) > 0` proves, and `f(x) > 0 |- f(y) >
  // 0` does not.
  const auto facts = fact_set{at_least_zero(var_minus("f(x)", 1))};
  expect(solve(facts, single(at_least_zero(var_minus("f(x)", 1)))) ==
             proof_result::proved,
         "an uninterpreted call equals itself");
  expect(solve(facts, single(at_least_zero(var_minus("f(y)", 1)))) ==
             proof_result::unknown,
         "an uninterpreted call says nothing about a different one");
}

auto test_too_many_atoms_declines_rather_than_blowing_up() -> void {
  // Past the fragment's size limit the solver answers `unknown` — it does not
  // search harder, and it does not hang. Built as a *connected* chain
  // (v0 <= v1 <= ... <= vN) so relevance filtering can't shrink it.
  constexpr auto k_length = kira::semantic::k_atom_limit + 4;
  auto facts = fact_set{};
  for (size_t i = 0; i + 1 < k_length; ++i) {
    facts.push_back(
        at_least_zero(poly_sub(poly_variable(std::format("v{}", i + 1)),
                               poly_variable(std::format("v{}", i)))));
  }
  const auto goal = single(at_least_zero(poly_sub(
      poly_variable(std::format("v{}", k_length - 1)), poly_variable("v0"))));
  expect(solve(facts, goal) == proof_result::unknown,
         "an oversized system must decline, not decide");
}

} // namespace

auto main() -> int {
  test_commuted_polynomials_are_identical();
  test_reassociated_polynomials_are_identical();
  test_like_terms_combine_and_cancel();
  test_display_reads_like_source();
  test_proves_from_a_matching_fact();
  test_proves_by_transitivity();
  test_proves_index_in_bounds();
  test_refutes_an_impossible_goal();
  test_unknown_when_nothing_is_known();
  test_integer_tightening_sharpens_a_bound();
  test_irrelevant_facts_do_not_count_against_the_limit();
  test_disjunctive_goal_proved_by_either_side();
  test_conjunctive_goal_needs_both_sides();
  test_uninterpreted_atoms_are_opaque_but_equal_to_themselves();
  test_too_many_atoms_declines_rather_than_blowing_up();
  return 0;
}
