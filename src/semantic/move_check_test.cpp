#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analysis.h"
#include "move_check.h"
#include "src/parser/parser.h"
#include "src/testing/test_assert.h"
#include "src/testing/test_data.h"

namespace {

using kira::testing::expect;
using kira::testing::fail;

struct source_fixture {
  std::string path;
  std::string text;
};

struct analyzed_session {
  std::string diagnostics;
  uint32_t error_count = 0;
};

auto expect_diagnostic(
    const analyzed_session &analyzed,
    std::string_view needle, // NOLINT(bugprone-easily-swappable-parameters)
    std::string_view message) -> void {
  if (analyzed.diagnostics.find(needle) == std::string::npos) {
    std::cerr << "move_check_test: missing diagnostic `" << needle << "`\n"
              << "rendered diagnostics were:\n"
              << analyzed.diagnostics << '\n';
    fail(message);
  }
}

/// Locates the real `src/std` package under whichever invocation shape the
/// test binary is running — mirrors `find_test_data_dir`
/// (`src/testing/test_data.h`), pointed at `src/std` instead of a
/// `src/testdata/<test_name>` subdirectory.
auto find_std_dir() -> kira::testing::fs::path {
  namespace fs = kira::testing::fs;
  auto candidates = std::vector<fs::path>{};
  if (const auto *srcdir = std::getenv("TEST_SRCDIR"); srcdir != nullptr) {
    if (const auto *workspace = std::getenv("TEST_WORKSPACE");
        workspace != nullptr && *workspace != '\0') {
      candidates.emplace_back(fs::path(srcdir) / workspace / "src/std");
    }
    candidates.emplace_back(fs::path(srcdir) / "_main" / "src/std");
  }
  candidates.emplace_back("src/std");

  for (const auto &candidate : candidates) {
    auto ec = std::error_code{};
    if (fs::is_directory(candidate, ec)) {
      return candidate;
    }
  }

  fail("could not locate src/std directory");
  std::abort();
}

/// Fixtures for the real auto-injected prelude, in the same order
/// `inject_stdlib_prelude` (`src/driver/driver.cpp`) lists them. Unlike
/// `check_test.cpp`'s trimmed-down prelude, this pulls in `std.algo` too:
/// the bug this test file guards against (`nv.for_each(...)` silently
/// moving `nv`, then a reuse going undetected) only reproduces against the
/// real `std.algo`/`std.iter` UFCS surface, not a hand-rolled stand-in.
auto prelude_fixtures() -> std::vector<source_fixture> {
  const auto std_dir = find_std_dir();
  auto fixtures = std::vector<source_fixture>{};
  for (const auto *filename :
       {"traits.kira", "iter.kira", "prelude.kira", "panic.kira",
        "option.kira", "result.kira", "list.kira", "io.kira", "console.kira",
        "algo.kira", "fmt.kira", "string.kira", "deriving.kira"}) {
    fixtures.push_back(source_fixture{
        .path = std::string("std/") + filename,
        .text = kira::testing::load_test_data_file(std_dir.string(),
                                                    filename),
    });
  }
  return fixtures;
}

auto analyze_sources(const std::vector<source_fixture> &extra_fixtures)
    -> analyzed_session {
  // The user's own fixtures must come *first* in file-id order: `check_moves`
  // (mirroring the driver's own `stdlib_start`/`skip_from_fileid` pairing)
  // only checks files whose id is below the boundary, and the driver builds
  // that boundary from the user-supplied sources before appending the
  // injected prelude after them.
  const auto stdlib_boundary = extra_fixtures.size();
  auto fixtures = extra_fixtures;
  const auto prelude = prelude_fixtures();
  fixtures.insert(fixtures.end(), prelude.begin(), prelude.end());

  auto sources = kira::source_manager{};
  auto diag = kira::diagnostic_bag{};
  auto file_has_errors = std::vector<bool>{};
  auto ast_files = std::vector<kira::ast::ptr<kira::ast::file>>{};
  auto parsed_modules = std::vector<kira::semantic::parsed_module>{};
  ast_files.reserve(fixtures.size());
  parsed_modules.reserve(fixtures.size());

  for (const auto &fixture : fixtures) {
    auto file_id = sources.add_file(fixture.path, fixture.text);
    expect(file_id.has_value(), "expected fixture source to register");

    if (file_has_errors.size() <= static_cast<size_t>(*file_id)) {
      file_has_errors.resize(static_cast<size_t>(*file_id) + 1, false);
    }

    const auto *file = sources.get(*file_id);
    expect(file != nullptr, "expected registered fixture source");

    const auto errors_before = diag.error_count();
    auto lexer = kira::lexer(file->source(), file->id(), diag);
    auto tokens = lexer.tokenize();
    auto parser = kira::parser(std::move(tokens), file->id(), diag);
    auto ast_file = parser.parse_file();

    if (diag.error_count() > errors_before) {
      file_has_errors[*file_id] = true;
    }

    parsed_modules.push_back(kira::semantic::parsed_module{
        .file_id = *file_id,
        .ast_file = ast_file.get(),
    });
    ast_files.push_back(std::move(ast_file));
  }

  if (diag.error_count() != 0) {
    std::cerr << kira::diagnostic_renderer(sources, false).render_all(diag);
    fail("expected move check test fixtures to parse");
  }

  const auto checked =
      kira::semantic::validate_semantics(parsed_modules, diag, file_has_errors);

  // Mirrors `driver.cpp`'s own `validate_semantics` + `check_moves` pairing:
  // move checking is a separate pass over the same checked result, run only
  // over the user's own files (`stdlib_boundary` skips the injected prelude,
  // exactly like the driver's `stdlib_start`).
  kira::semantic::check_moves(parsed_modules, checked, diag, file_has_errors,
                              static_cast<unsigned>(stdlib_boundary));

  return analyzed_session{
      .diagnostics = kira::diagnostic_renderer(sources, false).render_all(diag),
      .error_count = diag.error_count(),
  };
}

auto analyze_test_data_file(std::string_view filename) -> analyzed_session {
  const auto test_data_dir =
      kira::testing::find_test_data_dir("semantic_move_check_test");
  const auto text =
      kira::testing::load_test_data_file(test_data_dir.string(), filename);
  return analyze_sources({{.path = std::string(filename), .text = text}});
}

// ==========================================================================
//  The regression this test file exists for: a UFCS call whose resolved
//  method takes its receiver by value (not `self`, not `&`/`&mut`) must move
//  it, the same as an ordinary by-value call argument does. Before this fix,
//  `move_check.cpp` treated every method-call receiver as a mere field
//  projection, so `nv.for_each(...)` never registered as consuming `nv` and a
//  second use compiled silently into a no-op (the iterator was already
//  drained at runtime) instead of being rejected at compile time.
// ==========================================================================

auto test_reuse_after_by_value_ufcs_call_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_reuse_after_for_each.kira");
  expect(analyzed.error_count > 0,
         "expected reusing an iterator after `for_each` moved it to be "
         "rejected");
  expect_diagnostic(analyzed, "use of moved value `nv`",
                     "expected a use-after-move diagnostic naming `nv`");
}

