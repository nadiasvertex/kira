#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "src/hir/captures.h"
#include "src/hir/lower.h"
#include "src/hir/nodes.h"
#include "src/parser/diagnostic.h"
#include "src/parser/lexer.h"
#include "src/parser/parser.h"
#include "src/parser/source_location.h"
#include "src/semantic/analysis.h"
#include "src/semantic/check.h"
#include "src/semantic/types.h"
#include "src/testing/test_assert.h"

namespace {

using kira::testing::expect;
using kira::testing::fail;
namespace hir = kira::hir;

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
      const auto &decl = dynamic_cast<const kira::ast::func_decl &>(*item);
      if (decl.name == name) {
        return decl;
      }
    }
  }
  fail("expected to find a function declaration by name");
}

/// Lowers `fn_name`, returning the owning `hir_function` together with the
/// `hir_lambda` its first statement's `hir_let` initializes — every
/// fixture below is shaped `let f = <lambda>` followed by uses of `f`,
/// mirroring `lower_test.cpp::test_lowers_lambda_expression`. Lowering
/// mints fresh symbol ids every call, so callers that need to compare a
/// captured symbol against the enclosing function's own parameter symbol
/// must get both from the same lowering pass rather than lowering twice.
struct lowered_lambda {
  hir::ptr<hir::hir_function> function;
  const hir::hir_lambda *lambda = nullptr;
};

auto lower_first_lambda(const checked_fixture &fixture,
                        std::string_view fn_name) -> lowered_lambda {
  const auto &decl = find_func(*fixture.ast_file, fn_name);
  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected the fixture function to lower");
  const auto &let =
      dynamic_cast<const hir::hir_let &>(*(*result)->body->stmts.front());
  const auto *lambda =
      dynamic_cast<const hir::hir_lambda *>(let.initializer.get());
  return lowered_lambda{.function = std::move(*result), .lambda = lambda};
}

auto test_non_capturing_lambda_has_no_free_variables() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def make() -> fn(int32) -> int32:\n"
                               "    let f = pure (x: int32) -> int32 => x * 2\n"
                               "    return f\n");
  const auto lowered = lower_first_lambda(fixture, "make");
  const auto free = hir::free_variables(*lowered.lambda);
  expect(free.empty(), "expected a lambda using only its own parameter to "
                       "capture nothing");
}

auto test_lambda_captures_an_outer_parameter() -> void {
  auto fixture =
      check_fixture("module sample\n"
                    "def make_adder(n: int32) -> fn(int32) -> int32:\n"
                    "    let f = pure (x: int32) -> int32 => x + n\n"
                    "    return f\n");
  const auto lowered = lower_first_lambda(fixture, "make_adder");

  const auto free = hir::free_variables(*lowered.lambda);
  expect(free.size() == 1, "expected exactly one captured variable");

  // Confirm the captured symbol really is the outer `n` parameter, not the
  // lambda's own `x` — the enclosing function's only parameter.
  expect(lowered.function->params.size() == 1,
         "expected make_adder to have one parameter");
  expect(free[0] == lowered.function->params[0].symbol,
         "expected the captured symbol to be the outer parameter `n`");
}

auto test_locally_bound_names_are_not_captured() -> void {
  auto fixture =
      check_fixture("module sample\n"
                    "def make_adder(n: int32) -> fn(int32) -> int32:\n"
                    "    let f = pure (x: int32) -> int32 =>:\n"
                    "        let y = x + 1\n"
                    "        y + n\n"
                    "    return f\n");
  const auto lowered = lower_first_lambda(fixture, "make_adder");
  const auto free = hir::free_variables(*lowered.lambda);
  expect(free.size() == 1,
         "expected only `n` to be captured — `x` and `y` are both bound "
         "inside the lambda's own body");
}

} // namespace

auto main() -> int {
  try {
    test_non_capturing_lambda_has_no_free_variables();
    test_lambda_captures_an_outer_parameter();
    test_locally_bound_names_are_not_captured();
  } catch (const std::exception &ex) {
    std::cerr << "captures_test failed: unhandled exception: " << ex.what()
              << '\n';
    std::exit(1);
  }
  return 0;
}
