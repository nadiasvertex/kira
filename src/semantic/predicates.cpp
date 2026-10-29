#include "src/semantic/predicates.h"

#include <charconv>
#include <format>
#include <vector>

#include "src/parser/ast_traversal.h"
#include "src/parser/operators.h"
#include "src/semantic/module_index.h"

namespace cinder::semantic::detail {

/// Parses an integer literal spelling (handles `_`, `0x`, `0o`, `0b`).
/// Returns nullopt when the value does not fit in 64 bits.
auto parse_integer_literal(std::string_view text) -> std::optional<uint64_t> {
  auto cleaned = std::string{};
  cleaned.reserve(text.size());
  for (const auto ch : text) {
    if (ch != '_') {
      cleaned.push_back(ch);
    }
  }
  auto base = 10;
  auto digits = std::string_view(cleaned);
  if (digits.size() > 2 && digits[0] == '0') {
    if (digits[1] == 'x' || digits[1] == 'X') {
      base = 16;
      digits.remove_prefix(2);
    } else if (digits[1] == 'o' || digits[1] == 'O') {
      base = 8;
      digits.remove_prefix(2);
    } else if (digits[1] == 'b' || digits[1] == 'B') {
      base = 2;
      digits.remove_prefix(2);
    }
  }
  auto value = uint64_t{0};
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,bugprone-suspicious-stringview-data-usage)
  // std::from_chars requires a raw begin/end pointer pair.
  const char *digits_end = digits.data() + digits.size();
  const auto result = std::from_chars(digits.data(), digits_end, value, base);
  if (result.ec != std::errc{} || result.ptr != digits_end) {
    return std::nullopt;
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,bugprone-suspicious-stringview-data-usage)
  return value;
}

// ==========================================================================
//  predicate_translator
// ==========================================================================

/// The atom key standing for an expression the solver cannot look inside —
/// a field access, or a call to a `pure` function. Uninterpreted: `f(x)`
/// equals `f(x)` and nothing else is known about it, so the key must be
/// canonical (two spellings of the same argument must produce one key) and
/// injective (two different arguments must not).
auto predicate_translator::atom_key(const ast::expr &expr,
                                    const predicate_subst &subst) const
    -> std::optional<std::string> {
  switch (expr.kind) {
  case ast::node_kind::ident_expr: {
    const auto &ident = ast::cast<const ast::ident_expr>(expr);
    if (const auto it = subst.values.find(ident.name);
        it != subst.values.end()) {
      return it->second.atom_text();
    }
    return variable_for_(ident.name);
  }
  case ast::node_kind::field_expr: {
    const auto &field = ast::cast<const ast::field_expr>(expr);
    if (field.object == nullptr) {
      return std::nullopt;
    }
    const auto object = atom_key(*field.object, subst);
    return object.has_value()
               ? std::optional{std::format("{}.{}", *object, field.field_name)}
               : std::nullopt;
  }
  case ast::node_kind::module_path_expr: {
    // `p.value` and `self.value` parse as *paths*, not field accesses —
    // `a.b` is ambiguous between a projection and a module member until
    // names are resolved, and the parser does not resolve names. So the
    // shape a predicate about a struct field actually arrives in is this
    // one, and treating it as an atom keyed on the written path is both
    // correct and exactly what makes an `invariant self.value > 0`
    // discharge a `positive` at `p.value`: the fact and the goal key alike.
    const auto &path = ast::cast<const ast::module_path_expr>(expr);
    if (path.segments.empty()) {
      return std::nullopt;
    }
    auto segments = path.segments;
    if (const auto it = subst.values.find(segments.front());
        it != subst.values.end()) {
      segments.front() = it->second.atom_text();
    }
    return join_strings(segments, ".");
  }
  case ast::node_kind::call_expr: {
    // A method call `v.len()` keys as `len(v)`, a free call `f(x)` as
    // `f(x)` — so a fact stated one way discharges a goal stated the other.
    const auto &call = ast::cast<const ast::call_expr>(expr);
    if (call.callee == nullptr) {
      return std::nullopt;
    }
    auto name = std::string{};
    auto arguments = std::vector<std::string>{};
    if (call.callee->kind == ast::node_kind::field_expr) {
      const auto &field = ast::cast<const ast::field_expr>(*call.callee);
      if (field.object == nullptr) {
        return std::nullopt;
      }
      const auto receiver = atom_key(*field.object, subst);
      if (!receiver.has_value()) {
        return std::nullopt;
      }
      name = field.field_name;
      arguments.push_back(*receiver);
    } else if (call.callee->kind == ast::node_kind::ident_expr) {
      name = ast::cast<const ast::ident_expr>(*call.callee).name;
    } else {
      return std::nullopt;
    }
    for (const auto &arg : call.args) {
      if (arg.value == nullptr) {
        return std::nullopt;
      }
      const auto key = atom_key(*arg.value, subst);
      if (!key.has_value()) {
        return std::nullopt;
      }
      arguments.push_back(*key);
    }
    return std::format("{}({})", name, join_strings(arguments, ","));
  }
  default:
    return std::nullopt;
  }
}

