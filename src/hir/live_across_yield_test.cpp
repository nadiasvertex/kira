#include <algorithm>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "src/hir/live_across_yield.h"
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

auto lower_generator(const checked_fixture &fixture, std::string_view fn_name)
    -> hir::ptr<hir::hir_function> {
  const auto &decl = find_func(*fixture.ast_file, fn_name);
  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected the fixture generator to lower");
  expect((*result)->is_generator, "expected the lowered function to be "
                                  "marked as a generator");
  return std::move(*result);
}

auto contains_symbol(const std::vector<hir::symbol_id> &symbols,
                     hir::symbol_id sym) -> bool {
  return std::ranges::find(symbols, sym) != symbols.end();
}

auto test_local_bound_before_yield_and_used_after_is_live() -> void {
  auto fixture =
      check_fixture("module sample\n"
                    "generator def counter() -> some iterator[int32]:\n"
                    "    let a = 1\n"
                    "    yield a\n"
                    "    yield a\n");
  const auto fn = lower_generator(fixture, "counter");
  const auto &let =
      dynamic_cast<const hir::hir_let &>(*fn->body->stmts.front());
  const auto live = hir::live_across_yield(*fn);
  expect(contains_symbol(live, let.symbol),
         "expected `a` (bound before the first yield, read again by the "
         "second) to be live across yield");
}

auto test_local_used_only_before_yield_is_not_live() -> void {
  auto fixture =
      check_fixture("module sample\n"
                    "generator def counter() -> some iterator[int32]:\n"
                    "    let a = 1\n"
                    "    yield a\n");
  const auto fn = lower_generator(fixture, "counter");
  const auto &let =
      dynamic_cast<const hir::hir_let &>(*fn->body->stmts.front());
  const auto live = hir::live_across_yield(*fn);
  expect(!contains_symbol(live, let.symbol),
         "expected `a`, referenced only before the single yield, to not be "
         "live across it");
}

auto test_local_bound_between_two_yields_and_used_after_later_yield_is_live()
    -> void {
  auto fixture =
      check_fixture("module sample\n"
                    "generator def counter() -> some iterator[int32]:\n"
                    "    yield 0\n"
                    "    let b = 1\n"
                    "    yield b\n"
                    "    yield b\n");
  const auto fn = lower_generator(fixture, "counter");
  const auto &let = dynamic_cast<const hir::hir_let &>(*fn->body->stmts[1]);
  const auto live = hir::live_across_yield(*fn);
  expect(contains_symbol(live, let.symbol),
         "expected `b` (bound after the first yield, read again after the "
         "third) to be live across yield");
}

auto test_loop_condition_variable_spanning_yield_is_live() -> void {
  auto fixture = check_fixture("module sample\n"
                               "generator def counter(limit: int32) -> some "
                               "iterator[int32]:\n"
                               "    var n = 0\n"
                               "    while n < limit:\n"
                               "        yield n\n"
                               "        n = n + 1\n");
  const auto fn = lower_generator(fixture, "counter");
  const auto &let = dynamic_cast<const hir::hir_let &>(*fn->body->stmts[0]);
  const auto live = hir::live_across_yield(*fn);
  expect(contains_symbol(live, let.symbol),
         "expected the loop counter, tested and updated across the yield in "
         "the loop body, to be live across yield");
}

auto test_no_yields_produces_empty_result() -> void {
  auto fixture =
      check_fixture("module sample\n"
                    "generator def empty() -> some iterator[int32]:\n"
                    "    let a = 1\n");
  const auto fn = lower_generator(fixture, "empty");
  const auto live = hir::live_across_yield(*fn);
  expect(live.empty(),
         "expected a generator body with no yields to have no live-across-"
         "yield locals");
}

} // namespace

auto main() -> int {
  try {
    test_local_bound_before_yield_and_used_after_is_live();
    test_local_used_only_before_yield_is_not_live();
    test_local_bound_between_two_yields_and_used_after_later_yield_is_live();
    test_loop_condition_variable_spanning_yield_is_live();
    test_no_yields_produces_empty_result();
  } catch (const std::exception &ex) {
    std::cerr << "live_across_yield_test failed: unhandled exception: "
              << ex.what() << '\n';
    std::exit(1);
  }
  return 0;
}
