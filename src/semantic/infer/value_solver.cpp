#include "src/semantic/infer/value_solver.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <numeric>
#include <vector>

namespace cinder::semantic::infer {

namespace {

/// The rigid variables of `poly`, spelled for a diagnostic: "`m`", or
/// "`m` and `n`".
auto rigid_names(const linear_poly &poly) -> std::string {
  auto names = std::vector<std::string>{};
  for (const auto &term : poly.terms) {
    auto name = std::format("`{}`", poly_var_spelling(term.var));
    if (std::ranges::find(names, name) == names.end()) {
      names.push_back(std::move(name));
    }
  }
  auto text = std::string{};
  for (size_t i = 0; i < names.size(); ++i) {
    if (i > 0) {
      text += i + 1 == names.size() ? " and " : ", ";
    }
    text += names[i];
  }
  return text;
}

} // namespace

auto solve_value_equation(const linear_poly &a, const linear_poly &b,
                          bool unsigned_domain,
                          const flexible_vars &is_flexible) -> value_solution {
  // Everything below reasons about one polynomial, `a - b = 0`, split into
  // the unknowns being solved for and everything else: `flex + rest = 0`,
  // where `rest` is a constant plus terms in the rigid variables.
  const auto residual = poly_sub(a, b);
  auto flex = std::vector<poly_term>{};
  auto rest = poly_constant(residual.constant);
  for (const auto &term : residual.terms) {
    if (is_flexible(term.var)) {
      flex.push_back(term);
    } else {
      rest.terms.push_back(term);
    }
  }

  if (flex.empty()) {
    if (rest.terms.empty() && rest.constant == 0) {
      return value_solution{.answer = value_answer::agreed};
    }
    if (rest.terms.empty()) {
      return value_solution{
          .answer = value_answer::unsatisfiable,
          .detail = std::format("`{}` and `{}` are different values",
                                a.display(), b.display())};
    }
    return value_solution{
        .answer = value_answer::unsatisfiable,
        .detail = std::format("`{}` and `{}` are not the same value for every "
                              "{}",
                              a.display(), b.display(), rigid_names(rest))};
  }

  // The complete integer criterion: `sum(c_i v_i) = -rest` has a solution
  // for every value of the rigid variables iff gcd(c_i) divides each of
  // `rest`'s coefficients. This is what refutes `2n = 5` exactly, without
  // enumerating anything, and it holds however many unknowns there are.
  auto divisor = int64_t{0};
  for (const auto &term : flex) {
    divisor = std::gcd(divisor, term.coeff);
  }
  const auto divides = [&](int64_t value) -> bool {
    return value % divisor == 0;
  };
  if (!divides(rest.constant) ||
      !std::ranges::all_of(rest.terms, divides, &poly_term::coeff)) {
    return value_solution{
        .answer = value_answer::unsatisfiable,
        .detail =
            rest.terms.empty()
                ? std::format("`{} = {}` has no whole-number solution: every "
                              "solution of the left side is a multiple of {} "
                              "apart",
                              a.display(), b.display(), std::abs(divisor))
                : std::format("`{} = {}` has no whole-number solution for "
                              "every {}",
                              a.display(), b.display(), rigid_names(rest))};
  }

  if (flex.size() > 1) {
    // Satisfiable over the integers as far as the gcd can tell, but the
    // unsigned domain may still refute it — `m + n = -1` has no solution in
    // naturals even though gcd(1, 1) divides 1. That reasoning already
    // exists and is deliberately shared rather than reimplemented; it only
    // applies when there is nothing rigid to reason about besides.
    if (rest.terms.empty() && !equation_satisfiable(a, b, unsigned_domain)) {
      return value_solution{
          .answer = value_answer::unsatisfiable,
          .detail = std::format("`{} = {}` has no solution for non-negative "
                                "values",
                                a.display(), b.display())};
    }
    return value_solution{.answer = value_answer::underdetermined};
  }

  // One unknown: `c * v + rest = 0`, so `v = -rest / c`, exact because the
  // gcd test above already established divisibility term by term.
  const auto &term = flex.front();
  auto solved = poly_constant(-rest.constant / term.coeff);
  for (const auto &other : rest.terms) {
    solved = poly_add(solved, poly_scale(poly_variable(other.var),
                                         -other.coeff / term.coeff));
  }
  if (unsigned_domain && solved.is_constant() && solved.constant < 0) {
    return value_solution{
        .answer = value_answer::unsatisfiable,
        .detail = std::format("`{} = {}` would need `{}` to be {}, and it "
                              "cannot be negative",
                              a.display(), b.display(),
                              poly_var_spelling(term.var), solved.constant)};
  }
  return value_solution{
      .answer = value_answer::solved, .var = term.var, .value = solved};
}

} // namespace cinder::semantic::infer
