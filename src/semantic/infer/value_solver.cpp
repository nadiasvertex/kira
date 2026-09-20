#include "src/semantic/infer/value_solver.h"

#include <cstdlib>
#include <format>
#include <numeric>

namespace kira::semantic::infer {

auto solve_value_equation(const linear_poly &a, const linear_poly &b,
                          bool unsigned_domain) -> value_solution {
  // Everything below reasons about one polynomial: `a - b = 0`.
  const auto residual = poly_sub(a, b);

  if (residual.terms.empty()) {
    if (residual.constant == 0) {
      return value_solution{.answer = value_answer::agreed};
    }
    return value_solution{.answer = value_answer::unsatisfiable,
                          .detail =
                              std::format("`{}` and `{}` are different values",
                                          a.display(), b.display())};
  }

  // The complete integer criterion: `sum(c_i v_i) = -k` has a solution iff
  // gcd(c_i) divides k. This is what refutes `2n = 5` exactly, without
  // enumerating anything, and it holds however many unknowns there are.
  auto divisor = int64_t{0};
  for (const auto &term : residual.terms) {
    divisor = std::gcd(divisor, term.coeff);
  }
  if (divisor != 0 && residual.constant % divisor != 0) {
    return value_solution{
        .answer = value_answer::unsatisfiable,
        .detail = std::format(
            "`{} = {}` has no whole-number solution: every solution of the "
            "left side is a multiple of {} apart",
            a.display(), b.display(), std::abs(divisor))};
  }

  if (residual.terms.size() > 1) {
    // Satisfiable over the integers as far as the gcd can tell, but the
    // unsigned domain may still refute it — `m + n = -1` has no solution in
    // naturals even though gcd(1, 1) divides 1. That reasoning already
    // exists and is deliberately shared rather than reimplemented.
    if (!equation_satisfiable(a, b, unsigned_domain)) {
      return value_solution{
          .answer = value_answer::unsatisfiable,
          .detail = std::format("`{} = {}` has no solution for non-negative "
                                "values",
                                a.display(), b.display())};
    }
    return value_solution{.answer = value_answer::underdetermined};
  }

  // One unknown: `c * v + k = 0`, so `v = -k / c`, exact because the gcd
  // test above already established divisibility.
  const auto &term = residual.terms.front();
  const auto solved = -residual.constant / term.coeff;
  if (unsigned_domain && solved < 0) {
    return value_solution{
        .answer = value_answer::unsatisfiable,
        .detail = std::format("`{} = {}` would need `{}` to be {}, and it "
                              "cannot be negative",
                              a.display(), b.display(), term.var, solved)};
  }
  return value_solution{.answer = value_answer::solved,
                        .var = term.var,
                        .value = poly_constant(solved)};
}

} // namespace kira::semantic::infer
