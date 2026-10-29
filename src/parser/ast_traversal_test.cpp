#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "parser.h"
#include "src/parser/ast_traversal.h"
#include "src/testing/test_assert.h"

namespace {

using cinder::testing::expect;
namespace ast = cinder::ast;

struct parsed_source {
  ast::ptr<ast::file> file;
  uint32_t error_count = 0;
};

auto parse_source(std::string_view source) -> parsed_source {
  cinder::diagnostic_bag diag;
  auto sources = cinder::source_manager{};
  auto file_id = sources.add_file("test.cn", std::string(source));
  expect(file_id.has_value(), "expected test source to register");
  auto *file = sources.get(*file_id);
  cinder::lexer lexer(file->source(), file->id(), diag);
  auto tokens = lexer.tokenize();
  cinder::parser parser(std::move(tokens), file->id(), diag);
  auto parsed = parsed_source{.file = parser.parse_file(),
                              .error_count = diag.error_count()};
  if (parsed.error_count != 0) {
    std::cerr << cinder::diagnostic_renderer(sources, false).render_all(diag);
  }
  return parsed;
}

auto count_kind(const ast::node &root, ast::node_kind kind) -> int {
  auto count = 0;
  ast::walk(root, [&](const ast::node &n) -> bool {
    count += n.kind == kind ? 1 : 0;
    return true;
  });
  return count;
}

constexpr std::string_view k_program = R"(module walk_test

def f(a: i32, b: i32) -> i32:
    let x = a + b * 2
    var y = x
    y += 1
    if y > 3:
        return y
    let z = match x:
        0 => 1
        n if n > 2 => n
        _ => 2
    for i in [1, 2, 3]:
        y = y + i
    let g = (v: i32) => v + z
    return g(y)
)";

/// Finds every kind the program contains, so a node kind that
/// `for_each_child` forgot to descend into shows up as a missing count.
auto test_walk_reaches_nested_nodes() -> void {
  const auto parsed = parse_source(k_program);
  expect(parsed.error_count == 0, "walk_test program should parse cleanly");
  const auto &file = *parsed.file;

  expect(count_kind(file, ast::node_kind::func_decl) == 1, "one function");
  expect(count_kind(file, ast::node_kind::binary_expr) == 6,
         "binary expressions in bodies, guards, lambdas and loops are reached");
  expect(count_kind(file, ast::node_kind::match_expr) == 1, "match reached");
  expect(count_kind(file, ast::node_kind::lambda_expr) == 1, "lambda reached");
  expect(count_kind(file, ast::node_kind::for_stmt) == 1, "for reached");
  expect(count_kind(file, ast::node_kind::literal_pattern) == 1,
         "match-arm pattern reached");
  expect(count_kind(file, ast::node_kind::named_type) == 4,
         "parameter, return and lambda parameter types are children");
  expect(count_kind(file, ast::node_kind::call_expr) == 1, "call reached");
}

auto test_walk_can_prune() -> void {
  const auto parsed = parse_source(k_program);
  auto saw_lambda_body_binary = 0;
  auto binaries = 0;
  ast::walk(static_cast<const ast::node &>(*parsed.file),
            [&](const ast::node &n) -> bool {
              if (n.kind == ast::node_kind::lambda_expr) {
                return false;
              }
              binaries += n.kind == ast::node_kind::binary_expr ? 1 : 0;
              return true;
            });
  saw_lambda_body_binary =
      count_kind(*parsed.file, ast::node_kind::binary_expr);
  expect(binaries == saw_lambda_body_binary - 1,
         "returning false skips exactly the lambda's one binary expression");
}

auto test_mutable_walk_can_edit() -> void {
  auto parsed = parse_source(k_program);
  ast::walk(static_cast<ast::node &>(*parsed.file), [](ast::node &n) -> bool {
    if (auto *ident = ast::as<ast::ident_expr>(&n)) {
      if (ident->name == "a") {
        ident->name = "renamed";
      }
    }
    return true;
  });
  auto renamed = 0;
  ast::walk(static_cast<const ast::node &>(*parsed.file),
            [&](const ast::node &n) -> bool {
              if (const auto *ident = ast::as<ast::ident_expr>(&n)) {
                renamed += ident->name == "renamed" ? 1 : 0;
              }
              return true;
            });
  expect(renamed == 1, "mutable walk renamed the one use of `a`");
}

auto test_kind_classes() -> void {
  expect(ast::is_type_kind(ast::node_kind::named_type), "named_type is a type");
  expect(ast::is_pattern_kind(ast::node_kind::or_pattern), "or is a pattern");
  expect(ast::is_expr_kind(ast::node_kind::yield_expr), "yield is an expr");
  expect(ast::is_stmt_kind(ast::node_kind::splice_stmt), "splice is a stmt");
  expect(!ast::is_expr_kind(ast::node_kind::let_stmt), "let is not an expr");
}

auto test_as_rejects_error_placeholders() -> void {
  // `error_expr` borrows the `ident_expr` tag; a tag-only cast would read
  // `name` out of the wrong type.
  ast::error_expr placeholder;
  expect(placeholder.kind == ast::node_kind::ident_expr,
         "error_expr borrows the ident_expr tag");
  expect(ast::as<ast::ident_expr>(static_cast<ast::node *>(&placeholder)) ==
             nullptr,
         "as<> must not treat an error placeholder as an identifier");
  expect(ast::as_kind<ast::ident_expr>(static_cast<ast::node *>(&placeholder),
                                       ast::node_kind::ident_expr) == nullptr,
         "as_kind<> must not either");
  int visited = 0;
  ast::for_each_child(static_cast<const ast::node &>(placeholder),
                      [&](const ast::node &) -> void { ++visited; });
  expect(visited == 0, "an error placeholder has no children");
}

} // namespace

auto main() -> int {
  test_walk_reaches_nested_nodes();
  test_walk_can_prune();
  test_mutable_walk_can_edit();
  test_kind_classes();
  test_as_rejects_error_placeholders();
  return 0;
}
