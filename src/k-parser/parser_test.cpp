#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "parser.h"

namespace {

auto fail(std::string_view message) -> void {
  std::cerr << "parser_test failed: " << message << '\n';
  std::exit(1);
}

auto expect(bool condition, std::string_view message) -> void {
  if (!condition) {
    fail(message);
  }
}

template <typename T>
auto expect_node(kira::ast::node *node, kira::ast::node_kind kind,
                 std::string_view message) -> T * {
  expect(node != nullptr, message);
  expect(node->kind == kind, message);
  return static_cast<T *>(node);
}

template <typename T>
auto expect_expr(kira::ast::expr *expr, kira::ast::node_kind kind,
                 std::string_view message) -> T * {
  expect(expr != nullptr, message);
  expect(expr->kind == kind, message);
  return static_cast<T *>(expr);
}

template <typename T>
auto expect_pattern(kira::ast::pattern *pattern, kira::ast::node_kind kind,
                    std::string_view message) -> T * {
  expect(pattern != nullptr, message);
  expect(pattern->kind == kind, message);
  return static_cast<T *>(pattern);
}

struct parsed_source {
  kira::ast::ptr<kira::ast::file> file;
  std::string diagnostics;
  uint32_t error_count = 0;
};

auto parse_source(std::string_view source) -> parsed_source {
  kira::diagnostic_bag diag;
  kira::source_file file(0, "test.kira", std::string(source));
  kira::Lexer lexer(file.source(), file.id(), diag);
  auto tokens = lexer.tokenize();
  kira::parser parser(std::move(tokens), file.id(), diag);

  parsed_source parsed{
      .file = parser.parse_file(),
      .diagnostics = kira::diagnostic_renderer(file, false).render_all(diag),
      .error_count = diag.error_count(),
  };
  return parsed;
}

auto test_lexer_emits_indent_and_dedent() -> void {
  kira::diagnostic_bag diag;
  std::string source =
      "module sample\n"
      "\n"
      "def run():\n"
      "  let value = 1\n"
      "  return value\n";
  kira::Lexer lexer(source, 0, diag);
  auto tokens = lexer.tokenize();

  expect(!diag.has_errors(), "expected lexer test source to tokenize cleanly");

  const auto indent_count = std::count_if(
      tokens.begin(), tokens.end(), [](const kira::token &token) {
        return token.kind == kira::token_kind::indent;
      });
  const auto dedent_count = std::count_if(
      tokens.begin(), tokens.end(), [](const kira::token &token) {
        return token.kind == kira::token_kind::dedent;
      });

  expect(indent_count == 1, "expected one indent token for function body");
  expect(dedent_count == 1, "expected one dedent token for function body");
}

auto test_parser_builds_type_body_nodes() -> void {
  auto parsed = parse_source(
      "module sample\n"
      "\n"
      "type person = { name: str, age: int }\n"
      "type shape = | circle(float64) | point\n");

  expect(parsed.error_count == 0, parsed.diagnostics);
  expect(parsed.file->items.size() == 2, "expected two top-level type declarations");

  auto *person_decl = expect_node<kira::ast::type_decl>(
      parsed.file->items[0].get(), kira::ast::node_kind::type_decl,
      "expected first item to be a type declaration");
  auto *person_def = expect_node<kira::ast::struct_type_def>(
      person_decl->definition.get(), kira::ast::node_kind::struct_type_def,
      "expected struct type definition node");
  expect(person_def->body.fields.size() == 2,
         "expected struct body to preserve both fields");
  expect(person_def->body.fields[0].name == "name",
         "expected first struct field name");
  expect(person_def->body.fields[1].name == "age",
         "expected second struct field name");

  auto *shape_decl = expect_node<kira::ast::type_decl>(
      parsed.file->items[1].get(), kira::ast::node_kind::type_decl,
      "expected second item to be a type declaration");
  auto *shape_def = expect_node<kira::ast::sum_type_def>(
      shape_decl->definition.get(), kira::ast::node_kind::sum_type_def,
      "expected sum type definition node");
  expect(shape_def->body.variants.size() == 2,
         "expected sum body to preserve both variants");
  expect(shape_def->body.variants[0].name == "circle",
         "expected first sum variant name");
  expect(shape_def->body.variants[0].payload_types.size() == 1,
         "expected payload type for first sum variant");
  expect(shape_def->body.variants[1].name == "point",
         "expected second sum variant name");
}

auto test_parser_preserves_associated_types_where_and_aliases() -> void {
  auto parsed = parse_source(
      "module sample\n"
      "\n"
      "trait iterable:\n"
      "  type item = int\n"
      "\n"
      "impl iterable for int:\n"
      "  type item = int\n"
      "\n"
      "def evaluate(x):\n"
      "  let value = x where:\n"
      "    base = 1\n"
      "  let chosen = match x:\n"
      "    some(item) as alias => alias\n"
      "    _ => value\n"
      "  return chosen\n");

  expect(parsed.error_count == 0, parsed.diagnostics);
  expect(parsed.file->items.size() == 3, "expected trait, impl, and function items");

  auto *trait_decl = expect_node<kira::ast::trait_decl>(
      parsed.file->items[0].get(), kira::ast::node_kind::trait_decl,
      "expected first item to be a trait declaration");
  expect(trait_decl->items.size() == 1,
         "expected trait to preserve associated type item");
  auto *trait_assoc = expect_node<kira::ast::associated_type_decl_node>(
      trait_decl->items[0].get(), kira::ast::node_kind::associated_type_decl_node,
      "expected trait associated type node");
  expect(trait_assoc->value.name == "item",
         "expected trait associated type name");
  expect(trait_assoc->value.default_type != nullptr,
         "expected trait associated type default type");

  auto *impl_decl = expect_node<kira::ast::impl_decl>(
      parsed.file->items[1].get(), kira::ast::node_kind::impl_decl,
      "expected second item to be an impl declaration");
  expect(impl_decl->items.size() == 1,
         "expected impl to preserve associated type item");
  auto *impl_assoc = expect_node<kira::ast::associated_type_def_node>(
      impl_decl->items[0].get(), kira::ast::node_kind::associated_type_def_node,
      "expected impl associated type node");
  expect(impl_assoc->value.name == "item",
         "expected impl associated type name");
  expect(impl_assoc->value.type != nullptr,
         "expected impl associated type definition type");

  auto *func_decl = expect_node<kira::ast::func_decl>(
      parsed.file->items[2].get(), kira::ast::node_kind::func_decl,
      "expected third item to be a function declaration");
  expect(func_decl->body_stmts.size() == 3,
         "expected function block body to preserve statements");

  auto *let_stmt = expect_node<kira::ast::let_stmt>(
      func_decl->body_stmts[0].get(), kira::ast::node_kind::let_stmt,
      "expected let statement in function body");
  auto *where_expr = expect_expr<kira::ast::where_expr>(
      let_stmt->initializer.get(), kira::ast::node_kind::where_expr,
      "expected let initializer to be a where expression");
  expect(where_expr->bindings.size() == 1,
         "expected one where binding");
  expect(where_expr->bindings[0].name == "base",
         "expected where binding name");

  auto *chosen_stmt = expect_node<kira::ast::let_stmt>(
      func_decl->body_stmts[1].get(), kira::ast::node_kind::let_stmt,
      "expected second statement to bind match expression");
  auto *match_expr = expect_expr<kira::ast::match_expr>(
      chosen_stmt->initializer.get(), kira::ast::node_kind::match_expr,
      "expected bound value to be a match expression");
  expect(match_expr->arms.size() == 2, "expected two match arms");

  auto *aliased_pattern = expect_pattern<kira::ast::group_pattern>(
      static_cast<kira::ast::pattern *>(match_expr->arms[0].pattern.get()),
      kira::ast::node_kind::group_pattern,
      "expected first match arm to preserve aliased pattern");
  expect(aliased_pattern->alias.has_value(),
         "expected aliased pattern name to be preserved");
  expect(*aliased_pattern->alias == "alias",
         "expected aliased pattern alias name");

  auto *option_pattern = expect_pattern<kira::ast::option_pattern>(
      aliased_pattern->inner.get(), kira::ast::node_kind::option_pattern,
      "expected inner pattern to remain the original option pattern");
  expect(option_pattern->option_kind == kira::ast::OptionResultKind::Some,
         "expected some-pattern to preserve its kind");

  auto *return_stmt = expect_node<kira::ast::return_stmt>(
      func_decl->body_stmts[2].get(), kira::ast::node_kind::return_stmt,
      "expected return statement in function body");
  auto *return_ident = expect_expr<kira::ast::ident_expr>(
      return_stmt->value.get(), kira::ast::node_kind::ident_expr,
      "expected return value to preserve chosen identifier");
  expect(return_ident->name == "chosen", "expected return identifier name");
}

} // namespace

auto main() -> int {
  test_lexer_emits_indent_and_dedent();
  test_parser_builds_type_body_nodes();
  test_parser_preserves_associated_types_where_and_aliases();
  return 0;
}
