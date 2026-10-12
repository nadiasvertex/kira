#include <memory>
#include <string>
#include <utility>

#include "src/parser/ast.h"
#include "src/semantic/predicates.h"
#include "src/testing/test_assert.h"

namespace {

using cinder::semantic::relation;
using cinder::semantic::detail::predicate_subst;
using cinder::semantic::detail::predicate_translator;
using cinder::testing::expect;
namespace ast = cinder::ast;

auto ident(std::string name) -> ast::ptr<ast::expr> {
  auto node = std::make_unique<ast::ident_expr>();
  node->name = std::move(name);
  return node;
}

auto number(std::string value) -> ast::ptr<ast::expr> {
  auto node = std::make_unique<ast::literal_expr>();
  node->lit_kind = cinder::token_kind::int_lit;
  node->value = std::move(value);
  return node;
}

auto binary(ast::binary_op op, ast::ptr<ast::expr> lhs, ast::ptr<ast::expr> rhs)
    -> ast::ptr<ast::expr> {
  auto node = std::make_unique<ast::binary_expr>();
  node->op = op;
  node->lhs = std::move(lhs);
  node->rhs = std::move(rhs);
  return node;
}

auto negate(ast::ptr<ast::expr> operand) -> ast::ptr<ast::expr> {
  auto node = std::make_unique<ast::unary_expr>();
  node->op = ast::unary_op::logical_not;
  node->operand = std::move(operand);
  return node;
}

auto method_call(ast::ptr<ast::expr> receiver, std::string name)
    -> ast::ptr<ast::expr> {
  auto field = std::make_unique<ast::field_expr>();
  field->object = std::move(receiver);
  field->field_name = std::move(name);
  auto call = std::make_unique<ast::call_expr>();
  call->callee = std::move(field);
  return call;
}

auto free_call(std::string name, ast::ptr<ast::expr> argument)
    -> ast::ptr<ast::expr> {
  auto call = std::make_unique<ast::call_expr>();
  call->callee = ident(std::move(name));
  auto arg = ast::call_arg{};
  arg.value = std::move(argument);
  call->args.push_back(std::move(arg));
  return call;
}

/// Names keep their spelling, except `n`, which stands for a declared value
/// parameter and so carries an identity.
auto translator() -> predicate_translator {
  return predicate_translator{[](const std::string &name) {
    return name == "n" ? std::string{"n#7"} : name;
  }};
}

auto test_folds_linear_arithmetic() -> void {
  // 2 * n + 1
  const auto expr =
      binary(ast::binary_op::add,
             binary(ast::binary_op::mul, number("2"), ident("n")), number("1"));
  const auto poly = translator().solver_poly(*expr, predicate_subst{});
  expect(poly.has_value(), "2 * n + 1 must stay in the fragment");
  expect(poly->coefficient_of("n#7") == 2,
         "the name must resolve through the translator's resolver");
  expect(poly->constant == 1, "the constant must fold");
}

auto test_product_of_unknowns_leaves_the_fragment() -> void {
  const auto expr = binary(ast::binary_op::mul, ident("m"), ident("n"));
  expect(!translator().solver_poly(*expr, predicate_subst{}).has_value(),
         "m * n must be reported as outside the fragment");
}

auto test_comparison_becomes_a_ge_constraint() -> void {
  // i < n   ==>   n - i - 1 >= 0
  const auto expr = binary(ast::binary_op::lt, ident("i"), ident("n"));
  const auto goal = translator().goal_from(*expr, predicate_subst{}, false);
  expect(goal.size() == 1 && goal.front().size() == 1,
         "one comparison is one alternative of one constraint");
  const auto &built = goal.front().front();
  expect(built.rel == relation::ge, "`<` normalizes to `>= 0`");
  expect(built.poly.coefficient_of("n#7") == 1, "n has coefficient 1");
  expect(built.poly.coefficient_of("i") == -1, "i has coefficient -1");
  expect(built.poly.constant == -1, "the strict bound shifts by one");
  expect(built.label == "i < n", "the label echoes the source");
}

auto test_negation_flips_the_comparison() -> void {
  const auto direct = binary(ast::binary_op::gt_eq, ident("i"), ident("n"));
  const auto negated =
      negate(binary(ast::binary_op::lt, ident("i"), ident("n")));
  const auto want = translator().goal_from(*direct, predicate_subst{}, false);
  const auto got = translator().goal_from(*negated, predicate_subst{}, false);
  expect(want.size() == 1 && got.size() == 1, "both are single alternatives");
  expect(want.front().front().poly == got.front().front().poly,
         "`not (i < n)` must be the same constraint as `i >= n`");
}

auto test_connectives_follow_de_morgan() -> void {
  auto both = [] {
    return binary(ast::binary_op::logical_and,
                  binary(ast::binary_op::lt, ident("a"), ident("b")),
                  binary(ast::binary_op::lt, ident("c"), ident("d")));
  };
  const auto conjunction = both();
  const auto goal =
      translator().goal_from(*conjunction, predicate_subst{}, false);
  expect(goal.size() == 1 && goal.front().size() == 2,
         "`and` is one alternative holding both constraints");
  // not (a < b and c < d)  is  a >= b or c >= d: two alternatives.
  const auto negated =
      translator().goal_from(*conjunction, predicate_subst{}, true);
  expect(negated.size() == 2, "negated `and` must split into alternatives");
}

auto test_inexpressible_conjunct_sinks_a_goal() -> void {
  const auto expr = binary(
      ast::binary_op::logical_and,
      binary(ast::binary_op::lt, ident("a"), ident("b")),
      binary(ast::binary_op::lt,
             binary(ast::binary_op::mul, ident("m"), ident("n")), number("3")));
  expect(translator().goal_from(*expr, predicate_subst{}, false).empty(),
         "a goal with an inexpressible conjunct must not be provable");
}

auto test_disjunctive_fact_is_dropped() -> void {
  const auto expr = binary(ast::binary_op::logical_or,
                           binary(ast::binary_op::lt, ident("a"), ident("b")),
                           binary(ast::binary_op::lt, ident("c"), ident("d")));
  expect(translator().facts_from(*expr, predicate_subst{}, false).empty(),
         "a disjunction carries no assumable fact");
}

auto test_describe_applies_spellings() -> void {
  const auto expr = binary(ast::binary_op::lt, ident("self"), ident("n"));
  auto subst = predicate_subst{};
  subst.spellings.emplace("self", "i");
  expect(translator().describe(*expr, subst) == "i < n",
         "`self` must read as the value it was replaced by");
}

auto test_method_and_free_call_share_an_atom() -> void {
  const auto method = method_call(ident("v"), "len");
  const auto free = free_call("len", ident("v"));
  const auto left = translator().atom_key(*method, predicate_subst{});
  const auto right = translator().atom_key(*free, predicate_subst{});
  expect(left.has_value() && right.has_value(), "both calls are atoms");
  expect(*left == *right, "`v.len()` and `len(v)` must key alike");
}

} // namespace

auto main() -> int {
  test_folds_linear_arithmetic();
  test_product_of_unknowns_leaves_the_fragment();
  test_comparison_becomes_a_ge_constraint();
  test_negation_flips_the_comparison();
  test_connectives_follow_de_morgan();
  test_inexpressible_conjunct_sinks_a_goal();
  test_disjunctive_fact_is_dropped();
  test_describe_applies_spellings();
  test_method_and_free_call_share_an_atom();
  return 0;
}