/// Translates a value expression into a solver polynomial, treating what it
/// cannot decompose as an opaque atom. `nullopt` means the expression left
/// the fragment entirely (`m * n`, `n / 2`, an index, a cast) — see
/// `spec/dependent-types-design.md` §4.4.
auto predicate_translator::solver_poly(const ast::expr &expr,
                                       const predicate_subst &subst) const
    -> std::optional<linear_poly> {
  return linear_poly_of(
      expr, [&](const ast::node &leaf) -> std::optional<linear_poly> {
        switch (leaf.kind) {
        case ast::node_kind::ident_expr: {
          const auto &ident = ast::cast<const ast::ident_expr>(leaf);
          if (const auto it = subst.values.find(ident.name);
              it != subst.values.end()) {
            return it->second;
          }
          return poly_variable(variable_for_(ident.name));
        }
        case ast::node_kind::field_expr:
        case ast::node_kind::module_path_expr:
        case ast::node_kind::call_expr: {
          const auto key = atom_key(ast::cast<const ast::expr>(leaf), subst);
          return key.has_value() ? std::optional{poly_variable(*key)}
                                 : std::nullopt;
        }
        default:
          return std::nullopt;
        }
      });
}

/// Builds `lhs OP rhs` as a constraint `poly REL 0`. `<` and `<=` have no
/// relation of their own — they are the same statement about the negated
/// polynomial — and `<`/`>` shift the constant by one because the fragment
/// is over integers, where `a > b` *is* `a >= b + 1`.
auto predicate_translator::comparison_constraint(ast::binary_op op,
                                                 const linear_poly &lhs,
                                                 const linear_poly &rhs,
                                                 std::string label)
    -> std::optional<constraint> {
  const auto difference = poly_sub(lhs, rhs);
  auto shifted = [&](int64_t by) -> linear_poly {
    auto poly = difference;
    poly.constant += by;
    return poly;
  };
  switch (op) {
  case ast::binary_op::eq_eq:
    return constraint{
        .poly = difference, .rel = relation::eq, .label = std::move(label)};
  case ast::binary_op::bang_eq:
    return constraint{
        .poly = difference, .rel = relation::ne, .label = std::move(label)};
  case ast::binary_op::gt_eq:
    return constraint{
        .poly = difference, .rel = relation::ge, .label = std::move(label)};
  case ast::binary_op::gt:
    return constraint{
        .poly = shifted(-1), .rel = relation::ge, .label = std::move(label)};
  case ast::binary_op::lt_eq:
    return constraint{.poly = poly_negate(difference),
                      .rel = relation::ge,
                      .label = std::move(label)};
  case ast::binary_op::lt:
    return constraint{.poly = poly_negate(shifted(1)),
                      .rel = relation::ge,
                      .label = std::move(label)};
  default:
    return std::nullopt;
  }
}

/// Renders a predicate back into source-like text with the substitution
/// applied, so a diagnostic about `index[n]`'s `self < n` can say
/// ``cannot prove `i < n` `` rather than quoting a predicate the user never
/// wrote about a `self` that isn't there.
auto predicate_translator::describe(const ast::expr &expr,
                                    const predicate_subst &subst) const
    -> std::string {
  switch (expr.kind) {
  case ast::node_kind::literal_expr:
    return {ast::cast<const ast::literal_expr>(expr).value};
  case ast::node_kind::ident_expr: {
    const auto &ident = ast::cast<const ast::ident_expr>(expr);
    if (const auto it = subst.spellings.find(ident.name);
        it != subst.spellings.end()) {
      return it->second;
    }
    return ident.name;
  }
  case ast::node_kind::field_expr: {
    const auto &field = ast::cast<const ast::field_expr>(expr);
    return field.object != nullptr
               ? std::format("{}.{}", describe(*field.object, subst),
                             field.field_name)
               : field.field_name;
  }
  case ast::node_kind::module_path_expr: {
    const auto &path = ast::cast<const ast::module_path_expr>(expr);
    auto segments = path.segments;
    if (!segments.empty()) {
      if (const auto it = subst.spellings.find(segments.front());
          it != subst.spellings.end()) {
        segments.front() = it->second;
      }
    }
    return join_strings(segments, ".");
  }
  case ast::node_kind::call_expr: {
    const auto &call = ast::cast<const ast::call_expr>(expr);
    auto arguments = std::vector<std::string>{};
    for (const auto &arg : call.args) {
      arguments.push_back(arg.value != nullptr ? describe(*arg.value, subst)
                                               : "_");
    }
    const auto callee =
        call.callee != nullptr ? describe(*call.callee, subst) : "_";
    return std::format("{}({})", callee, join_strings(arguments, ", "));
  }
  case ast::node_kind::unary_expr: {
    const auto &unary = ast::cast<const ast::unary_expr>(expr);
    const auto operand =
        unary.operand != nullptr ? describe(*unary.operand, subst) : "_";
    switch (unary.op) {
    case ast::unary_op::neg:
      return std::format("-{}", operand);
    case ast::unary_op::logical_not:
      return std::format("not {}", operand);
    default:
      return operand;
    }
  }
  case ast::node_kind::binary_expr: {
    const auto &binary = ast::cast<const ast::binary_expr>(expr);
    const auto left =
        binary.lhs != nullptr ? describe(*binary.lhs, subst) : "_";
    const auto right =
        binary.rhs != nullptr ? describe(*binary.rhs, subst) : "_";
    return std::format("{} {} {}", left, ast::binary_op_name(binary.op), right);
  }
  default:
    return "...";
  }
}

