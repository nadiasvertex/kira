#include "reason.h"

#include <algorithm>
#include <numeric>
#include <set>
#include <string>

namespace kira::semantic {
namespace {

/// The solver's internal form: a conjunction of `poly >= 0`. Everything
/// reduces to this — an equality becomes two of them, a strict inequality
/// becomes one with the constant shifted (integers, so `p > 0` is `p - 1 >=
/// 0`), and a disequality becomes a two-way case split *before* it gets here.
/// One shape means one decision procedure.
using system = std::vector<linear_poly>;

/// Divides a constraint through by the gcd of its variable coefficients,
/// rounding the constant *inward*. Sound because `sum(ci * vi)` is always a
/// multiple of `g = gcd(ci)`, so `sum(ci*vi) + c0 >= 0` says `g*k >= -c0`,
/// which for integer `k` is exactly `k >= ceil(-c0 / g)` — a strictly
/// stronger statement than the rational relaxation gives.
///
/// This is what lets the solver refute `2n = 1`-shaped systems that a purely
/// rational Fourier-Motzkin would call satisfiable, and it is the whole of
/// the "integer tightening" the design doc promises.
auto tighten(linear_poly poly) -> linear_poly {
  if (poly.terms.empty()) {
    return poly;
  }
  auto divisor = int64_t{0};
  for (const auto &term : poly.terms) {
    divisor = std::gcd(divisor, term.coeff);
  }
  if (divisor <= 1) {
    return poly;
  }
  for (auto &term : poly.terms) {
    term.coeff /= divisor;
  }
  // Floor division toward negative infinity, which `/` does not do for
  // negative numerators in C++.
  const auto quotient = poly.constant >= 0
                            ? poly.constant / divisor
                            : -((-poly.constant + divisor - 1) / divisor);
  poly.constant = quotient;
  return poly;
}

/// Whether a system of `poly >= 0` constraints has no solution, decided by
/// Fourier-Motzkin elimination: repeatedly pick a variable, pair every
/// constraint that bounds it from below with every one that bounds it from
/// above, and replace both with the combination that eliminates it. What
/// remains at the end is constant; the system is unsatisfiable exactly when
/// one of those constants is negative.
///
/// Answers `false` (i.e. "not provably unsatisfiable") whenever the problem
/// exceeds the fragment's size limits, which keeps the procedure total. A
/// `false` here always widens to `proof_result::unknown` upstream, never to a
/// wrong acceptance.
auto unsatisfiable(system constraints) -> bool {
  auto variables = std::set<std::string>{};
  for (const auto &poly : constraints) {
    for (const auto &term : poly.terms) {
      variables.insert(term.var);
    }
  }
  if (variables.size() > k_atom_limit) {
    return false;
  }

  for (const auto &variable : variables) {
    auto lower = system{}; // constraints with a positive coefficient on it
    auto upper = system{}; // ... with a negative one
    auto rest = system{};
    for (auto &poly : constraints) {
      const auto coeff = poly.coefficient_of(variable);
      if (coeff > 0) {
        lower.push_back(std::move(poly));
      } else if (coeff < 0) {
        upper.push_back(std::move(poly));
      } else {
        rest.push_back(std::move(poly));
      }
    }

    if (lower.size() * upper.size() + rest.size() > k_constraint_limit) {
      return false;
    }

    for (const auto &low : lower) {
      const auto low_coeff = low.coefficient_of(variable);
      for (const auto &high : upper) {
        const auto high_coeff = high.coefficient_of(variable);
        // Scale each side so the variable's coefficients cancel exactly, then
        // add: the sum is implied by both, and mentions the variable no more.
        auto combined =
            poly_add(poly_scale(low, -high_coeff), poly_scale(high, low_coeff));
        rest.push_back(tighten(std::move(combined)));
      }
    }
    constraints = std::move(rest);

    // An unsatisfiable constant can appear at any round; stopping early keeps
    // a refuted system from paying for the remaining eliminations.
    for (const auto &poly : constraints) {
      if (poly.is_constant() && poly.constant < 0) {
        return true;
      }
    }
  }

  return std::ranges::any_of(constraints, [](const linear_poly &poly) -> bool {
    return poly.is_constant() && poly.constant < 0;
  });
}

/// Lowers a conjunction of `constraint`s into the solver's `poly >= 0` form.
///
/// A disequality among the *facts* is simply dropped. That is a deliberate
/// weakening: forgetting a fact can only make the solver prove less, never
/// prove something false — and a disequality is a disjunction, whose only
/// exact handling is a case split that would double the work for a fact that
/// almost never carries a proof on its own.
auto lower_to_system(std::span<const constraint> conjunction) -> system {
  auto result = system{};
  for (const auto &item : conjunction) {
    switch (item.rel) {
    case relation::ge:
      result.push_back(tighten(item.poly));
      break;
    case relation::eq:
      result.push_back(tighten(item.poly));
      result.push_back(tighten(poly_negate(item.poly)));
      break;
    case relation::ne:
      break;
    }
  }
  return result;
}

/// The negation of one constraint, as a *disjunction* of solver-form systems
/// — each of which must be refuted for the original to be entailed.
///
/// `not (p >= 0)` is `p <= -1`, a single system. `not (p == 0)` is
/// `p >= 1 or p <= -1`, two — the only place the solver ever branches, and it
/// branches exactly twice.
auto negate(const constraint &item) -> std::vector<system> {
  switch (item.rel) {
  case relation::ge: {
    // p <= -1  <=>  -p - 1 >= 0
    auto negated = poly_negate(item.poly);
    negated.constant -= 1;
    return {system{tighten(std::move(negated))}};
  }
  case relation::ne:
    // not (p != 0)  <=>  p == 0
    return {system{tighten(item.poly), tighten(poly_negate(item.poly))}};
  case relation::eq: {
    // p >= 1
    auto above = item.poly;
    above.constant -= 1;
    // p <= -1
    auto below = poly_negate(item.poly);
    below.constant -= 1;
    return {system{tighten(std::move(above))},
            system{tighten(std::move(below))}};
  }
  }
  return {};
}

/// Whether `facts` entail `goal`: true exactly when assuming the facts *and*
/// the goal's negation is contradictory, in every branch the negation opens.
auto entails(const system &facts, const constraint &goal) -> bool {
  for (auto &branch : negate(goal)) {
    auto combined = facts;
    combined.insert(combined.end(), branch.begin(), branch.end());
    if (!unsatisfiable(std::move(combined))) {
      return false;
    }
  }
  return true;
}

/// Every variable mentioned anywhere in `goal`.
auto goal_variables(const goal_form &goal) -> std::set<std::string> {
  auto variables = std::set<std::string>{};
  for (const auto &alternative : goal) {
    for (const auto &item : alternative) {
      for (const auto &term : item.poly.terms) {
        variables.insert(term.var);
      }
    }
  }
  return variables;
}

/// Keeps only the facts that could possibly bear on the goal: those sharing a
/// variable with it, then those sharing a variable with *those*, and so on to
/// a fixed point.
///
/// This is not an optimization — it is what makes the atom limit livable. A
/// function's fact environment accumulates every parameter's refinement,
/// every value-parameter domain, and every path condition, and would blow
/// past `k_atom_limit` in any real body; but a single obligation almost
/// always turns on two or three of them. Dropping the rest is sound for the
/// same reason dropping a disequality is: fewer facts can only prove less.
auto relevant_facts(std::span<const constraint> facts, const goal_form &goal)
    -> std::vector<constraint> {
  auto reachable = goal_variables(goal);
  auto kept = std::vector<bool>(facts.size(), false);

  for (auto growing = true; growing;) {
    growing = false;
    for (size_t i = 0; i < facts.size(); ++i) {
      if (kept[i]) {
        continue;
      }
      const auto touches_goal = std::ranges::any_of(
          facts[i].poly.terms, [&](const poly_term &term) -> bool {
            return reachable.contains(term.var);
          });
      if (!touches_goal) {
        continue;
      }
      kept[i] = true;
      growing = true;
      for (const auto &term : facts[i].poly.terms) {
        reachable.insert(term.var);
      }
    }
  }

  auto result = std::vector<constraint>{};
  for (size_t i = 0; i < facts.size(); ++i) {
    if (kept[i]) {
      result.push_back(facts[i]);
    }
  }
  return result;
}

} // namespace

/// `proved` when some alternative of the goal is entailed constraint by
/// constraint; `refuted` when *every* alternative contradicts the facts
/// outright (so no execution reaching here could satisfy the goal); `unknown`
/// in the gap between, which is where the language's escape hatches live.
auto solve(std::span<const constraint> facts, const goal_form &goal)
    -> proof_result {
  if (goal.empty()) {
    return proof_result::unknown;
  }

  const auto narrowed = relevant_facts(facts, goal);
  const auto fact_system = lower_to_system(narrowed);

  for (const auto &alternative : goal) {
    const auto all_entailed =
        !alternative.empty() &&
        std::ranges::all_of(alternative, [&](const constraint &item) -> bool {
          return entails(fact_system, item);
        });
    if (all_entailed) {
      return proof_result::proved;
    }
  }

  const auto every_alternative_impossible =
      std::ranges::all_of(goal, [&](const fact_set &alternative) -> bool {
        auto combined = fact_system;
        const auto lowered = lower_to_system(alternative);
        combined.insert(combined.end(), lowered.begin(), lowered.end());
        return unsatisfiable(std::move(combined));
      });
  if (every_alternative_impossible) {
    return proof_result::refuted;
  }

  return proof_result::unknown;
}

} // namespace kira::semantic
