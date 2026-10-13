#include <memory>
#include <string>
#include <utility>

#include "src/parser/ast.h"
#include "src/semantic/try_from_proof.h"
#include "src/semantic/type_table.h"
#include "src/testing/test_assert.h"

namespace {

using cinder::source_span;
using cinder::semantic::type_table;
using cinder::testing::expect;
namespace ast = cinder::ast;
namespace detail = cinder::semantic::detail;

auto ident(std::string name) -> ast::ptr<ast::expr> {
  auto node = std::make_unique<ast::ident_expr>();
  node->name = std::move(name);
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

auto name_of(const ast::expr &expr) -> const std::string & {
  return dynamic_cast<const ast::ident_expr &>(expr).name;
}

auto test_specialize_replaces_self_and_parameters() -> void {
  // self < n
  auto predicate = binary(ast::binary_op::lt, ident("self"), ident("n"));
  detail::specialize_predicate(predicate, "#proof0", {{"n", 8}});
  const auto &built = dynamic_cast<const ast::binary_expr &>(*predicate);
  expect(name_of(*built.lhs) == "#proof0", "`self` becomes the temporary");
  expect(built.rhs->kind == ast::node_kind::literal_expr,
         "`n` becomes a literal");
  expect(dynamic_cast<const ast::literal_expr &>(*built.rhs).value == "8",
         "the literal is the parameter's constant");
}

auto test_specialize_leaves_other_names_alone() -> void {
  auto predicate = binary(ast::binary_op::lt, ident("self"), ident("limit"));
  detail::specialize_predicate(predicate, "#proof0", {{"n", 8}});
  const auto &built = dynamic_cast<const ast::binary_expr &>(*predicate);
  expect(name_of(*built.rhs) == "limit", "unrelated names are not rewritten");
}

auto test_specialize_rewrites_self_paths() -> void {
  auto path = std::make_unique<ast::module_path_expr>();
  path->segments = {"self", "value"};
  ast::ptr<ast::expr> slot = std::move(path);
  detail::specialize_predicate(slot, "#proof1", {});
  const auto &built = dynamic_cast<const ast::module_path_expr &>(*slot);
  expect(built.segments.front() == "#proof1",
         "`self.value` must read the temporary's field");
  expect(built.segments.back() == "value", "the field name is untouched");
}

auto test_variant_spans_are_one_byte_wider() -> void {
  const auto span = source_span{.start = 10, .end = 40};
  const auto plain = detail::make_ident("some", span, false);
  const auto variant = detail::make_ident("some", span, true);
  expect(plain->span.end - plain->span.start == 4,
         "a plain name spans exactly its width");
  expect(variant->span.end - variant->span.start == 5,
         "a variant name spans its width plus `@`");
}

auto test_variant_call_shape() -> void {
  const auto span = source_span{.start = 0, .end = 5};
  const auto bare = detail::make_variant_call("none", {}, span);
  expect(bare->kind == ast::node_kind::ident_expr, "`@none` has no call");
  const auto call = detail::make_variant_call("some", "t", span);
  expect(call->kind == ast::node_kind::call_expr, "`@some(t)` is a call");
  const auto &built = dynamic_cast<const ast::call_expr &>(*call);
  expect(built.args.size() == 1 && name_of(*built.args.front().value) == "t",
         "the call carries the binding");
}

auto test_proof_syntax_shape() -> void {
  const auto span = source_span{.start = 0, .end = 20};
  auto syntax = detail::build_proof_syntax(
      binary(ast::binary_op::lt, ident("#proof0"), ident("m")), "#proof0",
      span, source_span{.start = 5, .end = 6});
  expect(syntax.block != nullptr, "a block is built");
  expect(syntax.block->stmts.size() == 2, "a `let` and the `if`");
  expect(syntax.block->stmts.front()->kind == ast::node_kind::let_stmt,
         "the first statement binds the value");
  expect(syntax.placeholder != nullptr &&
             syntax.placeholder->name == "#proof0",
         "the placeholder carries the temporary's name");
  expect(syntax.conditional != nullptr &&
             syntax.conditional->kind == ast::node_kind::if_expr,
         "the second statement is the conditional");
  const auto &conditional =
      dynamic_cast<const ast::if_expr &>(*syntax.conditional);
  expect(conditional.branches.size() == 1 && conditional.else_body.size() == 1,
         "one `if` branch and an `else`");
  expect(conditional.branches.front().condition.get() == syntax.predicate,
         "the handle points at the predicate in the tree");
}

auto test_constants_need_known_arguments() -> void {
  auto types = type_table{};
  auto decl = ast::type_decl{};
  auto param = ast::type_param{};
  param.name = "n";
  param.is_value_param = true;
  decl.type_params.push_back(std::move(param));

  const auto usize = types.usize_type();
  const auto base = types.builtin("int32");
  const auto known = types.refinement_of(&decl, "index", "m", base,
                                         {types.const_value(usize, 8)}, nullptr);
  const auto open = types.refinement_of(
      &decl, "index", "m", base,
      {types.symbolic_value(usize, cinder::semantic::poly_variable("k"))},
      nullptr);

  const auto closed = detail::refinement_constants(types, types.entry(known));
  expect(closed.has_value() && closed->at("n") == 8,
         "a constant argument yields its value");
  expect(!detail::refinement_constants(types, types.entry(open)).has_value(),
         "an open argument yields nothing");
}

} // namespace

auto main() -> int {
  test_specialize_replaces_self_and_parameters();
  test_specialize_leaves_other_names_alone();
  test_specialize_rewrites_self_paths();
  test_variant_spans_are_one_byte_wider();
  test_variant_call_shape();
  test_proof_syntax_shape();
  test_constants_need_known_arguments();
  return 0;
}
