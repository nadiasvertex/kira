#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "src/hir/lower.h"
#include "src/hir/nodes.h"
#include "src/k-parser/diagnostic.h"
#include "src/k-parser/lexer.h"
#include "src/k-parser/parser.h"
#include "src/k-parser/source_location.h"
#include "src/semantic/analysis.h"
#include "src/semantic/check.h"
#include "src/semantic/types.h"
#include "src/testing/test_assert.h"

namespace {

using kira::testing::expect;
using kira::testing::fail;
namespace hir = kira::hir;

// Parses and checks one fixture source, returning the AST root, the checked
// types, and the requested function's declaration for the caller to feed
// into `hir::lower_function`/`hir::lower_module`.
struct checked_fixture {
  kira::source_manager sources;
  kira::diagnostic_bag diag{};
  kira::ast::ptr<kira::ast::file> ast_file;
  kira::semantic::checked_types checked;
};

auto check_fixture(const std::string &text) -> checked_fixture {
  auto fixture = checked_fixture{};
  const auto file_id = fixture.sources.add_file("sample.kira", text);
  expect(file_id.has_value(), "expected fixture source to register");

  const auto *file = fixture.sources.get(*file_id);
  expect(file != nullptr, "expected registered fixture source");

  auto lexer = kira::lexer(file->source(), file->id(), fixture.diag);
  auto tokens = lexer.tokenize();
  auto parser = kira::parser(std::move(tokens), file->id(), fixture.diag);
  fixture.ast_file = parser.parse_file();
  expect(fixture.diag.error_count() == 0, "expected fixture to parse cleanly");

  auto file_has_errors =
      std::vector<bool>(static_cast<size_t>(*file_id) + 1, false);
  const auto parsed_modules = std::vector<kira::semantic::parsed_module>{
      kira::semantic::parsed_module{.file_id = *file_id,
                                    .ast_file = fixture.ast_file.get()},
  };
  fixture.checked = kira::semantic::check_program(parsed_modules, fixture.diag,
                                                  file_has_errors);
  expect(fixture.diag.error_count() == 0, "expected fixture to check cleanly");
  return fixture;
}

auto find_func(const kira::ast::file &file, std::string_view name)
    -> const kira::ast::func_decl & {
  for (const auto &item : file.items) {
    if (item != nullptr && item->kind == kira::ast::node_kind::func_decl) {
      const auto &decl = static_cast<const kira::ast::func_decl &>(*item);
      if (decl.name == name) {
        return decl;
      }
    }
  }
  fail("expected to find a function declaration by name");
}

auto test_lowers_fully_annotated_function() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def add(x: int32, y: int32) -> int32:\n"
                               "    return x + y\n");
  const auto &decl = find_func(*fixture.ast_file, "add");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a fully-annotated function to lower");

  const auto &function = **result;
  expect(function.name == "add", "expected lowered function name to survive");
  expect(function.params.size() == 2, "expected two lowered parameters");

  const auto int32_type = fixture.checked.types.builtin("int32");
  expect(function.params[0].type == int32_type,
         "expected parameter x to be resolved to int32");
  expect(function.params[1].type == int32_type,
         "expected parameter y to be resolved to int32");
  expect(function.return_type == int32_type,
         "expected the declared return type to be resolved to int32");

  expect(function.body->stmts.size() == 1,
         "expected the function body to hold one statement");
  const auto *stmt = function.body->stmts.front().get();
  expect(stmt->kind == hir::hir_node_kind::hir_return,
         "expected the lone statement to be a hir_return");
  const auto &ret = static_cast<const hir::hir_return &>(*stmt);
  expect(ret.value != nullptr, "expected return to carry a value");
  expect(ret.value->kind == hir::hir_node_kind::hir_binary,
         "expected the returned value to be a hir_binary");
  expect(ret.value->type == int32_type,
         "expected the sum's checked type to be int32");

  const auto &sum = static_cast<const hir::hir_binary &>(*ret.value);
  expect(sum.lhs->kind == hir::hir_node_kind::hir_local_ref,
         "expected the left operand to be a local reference");
  expect(sum.rhs->kind == hir::hir_node_kind::hir_local_ref,
         "expected the right operand to be a local reference");
  const auto &lhs = static_cast<const hir::hir_local_ref &>(*sum.lhs);
  const auto &rhs = static_cast<const hir::hir_local_ref &>(*sum.rhs);
  expect(lhs.name == "x", "expected the left operand to reference x");
  expect(rhs.name == "y", "expected the right operand to reference y");
  expect(lhs.symbol != rhs.symbol,
         "expected distinct parameters to receive distinct lowering-local ids");
}

