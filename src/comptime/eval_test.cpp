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

auto test_eval_match_expr_literal_pattern_selects_true_arm() -> void {
  // Regression check: `bind_pattern` had no case for `literal_pattern` at
  // all, so a compact `true => ...` / `false => ...` arm never matched its
  // subject and silently fell through to whatever `_` arm followed —
  // `match true: true => 1 / _ => 2` used to evaluate to 2, not 1, with no
  // diagnostic anywhere in the chain.
  const auto result = eval_source("match true:\n"
                                  "    true => 1\n"
                                  "    _ => 2\n");
  expect(result.kind == kira::comptime::value_kind::integer,
         "expected an integer result");
  expect(result.integer == 1,
         "expected `match true: true => 1 / _ => 2` to select the `true` "
         "arm and evaluate to 1");
}

auto test_eval_match_expr_block_arm_tail_value() -> void {
  // A block-form arm (`pattern => :` followed by an indented body — see
  // `parser::parse_match_arm`) that ends in a bare tail expression rather
  // than an explicit `return` must still produce the match's value —
  // mirroring `checker::node_provides_function_value`'s "a tail expression
  // implicitly provides the block's value" rule.
  const auto result = eval_source("match 1:\n"
                                  "    1 => :\n"
                                  "        2\n"
                                  "    _ => :\n"
                                  "        3\n");
  expect(result.kind == kira::comptime::value_kind::integer,
         "expected an integer result");
  expect(result.integer == 2,
         "expected the `1 => :` block arm's tail expression `2` to become "
         "the match's value");
}

auto test_eval_if_expr_tail_value_without_return() -> void {
  // Same tail-value rule for `if`/`else` branches used in expression
  // position (e.g. `return if cond: a else: b`). Deliberately the
  // single-line inline form (`if cond: expr else: expr`) rather than a
  // multi-line indented block: `parser::parse_if_expr`'s multi-line block
  // body for an if-*expression* (as opposed to an `if` statement) does not
  // currently parse at all (a separate, pre-existing parser gap, not
  // something this evaluator change should paper over) — but the inline
  // form still desugars to a one-statement block (`wrap_inline_body`) whose
  // single statement is a bare tail expression, so it already exercises
  // the same `evaluate_tail` path.
  const auto result = eval_source("if false: 1 else: 2");
  expect(result.kind == kira::comptime::value_kind::integer,
         "expected an integer result");
  expect(result.integer == 2,
         "expected the `else` branch's tail expression `2` to become the "
         "`if` expression's value");
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

auto test_eval_cast_int_narrowing_wraps_like_runtime() -> void {
  const auto result = eval_source("300 as int8");
  expect(result.kind == kira::comptime::value_kind::integer,
         "expected an integer result");
  expect(result.integer == 44,
         "expected `300 as int8` to truncate like a runtime cast (300 mod "
         "256 - 256 = 44)");
}

auto test_eval_cast_negative_to_unsigned_zero_extends() -> void {
  // `as` binds tighter than unary `-` (it's parsed as a postfix suffix), so
  // the negation must be parenthesized to apply to the whole value before
  // the cast rather than to `1 as uint8` first.
  const auto result = eval_source("(-1) as uint8");
  expect(result.kind == kira::comptime::value_kind::integer,
         "expected an integer result");
  expect(result.integer == 255,
         "expected `(-1) as uint8` to reinterpret as the unsigned pattern "
         "255");
}

auto test_eval_cast_int_to_float() -> void {
  const auto result = eval_source("5 as float64");
  expect(result.kind == kira::comptime::value_kind::floating,
         "expected a floating result");
  expect(result.floating == 5.0, "expected `5 as float64` to evaluate to 5.0");
}

auto test_eval_cast_float_to_int_truncates_toward_zero() -> void {
  const auto result = eval_source("3.9 as int32");
  expect(result.kind == kira::comptime::value_kind::integer,
         "expected an integer result");
  expect(result.integer == 3,
         "expected `3.9 as int32` to truncate toward zero");
}

auto test_eval_cast_through_bound_generic_type_param() -> void {
  // Regression check for todo item 12: `static def min[T]()` needs
  // `<value> as T` to resolve `T` to whatever scalar the call site bound it
  // to, not fail as an unsupported cast target.
  kira::diagnostic_bag diag;
  auto eval = kira::comptime::evaluator(diag, 0);
  eval.push_locals(
      {{"T", kira::comptime::value::make_type_value("int32", nullptr)}});

  kira::diagnostic_bag parse_diag;
  auto sources = kira::source_manager{};
  const auto file_id = sources.add_file(
      "cast_generic.kira",
      "module sample\n\ndef run():\n  let result = 300 as T\n");
  const auto *file = sources.get(*file_id);
  auto lexer = kira::lexer(file->source(), file->id(), parse_diag);
  auto tokens = lexer.tokenize();
  auto parser = kira::parser(std::move(tokens), file->id(), parse_diag);
  auto ast_file = parser.parse_file();
  expect(!parse_diag.has_errors(), "expected cast-generic source to parse");

  auto *run_func =
      dynamic_cast<kira::ast::func_decl *>(ast_file->items[0].get());
  auto *let_result =
      dynamic_cast<kira::ast::let_stmt *>(run_func->body_stmts[0].get());
  const auto result = eval.evaluate(*let_result->initializer);
  eval.pop_locals();

  expect(result.kind == kira::comptime::value_kind::integer,
         "expected an integer result");
  expect(result.integer == 300,
         "expected `300 as T` with `T` bound to `int32` to pass 300 through "
         "unchanged (it fits in int32)");
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
  test_eval_match_expr_literal_pattern_selects_true_arm();
  test_eval_match_expr_block_arm_tail_value();
  test_eval_if_expr_tail_value_without_return();
  test_eval_global_binding_reference();
  test_eval_cast_int_narrowing_wraps_like_runtime();
  test_eval_cast_negative_to_unsigned_zero_extends();
  test_eval_cast_int_to_float();
  test_eval_cast_float_to_int_truncates_toward_zero();
  test_eval_cast_through_bound_generic_type_param();
  test_eval_quote_boxes_matching_fragment_kind();
  return 0;
}
