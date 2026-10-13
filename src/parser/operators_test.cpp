#include <string_view>

#include "src/parser/operators.h"
#include "src/testing/test_assert.h"

namespace {

using cinder::ast::assign_op;
using cinder::ast::binary_op;
using cinder::testing::expect;
namespace ast = cinder::ast;

auto test_overload_traits() -> void {
  expect(ast::overload_trait_of(binary_op::add) == "add", "add -> add");
  expect(ast::overload_trait_of(binary_op::sub_wrap) == "sub",
         "wrapping sub overloads through sub");
  expect(ast::overload_trait_of(binary_op::mul_sat) == "mul",
         "saturating mul overloads through mul");
  expect(ast::overload_trait_of(binary_op::div) == "div", "div -> div");
  expect(ast::overload_trait_of(binary_op::mod) == "rem", "mod -> rem");
  expect(ast::overload_trait_of(binary_op::shl).empty(),
         "shift has no overload trait");
  expect(ast::overload_trait_of(binary_op::eq_eq).empty(),
         "equality has no overload trait");
}

auto test_families() -> void {
  expect(ast::is_arithmetic(binary_op::add_sat), "add_sat is arithmetic");
  expect(!ast::is_arithmetic(binary_op::bit_and), "& is not arithmetic");
  expect(ast::is_comparison(binary_op::bang_eq), "!= is a comparison");
  expect(ast::is_equality(binary_op::eq_eq) && !ast::is_ordering(binary_op::eq_eq),
         "== is equality, not ordering");
  expect(ast::is_ordering(binary_op::gt_eq), ">= is ordering");
  expect(ast::is_logical(binary_op::logical_or), "or is logical");
  expect(ast::is_range(binary_op::range_inclusive), "..= is a range");
  expect(ast::is_integer_bitwise(binary_op::shr) &&
             ast::is_integer_bitwise(binary_op::bit_xor),
         "shift and bitwise are integer-only");
  expect(!ast::is_integer_bitwise(binary_op::add), "+ accepts non-integers");
}

auto test_negated_comparison() -> void {
  expect(ast::negated_comparison(binary_op::lt) == binary_op::gt_eq, "< -> >=");
  expect(ast::negated_comparison(binary_op::gt_eq) == binary_op::lt, ">= -> <");
  expect(ast::negated_comparison(binary_op::eq_eq) == binary_op::bang_eq,
         "== -> !=");
  expect(ast::negated_comparison(binary_op::add) == binary_op::add,
         "non-comparisons are unchanged");
}

auto test_compound_assign() -> void {
  expect(!ast::compound_assign_op(assign_op::assign).has_value(),
         "plain assignment has no binary operator");
  expect(ast::compound_assign_op(assign_op::add_assign) == binary_op::add,
         "+= is add");
  expect(ast::compound_assign_op(assign_op::xor_assign) == binary_op::bit_xor,
         "^= is xor");
  expect(ast::compound_assign_op(assign_op::shr_assign) == binary_op::shr,
         ">>= is shr");
  expect(ast::compound_assign_op(assign_op::mul_sat_assign) ==
             binary_op::mul_sat,
         "*|= is saturating mul");
  // Every compound form must map to an arithmetic or integer-only operator.
  for (auto raw = 1; raw <= static_cast<int>(assign_op::mul_sat_assign);
       ++raw) {
    const auto op = ast::compound_assign_op(static_cast<assign_op>(raw));
    expect(op.has_value() &&
               (ast::is_arithmetic(*op) || ast::is_integer_bitwise(*op)),
           "compound operator maps to arithmetic or integer-only operator");
  }
}

} // namespace

auto main() -> int {
  test_overload_traits();
  test_families();
  test_negated_comparison();
  test_compound_assign();
  return 0;
}
