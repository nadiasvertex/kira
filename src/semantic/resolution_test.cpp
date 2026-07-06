#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/semantic/module_index.h"

#include "src/k-parser/parser.h"

namespace {

struct source_fixture {
  std::string path;
  std::string text;
};

struct parsed_fixture {
  kira::source_manager sources;
  kira::diagnostic_bag diag;
  std::vector<kira::ast::ptr<kira::ast::file>> ast_files;
  std::vector<kira::semantic::parsed_module> parsed_modules;
};

[[noreturn]] auto fail(std::string_view message) -> void {
  std::cerr << "resolution_test failed: " << message << '\n';
  std::exit(1);
}

auto expect(bool condition, std::string_view message) -> void {
  if (!condition) {
    fail(message);
  }
}

auto parse_sources(const std::vector<source_fixture> &fixtures) -> parsed_fixture {
  auto parsed = parsed_fixture{
      .sources = kira::source_manager{},
      .diag = kira::diagnostic_bag{},
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

    auto lexer = kira::lexer(file->source(), file->id(), parsed.diag);
    auto tokens = lexer.tokenize();
    auto parser = kira::parser(std::move(tokens), file->id(), parsed.diag);
    auto ast_file = parser.parse_file();

    parsed.parsed_modules.push_back(kira::semantic::parsed_module{
        .file_id = *file_id,
        .ast_file = ast_file.get(),
    });
    parsed.ast_files.push_back(std::move(ast_file));
  }

  expect(parsed.diag.error_count() == 0,
         "expected resolution test fixtures to parse cleanly");
  return parsed;
}

auto expect_expr_stmt(const kira::ast::node *node) -> const kira::ast::expr_stmt * {
  expect(node != nullptr, "expected expression statement node");
  expect(node->kind == kira::ast::node_kind::expr_stmt,
         "expected expression statement node kind");
  return dynamic_cast<const kira::ast::expr_stmt *>(node);
}

auto expect_func_decl(const kira::ast::node *node) -> const kira::ast::func_decl * {
  expect(node != nullptr, "expected function declaration node");
  expect(node->kind == kira::ast::node_kind::func_decl,
         "expected function declaration node kind");
  return dynamic_cast<const kira::ast::func_decl *>(node);
}

auto expect_if_stmt(const kira::ast::node *node) -> const kira::ast::if_stmt * {
  expect(node != nullptr, "expected if statement node");
  expect(node->kind == kira::ast::node_kind::if_stmt,
         "expected if statement node kind");
  return dynamic_cast<const kira::ast::if_stmt *>(node);
}

auto expect_match_stmt(const kira::ast::node *node) -> const kira::ast::match_stmt * {
  expect(node != nullptr, "expected match statement node");
  expect(node->kind == kira::ast::node_kind::match_stmt,
         "expected match statement node kind");
  return dynamic_cast<const kira::ast::match_stmt *>(node);
}

auto expect_lambda_expr(const kira::ast::expr *expr) -> const kira::ast::lambda_expr * {
  expect(expr != nullptr, "expected lambda expression");
  expect(expr->kind == kira::ast::node_kind::lambda_expr,
         "expected lambda expression kind");
  return dynamic_cast<const kira::ast::lambda_expr *>(expr);
}

auto expect_let_stmt(const kira::ast::node *node) -> const kira::ast::let_stmt * {
  expect(node != nullptr, "expected let statement node");
  expect(node->kind == kira::ast::node_kind::let_stmt,
         "expected let statement node kind");
  return dynamic_cast<const kira::ast::let_stmt *>(node);
}

auto expect_ident_expr(const kira::ast::expr *expr) -> const kira::ast::ident_expr * {
  expect(expr != nullptr, "expected identifier expression");
  expect(expr->kind == kira::ast::node_kind::ident_expr,
         "expected identifier expression kind");
  return dynamic_cast<const kira::ast::ident_expr *>(expr);
}

auto find_node_scope_or_fail(const kira::semantic::semantic_session &session,
                             const kira::ast::node &node) -> kira::semantic::scope_id {
  const auto scope = kira::semantic::find_node_scope(session, node);
  if (!scope.has_value()) {
    fail("expected node scope to be recorded");
  }
  return *scope;
}

auto test_build_semantic_session_indexes_module_symbols() -> void {
  const auto parsed = parse_sources({{
      .path = "sample_tools.kira",
      .text = "module sample.tools\n"
              "type point = int32\n"
              "trait show:\n"
              "  def show(self) -> str\n"
              "module inner\n"
              "pub def run():\n"
              "  return 1\n"
              "static limit = 4\n",
  }});

  const auto session = kira::semantic::build_semantic_session(parsed.parsed_modules);
  const auto index = kira::semantic::build_semantic_resolution_index(parsed.parsed_modules);

  const auto *module_scope =
      kira::semantic::find_module_scope(index, "sample.tools");
  expect(module_scope != nullptr, "expected module scope to exist");
  expect(module_scope->symbols.size() == 5,
         "expected five direct module symbols to be indexed");

  const auto *type_symbol =
      kira::semantic::find_module_scope_symbol(index, "sample.tools", "point");
  expect(type_symbol != nullptr, "expected type symbol lookup to succeed");
  expect(type_symbol->kind == kira::semantic::semantic_symbol_kind::type_symbol,
         "expected type symbol kind");

  const auto *function_symbol =
      kira::semantic::find_module_scope_symbol(index, "sample.tools", "run");
  expect(function_symbol != nullptr,
         "expected function symbol lookup to succeed");
  expect(function_symbol->name_space ==
             kira::semantic::symbol_namespace::value_namespace,
         "expected function to live in value namespace");

  const auto *inner_scope = kira::semantic::find_module_scope(index, "sample.tools.inner");
  expect(inner_scope == nullptr,
         "expected declaration-only submodule without a body to avoid creating a nested scope");

  expect(session.symbols.size() >= 5,
         "expected semantic session to own stable symbol records");
}

auto test_resolve_value_name_shadowing_in_nested_blocks() -> void {
  const auto parsed = parse_sources({{
      .path = "sample.kira",
      .text = "module sample\n"
              "def run():\n"
              "  let value = 1\n"
              "  if true:\n"
              "    value\n"
              "  let value = 2\n"
              "  value\n",
  }});

  const auto session = kira::semantic::build_semantic_session(parsed.parsed_modules);
  const auto *file = parsed.ast_files.front().get();
  const auto *run = expect_func_decl(file->items[0].get());
  const auto *if_stmt = expect_if_stmt(run->body_stmts[1].get());
  const auto *inner_expr = expect_expr_stmt(if_stmt->branches[0].body[0].get());
  const auto *inner_ident = expect_ident_expr(inner_expr->expr.get());
  const auto *final_expr = expect_expr_stmt(run->body_stmts[3].get());
  const auto *final_ident = expect_ident_expr(final_expr->expr.get());

  const auto inner_scope = find_node_scope_or_fail(session, *inner_ident);
  const auto final_scope = find_node_scope_or_fail(session, *final_ident);

  const auto *inner_symbol = kira::semantic::resolve_symbol(
      session, inner_scope, kira::semantic::symbol_namespace::value_namespace,
      "value");
  const auto *final_symbol = kira::semantic::resolve_symbol(
      session, final_scope, kira::semantic::symbol_namespace::value_namespace,
      "value");

  expect(inner_symbol != nullptr, "expected inner value lookup to resolve");
  expect(final_symbol != nullptr, "expected final value lookup to resolve");
  expect(inner_symbol->location.span.start != final_symbol->location.span.start,
         "expected final lookup to resolve the shadowing binding");
}

auto test_resolve_function_parameters_and_locals() -> void {
  const auto parsed = parse_sources({{
      .path = "sample.kira",
      .text = "module sample\n"
              "def run(input):\n"
              "  let current = input\n"
              "  current\n",
  }});

  const auto session = kira::semantic::build_semantic_session(parsed.parsed_modules);
  const auto *file = parsed.ast_files.front().get();
  const auto *run = expect_func_decl(file->items[0].get());
  const auto *current_expr = expect_expr_stmt(run->body_stmts[1].get());
  const auto *current_ident = expect_ident_expr(current_expr->expr.get());
  const auto *let_stmt = expect_let_stmt(run->body_stmts[0].get());
  const auto *input_ident = expect_ident_expr(let_stmt->initializer.get());

  const auto current_scope = find_node_scope_or_fail(session, *current_ident);
  const auto initializer_scope = find_node_scope_or_fail(session, *input_ident);

  const auto *current_symbol = kira::semantic::resolve_symbol(
      session, current_scope, kira::semantic::symbol_namespace::value_namespace,
      "current");
  const auto *input_symbol = kira::semantic::resolve_symbol(
      session, initializer_scope,
      kira::semantic::symbol_namespace::value_namespace, "input");

  expect(current_symbol != nullptr, "expected local binding to resolve");
  expect(current_symbol->kind ==
             kira::semantic::semantic_symbol_kind::local_binding_symbol,
         "expected local binding symbol kind");
  expect(input_symbol != nullptr, "expected parameter to resolve in initializer");
  expect(input_symbol->kind == kira::semantic::semantic_symbol_kind::parameter_symbol,
         "expected parameter symbol kind");
}

auto test_match_arm_pattern_bindings_are_arm_local() -> void {
  const auto parsed = parse_sources({{
      .path = "sample.kira",
      .text = "module sample\n"
              "type option[T] = @some(T) | @none\n"
              "def run(value):\n"
              "  match value:\n"
              "    @some(found) => found\n"
              "    @none => value\n",
  }});

  const auto session = kira::semantic::build_semantic_session(parsed.parsed_modules);
  const auto *file = parsed.ast_files.front().get();
  const auto *run = expect_func_decl(file->items[1].get());
  const auto *match = expect_match_stmt(run->body_stmts[0].get());
  const auto *found_expr = expect_ident_expr(match->arms[0].body_expr.get());
  const auto *value_expr = expect_ident_expr(match->arms[1].body_expr.get());

  const auto found_scope = find_node_scope_or_fail(session, *found_expr);
  const auto value_scope = find_node_scope_or_fail(session, *value_expr);

  const auto *found_symbol = kira::semantic::resolve_symbol(
      session, found_scope, kira::semantic::symbol_namespace::value_namespace,
      "found");
  const auto *value_symbol = kira::semantic::resolve_symbol(
      session, value_scope, kira::semantic::symbol_namespace::value_namespace,
      "value");

  expect(found_symbol != nullptr, "expected match arm binding to resolve");
  expect(found_symbol->kind ==
             kira::semantic::semantic_symbol_kind::pattern_binding_symbol,
         "expected match arm binding symbol kind");
  expect(value_symbol != nullptr,
         "expected non-pattern name to resolve through outer scopes");
  expect(value_symbol->kind == kira::semantic::semantic_symbol_kind::parameter_symbol,
         "expected match fallback name to resolve to parameter");
}

auto test_lambda_parameters_shadow_outer_bindings() -> void {
  const auto parsed = parse_sources({{
      .path = "sample.kira",
      .text = "module sample\n"
              "def run():\n"
              "  let value = 1\n"
              "  let reader = value => value\n"
              "  reader\n",
  }});

  const auto session = kira::semantic::build_semantic_session(parsed.parsed_modules);
  const auto *file = parsed.ast_files.front().get();
  const auto *run = expect_func_decl(file->items[0].get());
  const auto *reader_let = expect_let_stmt(run->body_stmts[1].get());
  const auto *lambda = expect_lambda_expr(reader_let->initializer.get());
  const auto *lambda_ident = expect_ident_expr(lambda->body_expr.get());

  const auto lambda_scope = find_node_scope_or_fail(session, *lambda_ident);
  const auto *resolved = kira::semantic::resolve_symbol(
      session, lambda_scope, kira::semantic::symbol_namespace::value_namespace,
      "value");

  expect(resolved != nullptr, "expected lambda body name to resolve");
  expect(resolved->kind == kira::semantic::semantic_symbol_kind::parameter_symbol,
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
  } catch (const std::exception &ex) {
    std::cerr << "resolution_test failed: unhandled exception: " << ex.what() << '\n';
    std::exit(1);
  }
  return 0;
}