/// Translates a boolean predicate into a goal in disjunctive normal form.
/// An empty result means "not expressible", which upstream reads as "not
/// provable" — the graceful path, never a silent acceptance.
auto predicate_translator::goal_from(const ast::expr &expr,
                                     const predicate_subst &subst,
                                     bool negated) const -> goal_form {
  switch (expr.kind) {
  case ast::node_kind::unary_expr: {
    const auto &unary = ast::cast<const ast::unary_expr>(expr);
    if (unary.op == ast::unary_op::logical_not && unary.operand != nullptr) {
      return goal_from(*unary.operand, subst, !negated);
    }
    break;
  }
  case ast::node_kind::binary_expr: {
    const auto &binary = ast::cast<const ast::binary_expr>(expr);
    if (binary.lhs == nullptr || binary.rhs == nullptr) {
      break;
    }
    // De Morgan: negation swaps the connective, so one traversal handles
    // both polarities.
    const auto conjunctive =
        (binary.op == ast::binary_op::logical_and) != negated;
    if (binary.op == ast::binary_op::logical_and ||
        binary.op == ast::binary_op::logical_or) {
      const auto left = goal_from(*binary.lhs, subst, negated);
      const auto right = goal_from(*binary.rhs, subst, negated);
      if (!conjunctive) {
        // Disjunction: either alternative will do, so an inexpressible side
        // just drops out and the other still stands on its own.
        auto combined = left;
        combined.insert(combined.end(), right.begin(), right.end());
        return combined;
      }
      // Conjunction: every conjunct must be proved, so an inexpressible
      // side sinks the whole goal.
      if (left.empty() || right.empty()) {
        return {};
      }
      auto combined = goal_form{};
      for (const auto &left_alt : left) {
        for (const auto &right_alt : right) {
          auto merged = left_alt;
          merged.insert(merged.end(), right_alt.begin(), right_alt.end());
          combined.push_back(std::move(merged));
        }
      }
      return combined;
    }

    const auto left = solver_poly(*binary.lhs, subst);
    const auto right = solver_poly(*binary.rhs, subst);
    if (!left.has_value() || !right.has_value()) {
      return {};
    }
    const auto op = negated ? ast::negated_comparison(binary.op) : binary.op;
    auto built =
        comparison_constraint(op, *left, *right, describe(expr, subst));
    if (!built.has_value()) {
      return {};
    }
    return goal_form{fact_set{std::move(*built)}};
  }
  default:
    break;
  }

  // Any other boolean term — a call to a `pure` predicate function, say —
  // enters as `atom == 1`. Deliberately the weakest useful rule: an
  // identical fact discharges it, and nothing else does.
  const auto atom = atom_key(expr, subst);
  if (!atom.has_value()) {
    return {};
  }
  auto poly = poly_variable(*atom);
  poly.constant -= 1;
  return goal_form{fact_set{constraint{
      .poly = std::move(poly),
      .rel = negated ? relation::ne : relation::eq,
      .label = describe(expr, subst),
  }}};
}

/// Translates a boolean predicate into facts that may be *assumed*. Unlike
/// a goal this is a plain conjunction: a disjunctive fact carries no
/// information the solver's fragment can use, so it is dropped — sound,
/// because forgetting a fact can only make the compiler prove less.
auto predicate_translator::facts_from(const ast::expr &expr,
                                      const predicate_subst &subst,
                                      bool negated) const -> fact_set {
  const auto goal = goal_from(expr, subst, negated);
  return goal.size() == 1 ? goal.front() : fact_set{};
}

} // namespace cinder::semantic::detail
