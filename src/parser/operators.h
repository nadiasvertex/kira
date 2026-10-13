#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "src/parser/ast.h"

namespace cinder::ast {

// One table answers the questions every phase asks about an operator: what
// family it belongs to, which trait overloads it, what a compound assignment
// means, and what negating a comparison gives. Spelling stays with
// `binary_op_name` in `ast.h`.

/// @brief The group of operators that share typing and lowering rules.
enum class operator_family : uint8_t {
  pipe,       ///< `|` read as a pipeline (also the bitwise-or spelling).
  logical,    ///< `and`, `or`.
  equality,   ///< `==`, `!=`.
  ordering,   ///< `<`, `<=`, `>`, `>=`.
  arithmetic, ///< `+ - * / %` and their wrapping and saturating forms.
  shift,      ///< `<<`, `>>`.
  bitwise,    ///< `&`, `|`, `^`.
  membership, ///< `in`, `not in`.
  range,      ///< `..`, `..=`.
};

/// @brief Everything the compiler knows about one binary operator.
struct binary_op_info {
  binary_op op;                  ///< The operator this row describes.
  operator_family family;        ///< Typing/lowering family.
  std::string_view overload_trait; ///< Trait that overloads it; empty if none.
};

namespace detail {

// Rows are in `binary_op` declaration order; `binary_op_info_of` indexes by
// enum value and the `static_assert` below checks the order.
inline constexpr std::array<binary_op_info, 29> k_binary_op_table{{
    {binary_op::pipe, operator_family::pipe, ""},
    {binary_op::logical_or, operator_family::logical, ""},
    {binary_op::logical_and, operator_family::logical, ""},
    {binary_op::eq_eq, operator_family::equality, ""},
    {binary_op::bang_eq, operator_family::equality, ""},
    {binary_op::lt, operator_family::ordering, ""},
    {binary_op::lt_eq, operator_family::ordering, ""},
    {binary_op::gt, operator_family::ordering, ""},
    {binary_op::gt_eq, operator_family::ordering, ""},
    {binary_op::in, operator_family::membership, ""},
    {binary_op::not_in, operator_family::membership, ""},
    {binary_op::add, operator_family::arithmetic, "add"},
    {binary_op::sub, operator_family::arithmetic, "sub"},
    {binary_op::mul, operator_family::arithmetic, "mul"},
    {binary_op::div, operator_family::arithmetic, "div"},
    {binary_op::mod, operator_family::arithmetic, "rem"},
    {binary_op::add_wrap, operator_family::arithmetic, "add"},
    {binary_op::sub_wrap, operator_family::arithmetic, "sub"},
    {binary_op::mul_wrap, operator_family::arithmetic, "mul"},
    {binary_op::add_sat, operator_family::arithmetic, "add"},
    {binary_op::sub_sat, operator_family::arithmetic, "sub"},
    {binary_op::mul_sat, operator_family::arithmetic, "mul"},
    {binary_op::shl, operator_family::shift, ""},
    {binary_op::shr, operator_family::shift, ""},
    {binary_op::bit_and, operator_family::bitwise, ""},
    {binary_op::bit_or, operator_family::bitwise, ""},
    {binary_op::bit_xor, operator_family::bitwise, ""},
    {binary_op::range, operator_family::range, ""},
    {binary_op::range_inclusive, operator_family::range, ""},
}};

constexpr auto table_is_in_enum_order() -> bool {
  for (size_t i = 0; i < k_binary_op_table.size(); ++i) {
    if (static_cast<size_t>(k_binary_op_table[i].op) != i) {
      return false;
    }
  }
  return static_cast<size_t>(binary_op::range_inclusive) + 1 ==
         k_binary_op_table.size();
}
static_assert(table_is_in_enum_order(),
              "k_binary_op_table must list every binary_op in enum order");

} // namespace detail

/// @brief The table row for `op`.
[[nodiscard]] constexpr auto binary_op_info_of(binary_op op) noexcept
    -> const binary_op_info & {
  return detail::k_binary_op_table[static_cast<size_t>(op)];
}

/// @brief The family `op` belongs to.
[[nodiscard]] constexpr auto family_of(binary_op op) noexcept
    -> operator_family {
  return binary_op_info_of(op).family;
}

/// @brief `+ - * / %` including the wrapping and saturating forms.
[[nodiscard]] constexpr auto is_arithmetic(binary_op op) noexcept -> bool {
  return family_of(op) == operator_family::arithmetic;
}

/// @brief `==` and `!=`.
[[nodiscard]] constexpr auto is_equality(binary_op op) noexcept -> bool {
  return family_of(op) == operator_family::equality;
}

/// @brief `<`, `<=`, `>`, `>=`.
[[nodiscard]] constexpr auto is_ordering(binary_op op) noexcept -> bool {
  return family_of(op) == operator_family::ordering;
}

/// @brief Equality or ordering comparison.
[[nodiscard]] constexpr auto is_comparison(binary_op op) noexcept -> bool {
  return is_equality(op) || is_ordering(op);
}

/// @brief `and`, `or`.
[[nodiscard]] constexpr auto is_logical(binary_op op) noexcept -> bool {
  return family_of(op) == operator_family::logical;
}

/// @brief `..` and `..=`.
[[nodiscard]] constexpr auto is_range(binary_op op) noexcept -> bool {
  return family_of(op) == operator_family::range;
}

/// @brief Operators that only accept integer operands: shifts and bitwise
/// `& | ^`.
[[nodiscard]] constexpr auto is_integer_bitwise(binary_op op) noexcept -> bool {
  const auto family = family_of(op);
  return family == operator_family::shift || family == operator_family::bitwise;
}

/// @brief The trait that overloads `op` on a user type (`add`, `sub`, `mul`,
/// `div`, `rem`); empty for operators with no overload trait.
[[nodiscard]] constexpr auto overload_trait_of(binary_op op) noexcept
    -> std::string_view {
  return binary_op_info_of(op).overload_trait;
}

/// @brief The comparison that holds exactly when `op` does not: `<` becomes
/// `>=`, `==` becomes `!=`. Operators that are not comparisons are returned
/// unchanged.
[[nodiscard]] constexpr auto negated_comparison(binary_op op) noexcept
    -> binary_op {
  switch (op) {
  case binary_op::eq_eq:
    return binary_op::bang_eq;
  case binary_op::bang_eq:
    return binary_op::eq_eq;
  case binary_op::lt:
    return binary_op::gt_eq;
  case binary_op::lt_eq:
    return binary_op::gt;
  case binary_op::gt:
    return binary_op::lt_eq;
  case binary_op::gt_eq:
    return binary_op::lt;
  default:
    return op;
  }
}

/// @brief The binary operator a compound assignment applies (`+=` is `add`).
/// Plain `=` has none.
[[nodiscard]] constexpr auto compound_assign_op(assign_op op) noexcept
    -> std::optional<binary_op> {
  switch (op) {
  case assign_op::assign:
    return std::nullopt;
  case assign_op::add_assign:
    return binary_op::add;
  case assign_op::sub_assign:
    return binary_op::sub;
  case assign_op::mul_assign:
    return binary_op::mul;
  case assign_op::div_assign:
    return binary_op::div;
  case assign_op::mod_assign:
    return binary_op::mod;
  case assign_op::and_assign:
    return binary_op::bit_and;
  case assign_op::or_assign:
    return binary_op::bit_or;
  case assign_op::xor_assign:
    return binary_op::bit_xor;
  case assign_op::shl_assign:
    return binary_op::shl;
  case assign_op::shr_assign:
    return binary_op::shr;
  case assign_op::add_wrap_assign:
    return binary_op::add_wrap;
  case assign_op::sub_wrap_assign:
    return binary_op::sub_wrap;
  case assign_op::mul_wrap_assign:
    return binary_op::mul_wrap;
  case assign_op::add_sat_assign:
    return binary_op::add_sat;
  case assign_op::sub_sat_assign:
    return binary_op::sub_sat;
  case assign_op::mul_sat_assign:
    return binary_op::mul_sat;
  }
  return std::nullopt;
}

} // namespace cinder::ast