auto test_rejects_unannotated_parameter_with_specific_error() -> void {
  // Per spec/kira-reference.md, `x` here is legally unannotated and the
  // checker infers its usage type locally — but the first lowering
  // milestone explicitly only lowers *explicitly* annotated signatures
  // (spec/typed-ir-design.md Decision 1), so this must be rejected, not
  // silently skipped or lowered with a guessed type.
  auto fixture = check_fixture("module sample\n"
                               "def double(x) -> int32:\n"
                               "    return x * 2\n");
  const auto &decl = find_func(*fixture.ast_file, "double");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(!result.has_value(),
         "expected a function with an unannotated parameter to be rejected");
  expect(result.error().kind == hir::lowering_error_kind::unannotated_parameter,
         "expected the specific unannotated_parameter error kind");
}

auto test_rejects_function_without_declared_return_type() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def add(x: int32, y: int32):\n"
                               "    return x + y\n");
  const auto &decl = find_func(*fixture.ast_file, "add");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(!result.has_value(),
         "expected a function with no declared return type to be rejected");
  expect(result.error().kind == hir::lowering_error_kind::missing_return_type,
         "expected the specific missing_return_type error kind");
}

auto test_preserves_source_spans() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def add(x: int32, y: int32) -> int32:\n"
                               "    return x + y\n");
  const auto &decl = find_func(*fixture.ast_file, "add");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a fully-annotated function to lower");

  const auto &function = **result;
  expect(function.span.start == decl.span.start &&
             function.span.end == decl.span.end,
         "expected the function's span to match its originating declaration");

  const auto &ret =
      static_cast<const hir::hir_return &>(*function.body->stmts.front());
  const auto &return_stmt =
      static_cast<const kira::ast::return_stmt &>(*decl.body_stmts.front());
  expect(
      ret.span.start == return_stmt.span.start &&
          ret.span.end == return_stmt.span.end,
      "expected the lowered return statement's span to match the AST return");
  expect(ret.value->span.start == return_stmt.value->span.start &&
             ret.value->span.end == return_stmt.value->span.end,
         "expected the lowered sum's span to match the AST `x + y`");
}

auto test_lowers_compact_expression_body() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def identity(x: int32) -> int32: x\n");
  const auto &decl = find_func(*fixture.ast_file, "identity");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a compact-body function to lower");

  const auto &function = **result;
  expect(function.body->stmts.size() == 1,
         "expected the compact body to lower to a single implicit return");
  expect(function.body->stmts.front()->kind == hir::hir_node_kind::hir_return,
         "expected the compact body's value to become a hir_return");
}

auto test_lowers_if_expression_and_module() -> void {
  const auto fixture = check_fixture("module sample\n"
                                     "def max2(a: int32, b: int32) -> int32:\n"
                                     "    return (if a > b: a else: b)\n");

  auto module_result =
      hir::lower_module(*fixture.ast_file, "sample", fixture.checked);
  expect(module_result.has_value(), "expected the module to lower");
  expect((*module_result)->functions.size() == 1,
         "expected exactly one lowered function in the module");
  expect((*module_result)->functions.front()->name == "max2",
         "expected the lowered function to be max2");

  const auto &function = *(*module_result)->functions.front();
  const auto &ret =
      static_cast<const hir::hir_return &>(*function.body->stmts.front());
  expect(ret.value->kind == hir::hir_node_kind::hir_if,
         "expected the returned value to be a hir_if");
  const auto &if_expr = static_cast<const hir::hir_if &>(*ret.value);
  expect(if_expr.branches.size() == 1, "expected a single if branch");
  expect(if_expr.else_body != nullptr, "expected an else block to be present");
}

auto test_rejects_generic_function() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def identity[T](x: T) -> T:\n"
                               "    return x\n");
  const auto &decl = find_func(*fixture.ast_file, "identity");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(!result.has_value(), "expected a generic function to be rejected");
  expect(result.error().kind == hir::lowering_error_kind::unsupported_construct,
         "expected the specific unsupported_construct error kind");
}

} // namespace

auto main() -> int {
  try {
    test_lowers_fully_annotated_function();
    test_rejects_unannotated_parameter_with_specific_error();
    test_rejects_function_without_declared_return_type();
    test_preserves_source_spans();
    test_lowers_compact_expression_body();
    test_lowers_if_expression_and_module();
    test_rejects_generic_function();
  } catch (const std::exception &ex) {
    std::cerr << "lower_test failed: unhandled exception: " << ex.what()
              << '\n';
    std::exit(1);
  }
  return 0;
}
