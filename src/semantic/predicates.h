#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "src/parser/ast.h"
#include "src/semantic/linear_poly.h"
#include "src/semantic/reason.h"

namespace cinder::semantic::detail {

// ==========================================================================
//  Predicates — source expressions to solver terms
//
//  The bridge between the AST and `src/semantic/reason.h`. A predicate the
//  user wrote (`self < n`, `x > 0`, `i < v.len()`) becomes a `goal_form`
//  when it must be *proved* and a `fact_set` when it may be *assumed* —
//  the same translation either way, differing only in what happens to the
//  parts that fall outside the fragment: an unrepresentable goal cannot be
//  proved, while an unrepresentable fact is simply forgotten.
//
//  Both translations carry a substitution, because a predicate is always
//  written about `self` and the declaring type's value parameters, and must
//  be re-stated about the actual value and the actual arguments before it
//  means anything at a use site: `index[n]`'s `self < n`, applied to the
//  argument `i` at a call in a function with its own `n`, is the goal
//  `i < n`.
//
//  Nothing here reads checker state. The one thing the translation needs
//  from its surroundings — which solver variable a bare name stands for —
//  is passed in as a function.
// ==========================================================================

/// Parses an integer literal spelling (handles `_`, `0x`, `0o`, `0b`).
/// Returns nullopt when the value does not fit in 64 bits.
auto parse_integer_literal(std::string_view text) -> std::optional<uint64_t>;

/// Builds the canonical linear polynomial a value expression denotes, or
/// `nullopt` when it falls outside the linear fragment. This owns the
/// arithmetic every caller shares: integer literals, negation, `+`, `-`, and
/// multiplication where one side is closed. `leaf` answers every other node
/// (names, atoms, and so on); returning `nullopt` from it puts that node
/// outside the fragment.
template <typename Leaf>
auto linear_poly_of(const ast::node &node, const Leaf &leaf)
    -> std::optional<linear_poly> {
  switch (node.kind) {
  case ast::node_kind::literal_expr: {
    const auto &lit = dynamic_cast<const ast::literal_expr &>(node);
    if (lit.lit_kind != token_kind::int_lit) {
      return std::nullopt;
    }
    const auto value = parse_integer_literal(lit.value);
    return value.has_value()
               ? std::optional{poly_constant(static_cast<int64_t>(*value))}
               : std::nullopt;
  }
  case ast::node_kind::unary_expr: {
    const auto &unary = dynamic_cast<const ast::unary_expr &>(node);
    if (unary.op != ast::unary_op::neg || unary.operand == nullptr) {
      return std::nullopt;
    }
    const auto operand = linear_poly_of(*unary.operand, leaf);
    return operand.has_value() ? std::optional{poly_negate(*operand)}
                               : std::nullopt;
  }
  case ast::node_kind::binary_expr: {
    const auto &binary = dynamic_cast<const ast::binary_expr &>(node);
    if (binary.lhs == nullptr || binary.rhs == nullptr) {
      return std::nullopt;
    }
    const auto left = linear_poly_of(*binary.lhs, leaf);
    const auto right = linear_poly_of(*binary.rhs, leaf);
    if (!left.has_value() || !right.has_value()) {
      return std::nullopt;
    }
    switch (binary.op) {
    case ast::binary_op::add:
      return poly_add(*left, *right);
    case ast::binary_op::sub:
      return poly_sub(*left, *right);
    case ast::binary_op::mul:
      // Linear means linear: one side has to be closed, or the product
      // isn't a polynomial we can decide anything about.
      if (left->is_constant()) {
        return poly_scale(*right, left->constant);
      }
      if (right->is_constant()) {
        return poly_scale(*left, right->constant);
      }
      return std::nullopt;
    default:
      return std::nullopt;
    }
  }
  default:
    return leaf(node);
  }
}

/// Re-states a predicate about a concrete value. `values` maps each name
/// the predicate is written in terms of (`self`, and the declaration's
/// value parameters) to the polynomial that stands in for it here;
/// `spellings` maps the same names to their source spelling, purely so the
/// diagnostic can echo the goal back in the user's own words.
struct predicate_subst {
  std::unordered_map<std::string, linear_poly> values;
  std::unordered_map<std::string, std::string> spellings;
};

/// What a call site has pinned each of the callee's value parameters to.
using value_bindings = std::unordered_map<std::string, linear_poly>;

/// Translates source predicates and value expressions into solver terms.
class predicate_translator {
public:
  /// Maps a bare name in a predicate to the solver variable it stands for:
  /// an in-scope value parameter is its declaration's variable, anything
  /// else keys on its spelling.
  using variable_resolver = std::function<std::string(const std::string &)>;

  explicit predicate_translator(variable_resolver variable_for)
      : variable_for_(std::move(variable_for)) {}

  /// The atom key standing for an expression the solver cannot look inside
  /// — a field access, or a call to a `pure` function. Canonical and
  /// injective; `nullopt` when the expression has no such key.
  [[nodiscard]] auto atom_key(const ast::expr &expr,
                              const predicate_subst &subst) const
      -> std::optional<std::string>;

  /// The polynomial an integer-valued expression denotes, treating what it
  /// cannot decompose as an opaque atom. `nullopt` means the expression
  /// left the fragment entirely (`m * n`, `n / 2`, an index, a cast).
  [[nodiscard]] auto solver_poly(const ast::expr &expr,
                                 const predicate_subst &subst) const
      -> std::optional<linear_poly>;

  /// Renders a predicate back into source-like text with `subst` applied.
  [[nodiscard]] auto describe(const ast::expr &expr,
                              const predicate_subst &subst) const
      -> std::string;

  /// Translates a boolean predicate into a goal in disjunctive normal form.
  /// An empty result means "not expressible", which upstream reads as "not
  /// provable".
  [[nodiscard]] auto goal_from(const ast::expr &expr,
                               const predicate_subst &subst, bool negated) const
      -> goal_form;

  /// Translates a boolean predicate into facts that may be *assumed*. A
  /// disjunctive fact is dropped, which is sound: forgetting a fact can only
  /// make the compiler prove less.
  [[nodiscard]] auto facts_from(const ast::expr &expr,
                                const predicate_subst &subst,
                                bool negated) const -> fact_set;

  /// Builds `lhs OP rhs` as a constraint `poly REL 0`; `nullopt` when `op`
  /// is not a relational operator.
  [[nodiscard]] static auto
  comparison_constraint(ast::binary_op op, const linear_poly &lhs,
                        const linear_poly &rhs, std::string label)
      -> std::optional<constraint>;

private:
  variable_resolver variable_for_;
};

} // namespace cinder::semantic::detail
