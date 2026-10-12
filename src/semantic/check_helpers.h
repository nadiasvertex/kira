#pragma once

#include "check.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <deque>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "src/comptime/eval.h"
#include "src/intrinsics.h"
#include "src/parser/ast_clone.h"
#include "src/parser/text_escape.h"
#include "src/semantic/binding_walk.h"
#include "src/semantic/infer/blame.h"
#include "src/semantic/infer/infer_ctxt.h"
#include "src/semantic/infer/obligations.h"
#include "src/semantic/infer/rigid_match.h"
#include "src/semantic/infer/unify.h"
#include "src/semantic/module_index.h"
#include "src/semantic/reason.h"
#include "src/semantic/resolution.h"
#include "src/semantic/types.h"

namespace cinder::semantic::detail {

auto binary_op_spelling(ast::binary_op op) -> std::string_view;

auto parse_integer_literal(std::string_view text) -> std::optional<uint64_t>;

auto is_variant_ident(const ast::ident_expr &ident) -> bool;

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

[[nodiscard]] auto builtin_method_owner(const type_entry &object)
    -> std::string_view;

} // namespace cinder::semantic::detail