auto test_reuse_after_by_value_free_function_ufcs_call_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_reuse_after_into_iter.kira");
  expect(analyzed.error_count > 0,
         "expected reusing a list after `into_iter` moved it to be rejected");
  expect_diagnostic(analyzed, "use of moved value `xs`",
                     "expected a use-after-move diagnostic naming `xs`");
}

// ==========================================================================
//  Guardrails against over-correcting: a `self`-receiver method call (which
//  always takes its receiver by reference regardless of `mut`) and a UFCS
//  call whose first parameter is an explicit `&`/`&mut` reference must both
//  still permit reusing the receiver across repeated calls.
// ==========================================================================

auto test_repeated_self_method_calls_on_same_binding_are_accepted() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_repeated_self_method_calls.kira");
  expect(analyzed.error_count == 0,
         "expected repeated `self`-receiver method calls on the same "
         "binding to check cleanly");
}

auto test_repeated_borrowing_ufcs_calls_are_accepted() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_repeated_borrowing_ufcs_calls.kira");
  expect(analyzed.error_count == 0,
         "expected repeated UFCS calls through a `&`-typed first parameter "
         "to check cleanly");
}

auto test_repeated_iter_values_chains_are_accepted() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_repeated_iter_values_chains.kira");
  expect(analyzed.error_count == 0,
         "expected rebuilding a fresh `iter().values()` chain per use to "
         "check cleanly, the fix for the original `algorithm-for_each.kira` "
         "demo bug");
}

} // namespace

auto main() -> int {
  try {
    test_reuse_after_by_value_ufcs_call_is_rejected();
    test_reuse_after_by_value_free_function_ufcs_call_is_rejected();
    test_repeated_self_method_calls_on_same_binding_are_accepted();
    test_repeated_borrowing_ufcs_calls_are_accepted();
    test_repeated_iter_values_chains_are_accepted();
  } catch (const std::exception &ex) {
    std::cerr << "move_check_test failed: unhandled exception: " << ex.what()
              << '\n';
    std::exit(1);
  }
  return 0;
}
