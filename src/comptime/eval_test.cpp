#include <string>

#include "src/comptime/eval.h"
#include "src/parser/lexer.h"
#include "src/parser/parser.h"
#include "src/testing/test_assert.h"

namespace {

using kira::testing::expect;
using kira::testing::fail;

/// Parses `text` as a standalone expression (wrapped in a throwaway module
/// so the ordinary file-level parser entry point can be reused) and
/// evaluates it, asserting parsing produced no diagnostics of its own so a
/// test failure can only be attributed to evaluation.
auto eval_source(std::string_view expr_text) -> kira::comptime::value {
  const auto source =
      std::string("module sample\n\ndef run():\n  let result = ") +
      std::string(expr_text) + "\n";

  kira::diagnostic_bag parse_diag;
  auto sources = kira::source_manager{};
  const auto file_id = sources.add_file("eval_test.kira", source);
  expect(file_id.has_value(), "expected eval test source to register");
  const auto *file = sources.get(*file_id);
  expect(file != nullptr, "expected registered eval test source");

  auto lexer = kira::lexer(file->source(), file->id(), parse_diag);
  auto tokens = lexer.tokenize();
  auto parser = kira::parser(std::move(tokens), file->id(), parse_diag);
  auto ast_file = parser.parse_file();
  expect(!parse_diag.has_errors(), "expected eval test source to parse");

  auto *run_func =
      dynamic_cast<kira::ast::func_decl *>(ast_file->items[0].get());
  expect(run_func != nullptr, "expected a run function");
  auto *let_result =
      dynamic_cast<kira::ast::let_stmt *>(run_func->body_stmts[0].get());
  expect(let_result != nullptr, "expected the let-result statement");

  kira::diagnostic_bag eval_diag;
  auto eval = kira::comptime::evaluator(eval_diag, file->id());
  return eval.evaluate(*let_result->initializer);
}

auto test_eval_integer_arithmetic() -> void {
  const auto result = eval_source("2 + 3 * 4");
  expect(result.kind == kira::comptime::value_kind::integer,
         "expected an integer result");
  expect(result.integer == 14, "expected `2 + 3 * 4` to evaluate to 14");
}

auto test_eval_float_arithmetic() -> void {
  const auto result = eval_source("1.5 + 2.5");
  expect(result.kind == kira::comptime::value_kind::floating,
         "expected a floating result");
  expect(result.floating == 4.0, "expected `1.5 + 2.5` to evaluate to 4.0");
}

auto test_eval_comparison_and_logical() -> void {
  const auto result = eval_source("(1 < 2) and (3 >= 3)");
  expect(result.kind == kira::comptime::value_kind::boolean,
         "expected a boolean result");
  expect(result.boolean, "expected `(1 < 2) and (3 >= 3)` to be true");
}

auto test_eval_unary_negation() -> void {
  const auto result = eval_source("-(2 + 3)");
  expect(result.kind == kira::comptime::value_kind::integer,
         "expected an integer result");
  expect(result.integer == -5, "expected `-(2 + 3)` to evaluate to -5");
}

auto test_eval_division_by_zero_reports_error() -> void {
  const auto result = eval_source("1 / 0");
  expect(result.is_error(),
         "expected division by zero to produce the error sentinel");
}

auto test_eval_string_equality() -> void {
  const auto result = eval_source(R"("abc" == "abc")");
  expect(result.kind == kira::comptime::value_kind::boolean,
         "expected a boolean result");
  expect(result.boolean, "expected equal string literals to compare equal");
}

auto test_eval_global_binding_reference() -> void {
  kira::diagnostic_bag diag;
  auto eval = kira::comptime::evaluator(diag, 0);
  eval.bind_global("limit", kira::comptime::value::make_int(100));

  kira::diagnostic_bag parse_diag;
  auto sources = kira::source_manager{};
  const auto file_id = sources.add_file(
      "global.kira", "module sample\n\ndef run():\n  let result = limit + 1\n");
  const auto *file = sources.get(*file_id);
  auto lexer = kira::lexer(file->source(), file->id(), parse_diag);
  auto tokens = lexer.tokenize();
  auto parser = kira::parser(std::move(tokens), file->id(), parse_diag);
  auto ast_file = parser.parse_file();
  expect(!parse_diag.has_errors(), "expected global-reference source to parse");

  auto *run_func =
      dynamic_cast<kira::ast::func_decl *>(ast_file->items[0].get());
  auto *let_result =
      dynamic_cast<kira::ast::let_stmt *>(run_func->body_stmts[0].get());
  const auto result = eval.evaluate(*let_result->initializer);
  expect(result.kind == kira::comptime::value_kind::integer,
         "expected an integer result");
  expect(result.integer == 101,
         "expected `limit + 1` to resolve the bound global and evaluate to "
         "101");
}

auto test_eval_quote_boxes_matching_fragment_kind() -> void {
  const auto result = eval_source("`(1 + 2)`");
  expect(result.kind == kira::comptime::value_kind::expr_fragment,
         "expected a quoted `(1 + 2)` to box as an `expr_fragment`, matching "
         "the parser's own `quote_fragment_kind::expr` classification");
  expect(result.fragment != nullptr,
         "expected the boxed fragment to point at the parsed sub-AST");
}

} // namespace

auto main() -> int {
  test_eval_integer_arithmetic();
  test_eval_float_arithmetic();
  test_eval_comparison_and_logical();
  test_eval_unary_negation();
  test_eval_division_by_zero_reports_error();
  test_eval_string_equality();
  test_eval_global_binding_reference();
  test_eval_quote_boxes_matching_fragment_kind();
  return 0;
}
