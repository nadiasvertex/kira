#include "linear_poly.h"

#include <algorithm>
#include <format>
#include <numeric>

namespace kira::semantic {
namespace {

/// Re-establishes the canonical form after an arbitrary term list has been
/// built: sort by variable, sum duplicates, drop zero coefficients. Every
/// public combinator funnels through here, so no caller can produce a
/// non-canonical polynomial.
auto canonicalize(std::vector<poly_term> terms) -> std::vector<poly_term> {
  std::ranges::sort(terms, [](const poly_term &a, const poly_term &b) -> bool {
    return a.var < b.var;
  });
  auto out = std::vector<poly_term>{};
  for (auto &term : terms) {
    if (!out.empty() && out.back().var == term.var) {
      out.back().coeff += term.coeff;
      continue;
    }
    out.push_back(std::move(term));
  }
  std::erase_if(out,
                [](const poly_term &term) -> bool { return term.coeff == 0; });
  return out;
}

} // namespace

auto linear_poly::coefficient_of(std::string_view var) const -> int64_t {
  for (const auto &term : terms) {
    if (term.var == var) {
      return term.coeff;
    }
  }
  return 0;
}

auto linear_poly::key() const -> std::string {
  auto out = std::to_string(constant);
  for (const auto &term : terms) {
    out += std::format("+{}*{}", term.coeff, term.var);
  }
  return out;
}

auto linear_poly::display() const -> std::string {
  if (terms.empty()) {
    return std::to_string(constant);
  }
  auto out = std::string{};
  for (const auto &term : terms) {
    const auto magnitude = term.coeff < 0 ? -term.coeff : term.coeff;
    if (out.empty()) {
      if (term.coeff < 0) {
        out += '-';
      }
    } else {
      out += term.coeff < 0 ? " - " : " + ";
    }
    if (magnitude != 1) {
      out += std::format("{}*", magnitude);
    }
    out += term.var;
  }
  if (constant > 0) {
    out += std::format(" + {}", constant);
  } else if (constant < 0) {
    out += std::format(" - {}", -constant);
  }
  return out;
}

auto poly_constant(int64_t value) -> linear_poly {
  return linear_poly{.terms = {}, .constant = value};
}

auto poly_variable(std::string name) -> linear_poly {
  return linear_poly{
      .terms = {poly_term{.coeff = 1, .var = std::move(name)}},
      .constant = 0,
  };
}

auto poly_add(const linear_poly &a, const linear_poly &b) -> linear_poly {
  auto terms = a.terms;
  terms.insert(terms.end(), b.terms.begin(), b.terms.end());
  return linear_poly{
      .terms = canonicalize(std::move(terms)),
      .constant = a.constant + b.constant,
  };
}

auto poly_negate(const linear_poly &a) -> linear_poly {
  auto result = a;
  for (auto &term : result.terms) {
    term.coeff = -term.coeff;
  }
  result.constant = -result.constant;
  return result;
}

auto poly_sub(const linear_poly &a, const linear_poly &b) -> linear_poly {
  return poly_add(a, poly_negate(b));
}

auto poly_scale(const linear_poly &a, int64_t factor) -> linear_poly {
  if (factor == 0) {
    return poly_constant(0);
  }
  auto result = a;
  for (auto &term : result.terms) {
    term.coeff *= factor;
  }
  result.constant *= factor;
  return result;
}

auto poly_substitute(
    const linear_poly &poly,
    const std::unordered_map<std::string, linear_poly> &bindings)
    -> linear_poly {
  auto result = poly_constant(poly.constant);
  for (const auto &term : poly.terms) {
    const auto it = bindings.find(term.var);
    const auto replacement =
        it != bindings.end() ? it->second : poly_variable(term.var);
    result = poly_add(result, poly_scale(replacement, term.coeff));
  }
  return result;
}

/// The pattern must be `±v + c` — one unknown, coefficient ±1 — for the
/// equation `pattern = value` to name `v` uniquely without dividing. Anything
/// wider (two unknowns, a coefficient of 2) is left unsolved on purpose; see
/// the header.
auto solve_for_unknown(const linear_poly &pattern, const linear_poly &value)
    -> std::optional<std::pair<std::string, linear_poly>> {
  if (pattern.terms.size() != 1) {
    return std::nullopt;
  }
  const auto &term = pattern.terms.front();
  if (term.coeff != 1 && term.coeff != -1) {
    return std::nullopt;
  }
  // pattern = value  =>  coeff * v = value - constant  =>  v = ±(value - c)
  auto solved = poly_sub(value, poly_constant(pattern.constant));
  if (term.coeff == -1) {
    solved = poly_negate(solved);
  }
  if (solved.coefficient_of(term.var) != 0) {
    return std::nullopt; // self-referential; nothing determined
  }
  return std::pair{term.var, solved};
}

/// Reduces `a = b` to `sum(coeff_i * var_i) = target` and asks whether that
/// has a solution. Three refutations, in increasing specificity:
///
///  1. No unknowns at all: the constants must simply be equal.
///  2. The gcd of the coefficients must divide `target` — otherwise no
///     *integer* assignment can reach it (`2n = 3`).
///  3. With unsigned unknowns, a sum whose coefficients all point the same
///     way can never cross zero in the other direction (`n + 1 = 0`).
///
/// Anything surviving all three is reported satisfiable; see the header on
/// why the remaining incompleteness is the safe direction to be wrong in.
auto equation_satisfiable(const linear_poly &a, const linear_poly &b,
                          bool vars_non_negative) -> bool {
  const auto difference = poly_sub(a, b);
  if (difference.is_constant()) {
    return difference.constant == 0;
  }

  const auto target = -difference.constant;
  auto divisor = int64_t{0};
  auto all_positive = true;
  auto all_negative = true;
  for (const auto &term : difference.terms) {
    divisor = std::gcd(divisor, term.coeff);
    all_positive = all_positive && term.coeff > 0;
    all_negative = all_negative && term.coeff < 0;
  }
  if (divisor != 0 && target % divisor != 0) {
    return false;
  }
  if (vars_non_negative) {
    if (all_positive && target < 0) {
      return false;
    }
    if (all_negative && target > 0) {
      return false;
    }
  }
  return true;
}

} // namespace kira::semantic
