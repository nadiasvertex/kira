#include <memory>
#include <string>
#include <utility>

#include "src/parser/ast.h"
#include "src/semantic/narrowing.h"
#include "src/testing/test_assert.h"

namespace {

using cinder::semantic::fact_set;
using cinder::semantic::proof_result;
using cinder::semantic::type_id;
using cinder::semantic::type_table;
using cinder::semantic::detail::fact_scope;
using cinder::semantic::detail::narrowing_engine;
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

auto compare(ast::binary_op op, std::string name, std::string value)
    -> ast::ptr<ast::expr> {
  auto node = std::make_unique<ast::binary_expr>();
  node->op = op;
  node->lhs = ident(std::move(name));
  node->rhs = number(std::move(value));
  return node;
}

/// A type table, the predicates the tests refine with, and the facts the
/// engine works on. Predicates are owned here because refinement types
/// point at them.
struct fixture {
  type_table types;
  fact_set facts;
  ast::ptr<ast::expr> positive_predicate =
      compare(ast::binary_op::gt, "self", "0");
  ast::ptr<ast::expr> small_predicate =
      compare(ast::binary_op::lt, "self", "10");
  type_id int32 = types.builtin("int32");
  type_id positive = types.refinement_of(nullptr, "positive", "m", int32, {},
                                         positive_predicate.get());
  type_id small = types.refinement_of(nullptr, "small", "m", int32, {},
                                      small_predicate.get());

  auto engine() -> narrowing_engine {
    return narrowing_engine{
        types, predicate_translator{[](const std::string &n) { return n; }},
        facts};
  }
};

auto test_unproven_narrowing_reports_the_goal() -> void {
  auto f = fixture{};
  const auto value = ident("x");
  const auto failure =
      f.engine().check(f.positive, f.int32, value.get(), nullptr);
  expect(failure.has_value(), "nothing is known about x, so nothing is proved");
  expect(failure->outcome == proof_result::unknown, "unproven, not refuted");
  expect(failure->goal_text == "x > 0", "the goal is the predicate about x");
  expect(failure->refined_name == "positive", "the refinement's name");
  expect(failure->base_name == "int32", "the base type's name");
  expect(failure->known_note.has_value() &&
             failure->known_note->find("nothing is known") != std::string::npos,
         "the note must say nothing is known about x");
}

auto test_assumed_condition_discharges_narrowing() -> void {
  auto f = fixture{};
  const auto value = ident("x");
  const auto condition = compare(ast::binary_op::gt, "x", "0");
  f.engine().assume_condition(condition.get(), false);
  expect(!f.engine().check(f.positive, f.int32, value.get(), nullptr),
         "`x > 0` in scope must prove x as a `positive`");
}

auto test_negated_condition_refutes() -> void {
  auto f = fixture{};
  const auto value = ident("x");
  const auto condition = compare(ast::binary_op::gt, "x", "0");
  f.engine().assume_condition(condition.get(), true);
  const auto failure =
      f.engine().check(f.positive, f.int32, value.get(), nullptr);
  expect(failure.has_value(), "`not (x > 0)` must not prove `positive`");
  expect(failure->outcome == proof_result::refuted,
         "the facts rule the predicate out");
}

auto test_fact_scope_forgets_a_branch() -> void {
  auto f = fixture{};
  const auto value = ident("x");
  const auto condition = compare(ast::binary_op::gt, "x", "0");
  {
    auto scope = fact_scope(f.facts);
    f.engine().assume_condition(condition.get(), false);
    expect(f.facts.size() == 1, "the branch condition is a fact inside");
  }
  expect(f.facts.empty(), "the fact must not outlive its region");
  expect(
      f.engine().check(f.positive, f.int32, value.get(), nullptr).has_value(),
      "after the region x is no longer known positive");
}

auto test_assignment_invalidates_facts() -> void {
  auto f = fixture{};
  const auto condition = compare(ast::binary_op::gt, "x", "0");
  const auto other = compare(ast::binary_op::gt, "xs", "0");
  f.engine().assume_condition(condition.get(), false);
  f.engine().assume_condition(other.get(), false);
  f.engine().invalidate("x");
  expect(f.facts.size() == 1, "only the facts about x are dropped");
  expect(cinder::semantic::detail::atom_mentions("len(x)", "x"),
         "a call over x mentions x");
  expect(!cinder::semantic::detail::atom_mentions("xs", "x"),
         "a longer name is not x");
}

auto test_refined_binding_assumes_its_predicate() -> void {
  auto f = fixture{};
  f.engine().assume_binding("y", f.positive);
  expect(f.facts.size() == 1, "a refined binding contributes its predicate");
  // y > 0 is known, so y < 10 is not, but y >= 1 follows: use `small` to
  // check the engine does not over-prove.
  const auto value = ident("y");
  expect(f.engine().check(f.small, f.int32, value.get(), nullptr).has_value(),
         "y > 0 does not prove y < 10");
  expect(!f.engine().check(f.positive, f.positive, value.get(), nullptr),
         "a value already of the refined type owes nothing");
}

auto test_array_binding_assumes_its_length() -> void {
  auto f = fixture{};
  const auto length = f.types.const_value(f.types.usize_type(), 4);
  const auto array = f.types.array_of(f.int32, 4, length);
  f.engine().assume_binding("v", array);
  expect(f.facts.size() == 1, "an array of known length contributes len(v)");
  expect(f.facts.front().label == "len(v) == 4", "the fact reads len(v) == 4");
}

auto test_precondition_known_from_facts_is_elidable() -> void {
  auto f = fixture{};
  f.engine().assume_binding("y", f.positive);
  const auto implied = compare(ast::binary_op::gt, "y", "0");
  const auto fresh = compare(ast::binary_op::gt, "z", "0");
  expect(f.engine().assume_precondition(*implied),
         "`pre y > 0` follows from `y: positive`");
  expect(!f.engine().assume_precondition(*fresh),
         "`pre z > 0` says something new");
  const auto value = ident("z");
  expect(!f.engine().check(f.positive, f.int32, value.get(), nullptr),
         "an assumed precondition is a fact afterwards");
}

} // namespace

auto main() -> int {
  test_unproven_narrowing_reports_the_goal();
  test_assumed_condition_discharges_narrowing();
  test_negated_condition_refutes();
  test_fact_scope_forgets_a_branch();
  test_assignment_invalidates_facts();
  test_refined_binding_assumes_its_predicate();
  test_array_binding_assumes_its_length();
  test_precondition_known_from_facts_is_elidable();
  return 0;
}
