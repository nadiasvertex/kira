#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/semantic/module_index.h"

#include "src/parser/parser.h"
#include "src/testing/test_assert.h"
#include "src/testing/test_data.h"

namespace {

using cinder::testing::expect;
using cinder::testing::fail;

struct source_fixture {
  std::string path;
  std::string text;
};

struct parsed_fixture {
  cinder::source_manager sources;
  cinder::diagnostic_bag diag;
  std::vector<cinder::ast::ptr<cinder::ast::file>> ast_files;
  std::vector<cinder::semantic::parsed_module> parsed_modules;
};

auto parse_sources(const std::vector<source_fixture> &fixtures)
    -> parsed_fixture {
  auto parsed = parsed_fixture{
      .sources = cinder::source_manager{},
      .diag = cinder::diagnostic_bag{},
      .ast_files = {},
      .parsed_modules = {},
  };
  parsed.ast_files.reserve(fixtures.size());
  parsed.parsed_modules.reserve(fixtures.size());

  for (const auto &fixture : fixtures) {
    auto file_id = parsed.sources.add_file(fixture.path, fixture.text);
    expect(file_id.has_value(), "expected fixture source to register");

    const auto *file = parsed.sources.get(*file_id);
    expect(file != nullptr, "expected registered fixture source");

    auto lexer = cinder::lexer(file->source(), file->id(), parsed.diag);
    auto tokens = lexer.tokenize();
    auto parser = cinder::parser(std::move(tokens), file->id(), parsed.diag);
    auto ast_file = parser.parse_file();

    parsed.parsed_modules.push_back(cinder::semantic::parsed_module{
        .file_id = *file_id,
        .ast_file = ast_file.get(),
    });
    parsed.ast_files.push_back(std::move(ast_file));
  }

  expect(parsed.diag.error_count() == 0,
         "expected resolution test fixtures to parse cleanly");
  return parsed;
}

auto load_test_data_fixture(std::string_view filename) -> source_fixture {
  const auto test_data_dir =
      cinder::testing::find_test_data_dir("semantic_resolution_test");
  const auto text =
      cinder::testing::load_test_data_file(test_data_dir.string(), filename);
  return source_fixture{.path = std::string(filename), .text = text};
}

auto expect_expr_stmt(const cinder::ast::node *node)
    -> const cinder::ast::expr_stmt * {
  expect(node != nullptr, "expected expression statement node");
  expect(node->kind == cinder::ast::node_kind::expr_stmt,
         "expected expression statement node kind");
  return dynamic_cast<const cinder::ast::expr_stmt *>(node);
}

auto expect_func_decl(const cinder::ast::node *node)
    -> const cinder::ast::func_decl * {
  expect(node != nullptr, "expected function declaration node");
  expect(node->kind == cinder::ast::node_kind::func_decl,
         "expected function declaration node kind");
  return dynamic_cast<const cinder::ast::func_decl *>(node);
}

auto expect_if_stmt(const cinder::ast::node *node) -> const cinder::ast::if_stmt * {
  expect(node != nullptr, "expected if statement node");
  expect(node->kind == cinder::ast::node_kind::if_stmt,
         "expected if statement node kind");
  return dynamic_cast<const cinder::ast::if_stmt *>(node);
}

auto expect_match_stmt(const cinder::ast::node *node)
    -> const cinder::ast::match_stmt * {
  expect(node != nullptr, "expected match statement node");
  expect(node->kind == cinder::ast::node_kind::match_stmt,
         "expected match statement node kind");
  return dynamic_cast<const cinder::ast::match_stmt *>(node);
}

auto expect_lambda_expr(const cinder::ast::expr *expr)
    -> const cinder::ast::lambda_expr * {
  expect(expr != nullptr, "expected lambda expression");
  expect(expr->kind == cinder::ast::node_kind::lambda_expr,
         "expected lambda expression kind");
  return dynamic_cast<const cinder::ast::lambda_expr *>(expr);
}

auto expect_let_stmt(const cinder::ast::node *node)
    -> const cinder::ast::let_stmt * {
  expect(node != nullptr, "expected let statement node");
  expect(node->kind == cinder::ast::node_kind::let_stmt,
         "expected let statement node kind");
  return dynamic_cast<const cinder::ast::let_stmt *>(node);
}

auto expect_ident_expr(const cinder::ast::expr *expr)
    -> const cinder::ast::ident_expr * {
  expect(expr != nullptr, "expected identifier expression");
  expect(expr->kind == cinder::ast::node_kind::ident_expr,
         "expected identifier expression kind");
  return dynamic_cast<const cinder::ast::ident_expr *>(expr);
}

auto find_node_scope_or_fail(const cinder::semantic::semantic_session &session,
                             const cinder::ast::node &node)
    -> cinder::semantic::scope_id {
  const auto scope = cinder::semantic::find_node_scope(session, node);
  if (!scope.has_value()) {
    fail("expected node scope to be recorded");
  }
  return *scope;
}

/// Finds the one node `pick` accepts among every node the scope walk
/// recorded — so a node the walk never reached is a test failure, not a
/// crash in hand-navigated AST.
template <typename predicate>
auto find_recorded_node(const cinder::semantic::semantic_session &session,
                        predicate pick, std::string_view what)
    -> const cinder::ast::node & {
  const cinder::ast::node *found = nullptr;
  for (const auto &[node, scope] : session.node_scopes) {
    (void)scope;
    if (pick(*node)) {
      expect(found == nullptr,
             std::string("expected one ") + std::string(what));
      found = node;
    }
  }
  if (found == nullptr) {
    fail(std::string("the scope walk never reached ") + std::string(what));
  }
  return *found;
}

/// The scope tree reaches every expression, not only statements: a lambda
/// nested in a call argument gets its parameter scope, and a `static for`
/// binder is a lexical binding (spec "Dotted Names", rule 1) visible to
/// the loop's body.
auto test_scope_walk_reaches_nested_expressions_and_static_for() -> void {
  const auto parsed = parse_sources({source_fixture{
      .path = "walk.cn",
      .text = "module sample\n"
              "def apply(f: fn(int32) -> int32, x: int32) -> int32:\n"
              "  return f(x)\n"
              "def run(bound: int32) -> int32:\n"
              "  return apply(k => k + bound, 1)\n"
              "static for step in [1, 2]:\n"
              "  static assert step.value > 0, \"positive\"\n",
  }});
  const auto session =
      cinder::semantic::build_semantic_session(parsed.parsed_modules);
  using cinder::semantic::symbol_namespace;

  const auto &k_ref = find_recorded_node(
      session,
      [](const cinder::ast::node &node) {
        return node.kind == cinder::ast::node_kind::ident_expr &&
               dynamic_cast<const cinder::ast::ident_expr &>(node).name == "k";
      },
      "the lambda body's `k`");
  const auto *k_symbol = cinder::semantic::resolve_symbol(
      session, find_node_scope_or_fail(session, k_ref),
      symbol_namespace::value_namespace, "k");
  expect(k_symbol != nullptr &&
             k_symbol->kind ==
                 cinder::semantic::semantic_symbol_kind::parameter_symbol,
         "expected `k` inside a lambda passed as a call argument to resolve "
         "to the lambda's parameter");

  const auto &step_ref = find_recorded_node(
      session,
      [](const cinder::ast::node &node) {
        return node.kind == cinder::ast::node_kind::module_path_expr &&
               dynamic_cast<const cinder::ast::module_path_expr &>(node)
                       .segments.front() == "step";
      },
      "the `static for` body's `step.value`");
  const auto step_scope = find_node_scope_or_fail(session, step_ref);
  const auto *step_symbol = cinder::semantic::resolve_symbol(
      session, step_scope, symbol_namespace::value_namespace, "step");
  expect(step_symbol != nullptr,
         "expected the `static for` binder to be in scope in its body");
  const auto *binder_scope =
      cinder::semantic::find_semantic_scope(session, step_symbol->defining_scope);
  expect(binder_scope != nullptr &&
             binder_scope->kind ==
                 cinder::semantic::semantic_scope_kind::static_for_scope,
         "expected the binder to live in a `static for` scope");
}

auto test_build_semantic_session_indexes_module_symbols() -> void {
  const auto parsed = parse_sources({
      load_test_data_fixture(
          "build_semantic_session_indexes_module_symbols.cn"),
  });

  const auto session =
      cinder::semantic::build_semantic_session(parsed.parsed_modules);
  const auto index =
      cinder::semantic::build_semantic_resolution_index(parsed.parsed_modules);

  const auto *module_scope =
      cinder::semantic::find_module_scope(index, "sample.tools");
  expect(module_scope != nullptr, "expected module scope to exist");
  expect(module_scope->symbols.size() == 5,
         "expected five direct module symbols to be indexed");

  const auto *type_symbol =
      cinder::semantic::find_module_scope_symbol(index, "sample.tools", "point");
  expect(type_symbol != nullptr, "expected type symbol lookup to succeed");
  expect(type_symbol->kind == cinder::semantic::semantic_symbol_kind::type_symbol,
         "expected type symbol kind");

  const auto *function_symbol =
      cinder::semantic::find_module_scope_symbol(index, "sample.tools", "run");
  expect(function_symbol != nullptr,
         "expected function symbol lookup to succeed");
  expect(function_symbol->name_space ==
             cinder::semantic::symbol_namespace::value_namespace,
         "expected function to live in value namespace");

  const auto *inner_scope =
      cinder::semantic::find_module_scope(index, "sample.tools.inner");
  expect(inner_scope == nullptr, "expected declaration-only submodule without "
                                 "a body to avoid creating a nested scope");

  expect(session.symbols.size() >= 5,
         "expected semantic session to own stable symbol records");
}

auto test_resolve_value_name_shadowing_in_nested_blocks() -> void {
  const auto parsed = parse_sources({
      load_test_data_fixture(
          "resolve_value_name_shadowing_in_nested_blocks.cn"),
  });

  const auto session =
      cinder::semantic::build_semantic_session(parsed.parsed_modules);
  const auto *file = parsed.ast_files.front().get();
  const auto *run = expect_func_decl(file->items[0].get());
  const auto *if_stmt = expect_if_stmt(run->body_stmts[1].get());
  const auto *inner_expr = expect_expr_stmt(if_stmt->branches[0].body[0].get());
  const auto *inner_ident = expect_ident_expr(inner_expr->expr.get());
  const auto *final_expr = expect_expr_stmt(run->body_stmts[3].get());
  const auto *final_ident = expect_ident_expr(final_expr->expr.get());

  const auto inner_scope = find_node_scope_or_fail(session, *inner_ident);
  const auto final_scope = find_node_scope_or_fail(session, *final_ident);

  const auto *inner_symbol = cinder::semantic::resolve_symbol(
      session, inner_scope, cinder::semantic::symbol_namespace::value_namespace,
      "value");
  const auto *final_symbol = cinder::semantic::resolve_symbol(
      session, final_scope, cinder::semantic::symbol_namespace::value_namespace,
      "value");

  expect(inner_symbol != nullptr, "expected inner value lookup to resolve");
  expect(final_symbol != nullptr, "expected final value lookup to resolve");
  expect(inner_symbol->location.span.start != final_symbol->location.span.start,
         "expected final lookup to resolve the shadowing binding");
}

auto test_resolve_function_parameters_and_locals() -> void {
  const auto parsed = parse_sources({
      load_test_data_fixture("resolve_function_parameters_and_locals.cn"),
  });

  const auto session =
      cinder::semantic::build_semantic_session(parsed.parsed_modules);
  const auto *file = parsed.ast_files.front().get();
  const auto *run = expect_func_decl(file->items[0].get());
  const auto *current_expr = expect_expr_stmt(run->body_stmts[1].get());
  const auto *current_ident = expect_ident_expr(current_expr->expr.get());
  const auto *let_stmt = expect_let_stmt(run->body_stmts[0].get());
  const auto *input_ident = expect_ident_expr(let_stmt->initializer.get());

  const auto current_scope = find_node_scope_or_fail(session, *current_ident);
  const auto initializer_scope = find_node_scope_or_fail(session, *input_ident);

  const auto *current_symbol = cinder::semantic::resolve_symbol(
      session, current_scope, cinder::semantic::symbol_namespace::value_namespace,
      "current");
  const auto *input_symbol = cinder::semantic::resolve_symbol(
      session, initializer_scope,
      cinder::semantic::symbol_namespace::value_namespace, "input");

  expect(current_symbol != nullptr, "expected local binding to resolve");
  expect(current_symbol->kind ==
             cinder::semantic::semantic_symbol_kind::local_binding_symbol,
         "expected local binding symbol kind");
  expect(input_symbol != nullptr,
         "expected parameter to resolve in initializer");
  expect(input_symbol->kind ==
             cinder::semantic::semantic_symbol_kind::parameter_symbol,
         "expected parameter symbol kind");
}

auto test_match_arm_pattern_bindings_are_arm_local() -> void {
  const auto parsed = parse_sources({
      load_test_data_fixture("match_arm_pattern_bindings_are_arm_local.cn"),
  });

  const auto session =
      cinder::semantic::build_semantic_session(parsed.parsed_modules);
  const auto *file = parsed.ast_files.front().get();
  const auto *run = expect_func_decl(file->items[1].get());
  const auto *match = expect_match_stmt(run->body_stmts[0].get());
  const auto *found_expr = expect_ident_expr(match->arms[0].body_expr.get());
  const auto *value_expr = expect_ident_expr(match->arms[1].body_expr.get());

  const auto found_scope = find_node_scope_or_fail(session, *found_expr);
  const auto value_scope = find_node_scope_or_fail(session, *value_expr);

  const auto *found_symbol = cinder::semantic::resolve_symbol(
      session, found_scope, cinder::semantic::symbol_namespace::value_namespace,
      "found");
  const auto *value_symbol = cinder::semantic::resolve_symbol(
      session, value_scope, cinder::semantic::symbol_namespace::value_namespace,
      "value");

  expect(found_symbol != nullptr, "expected match arm binding to resolve");
  expect(found_symbol->kind ==
             cinder::semantic::semantic_symbol_kind::pattern_binding_symbol,
         "expected match arm binding symbol kind");
  expect(value_symbol != nullptr,
         "expected non-pattern name to resolve through outer scopes");
  expect(value_symbol->kind ==
             cinder::semantic::semantic_symbol_kind::parameter_symbol,
         "expected match fallback name to resolve to parameter");
}

auto test_lambda_parameters_shadow_outer_bindings() -> void {
  const auto parsed = parse_sources({
      load_test_data_fixture("lambda_parameters_shadow_outer_bindings.cn"),
  });

  const auto session =
      cinder::semantic::build_semantic_session(parsed.parsed_modules);
  const auto *file = parsed.ast_files.front().get();
  const auto *run = expect_func_decl(file->items[0].get());
  const auto *reader_let = expect_let_stmt(run->body_stmts[1].get());
  const auto *lambda = expect_lambda_expr(reader_let->initializer.get());
  const auto *lambda_ident = expect_ident_expr(lambda->body_expr.get());

  const auto lambda_scope = find_node_scope_or_fail(session, *lambda_ident);
  const auto *resolved = cinder::semantic::resolve_symbol(
      session, lambda_scope, cinder::semantic::symbol_namespace::value_namespace,
      "value");

  expect(resolved != nullptr, "expected lambda body name to resolve");
  expect(resolved->kind ==
             cinder::semantic::semantic_symbol_kind::parameter_symbol,
         "expected lambda parameter to shadow outer binding");
}

} // namespace

auto main() -> int {
  try {
    test_build_semantic_session_indexes_module_symbols();
    test_resolve_value_name_shadowing_in_nested_blocks();
    test_resolve_function_parameters_and_locals();
    test_match_arm_pattern_bindings_are_arm_local();
    test_lambda_parameters_shadow_outer_bindings();
    test_scope_walk_reaches_nested_expressions_and_static_for();
  } catch (const std::exception &ex) {
    std::cerr << "resolution_test failed: unhandled exception: " << ex.what()
              << '\n';
    std::exit(1);
  }
  return 0;
}
