#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kira::semantic {

// ==========================================================================
//  Canonical linear polynomials
//
//  The one representation of a compile-time integer value that isn't closed:
//  a linear combination of named unknowns plus a constant. Used twice, for
//  the same reason both times — a canonical form makes equality decidable by
//  comparison instead of by search:
//
//   * `type_table` interns a `symbolic_value_kind` type on a polynomial's
//     `key()`, so `vec[T, m + n]` and `vec[T, n + m]` are the *same*
//     `type_id` and the table's id-equality invariant survives dependent
//     types (`src/semantic/types.h`).
//   * the reasoning solver states every constraint as `poly RELOP 0`
//     (`src/semantic/reason.h`).
//
//  The fragment is deliberately small — see `spec/dependent-types-design.md`
//  §2, which this file implements. Nothing here multiplies two unknowns,
//  divides, or takes a remainder: those expressions leave the fragment
//  entirely rather than being approximated.
// ==========================================================================

/// One term of a canonical linear polynomial: `coeff * var`. A variable is
/// named, not numbered, because both users of this file already have stable
/// names for their unknowns — a value type-parameter (`n`) for the type
/// table, and a solver atom key (`n`, `v.len`, `f(x)`) for the solver.
struct poly_term {
  int64_t coeff = 0;
  std::string var;

  [[nodiscard]] auto operator==(const poly_term &) const -> bool = default;
};

/// A linear polynomial `constant + sum(coeff_i * var_i)` in canonical form:
/// terms sorted by `var`, no duplicate variables, and no zero coefficients.
/// Every constructor and combinator below re-establishes that form, so two
/// polynomials denote the same value *only if* they compare equal — which is
/// what lets `key()` serve as an intern key.
struct linear_poly {
  std::vector<poly_term> terms;
  int64_t constant = 0;

  [[nodiscard]] auto operator==(const linear_poly &) const -> bool = default;

  /// Whether this polynomial has no unknowns and is just its `constant`.
  [[nodiscard]] auto is_constant() const -> bool { return terms.empty(); }
  /// The coefficient of `var`, or `0` when it doesn't occur.
  [[nodiscard]] auto coefficient_of(std::string_view var) const -> int64_t;
  /// A stable, injective string encoding of the canonical form, suitable as
  /// a `type_table` intern key ("1*n+1*m" can never collide with "1*n").
  [[nodiscard]] auto key() const -> std::string;
  /// The user-facing spelling used in diagnostics (`n + 1`, `2*m - 3`) —
  /// deliberately close to what the user wrote, since a diagnostic that
  /// echoes the source is the whole point of reporting the arithmetic.
  [[nodiscard]] auto display() const -> std::string;
};

/// The constant polynomial `value`.
[[nodiscard]] auto poly_constant(int64_t value) -> linear_poly;
/// The polynomial `1 * name`.
[[nodiscard]] auto poly_variable(std::string name) -> linear_poly;
/// `a + b`, re-canonicalized.
[[nodiscard]] auto poly_add(const linear_poly &a, const linear_poly &b)
    -> linear_poly;
/// `a - b`, re-canonicalized.
[[nodiscard]] auto poly_sub(const linear_poly &a, const linear_poly &b)
    -> linear_poly;
/// `-a`.
[[nodiscard]] auto poly_negate(const linear_poly &a) -> linear_poly;
/// `factor * a` — the only multiplication in the fragment, since one side
/// must be a literal for the result to stay linear.
[[nodiscard]] auto poly_scale(const linear_poly &a, int64_t factor)
    -> linear_poly;

/// Replaces each variable named in `bindings` with the polynomial bound to
/// it, leaving the rest alone, and re-canonicalizes.
///
/// This is how a callee's value parameters reach the caller's terms: `index
/// [n]`'s predicate `self < n` is written about the callee's `n`, and calling
/// `safe_get(v, i)` with a `v: array[T, 4]` binds `n := 4`, turning the
/// obligation into the `i < 4` the caller can actually be held to.
[[nodiscard]] auto
poly_substitute(const linear_poly &poly,
                const std::unordered_map<std::string, linear_poly> &bindings)
    -> linear_poly;

/// Solves `pattern = value` for a single unknown, when the pattern is a
/// linear expression in exactly one variable with a unit coefficient — the
/// shape a declaration actually writes (`n`, `n + 1`, `n - 1`). Returns the
/// variable and the polynomial it must equal, or `nullopt` when the pattern
/// isn't of that shape (`2n`, `m + n`) and so doesn't determine its unknown.
///
/// Deliberately narrow: this is *inference*, and a call site that doesn't
/// pin a value parameter down uniquely should leave it open rather than
/// guess. `vec[T, m + n]` matched against `vec[T, 5]` determines neither `m`
/// nor `n`, and the compiler says nothing about them rather than inventing a
/// split.
[[nodiscard]] auto solve_for_unknown(const linear_poly &pattern,
                                     const linear_poly &value)
    -> std::optional<std::pair<std::string, linear_poly>>;

/// Whether the equation `a = b` has *any* solution, given `vars_non_negative`
/// (true when the unknowns range over an unsigned type, so `v >= 0`).
///
/// This is what decides value-slot compatibility in `type_table::compatible`:
/// matching `vec[T, n + 1]` against `vec[T, 0]` asks whether `n + 1 = 0` is
/// solvable for `n: usize`, and the answer — no — is what rejects `head` on
/// an empty vector (`spec/dependent-types-design.md` §2.2).
///
/// Exact when the equation has no solution for a *linear* reason (the gcd of
/// the coefficients doesn't divide the constant, or every unknown pushes the
/// sum the wrong way); conservative otherwise, answering "satisfiable" when
/// only a numeric-semigroup argument (`2a + 3b = 1` has no solution in
/// naturals) would refute it. Conservative in the safe direction: the cost is
/// an error the compiler fails to report, never one it reports wrongly.
[[nodiscard]] auto equation_satisfiable(const linear_poly &a,
                                        const linear_poly &b,
                                        bool vars_non_negative) -> bool;

} // namespace kira::semantic
