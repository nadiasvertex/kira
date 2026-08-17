#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analysis.h"
#include "borrow_check.h"
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
    std::cerr << "borrow_check_test: missing diagnostic `" << needle << "`\n"
              << "rendered diagnostics were:\n"
              << analyzed.diagnostics << '\n';
    fail(message);
  }
}

/// Locates the real `src/std` package under whichever invocation shape the
/// test binary is running — mirrors `move_check_test.cpp`'s `find_std_dir`.
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

/// The real auto-injected prelude, in the order `inject_stdlib_prelude`
/// (`src/driver/driver.cpp`) lists them — same set `move_check_test.cpp` uses.
auto prelude_fixtures() -> std::vector<source_fixture> {
  const auto std_dir = find_std_dir();
  auto fixtures = std::vector<source_fixture>{};
  for (const auto *filename :
       {"traits.kira", "iter.kira", "prelude.kira", "panic.kira", "option.kira",
        "result.kira", "list.kira", "io.kira", "console.kira", "algo.kira",
        "fmt.kira", "string.kira", "deriving.kira"}) {
    fixtures.push_back(source_fixture{
        .path = std::string("std/") + filename,
        .text = kira::testing::load_test_data_file(std_dir.string(), filename),
    });
  }
  return fixtures;
}

auto analyze_sources(const std::vector<source_fixture> &extra_fixtures)
    -> analyzed_session {
  // The user's own fixtures must come *first* in file-id order: the checks
  // (mirroring the driver's `stdlib_start`/`skip_from_fileid` pairing) only
  // run over files whose id is below the boundary the driver builds from the
  // user sources before appending the injected prelude.
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
    fail("expected borrow check test fixtures to parse");
  }

  const auto checked =
      kira::semantic::validate_semantics(parsed_modules, diag, file_has_errors);

  // Mirror the driver's own pairing: the move checker runs first, then the
  // borrow checker, both over the same checked result and only over the
  // user's own files (`stdlib_boundary` skips the injected prelude).
  kira::semantic::check_moves(parsed_modules, checked, diag, file_has_errors,
                              static_cast<unsigned>(stdlib_boundary));
  kira::semantic::check_borrows(parsed_modules, checked, diag, file_has_errors,
                                static_cast<unsigned>(stdlib_boundary));

  return analyzed_session{
      .diagnostics = kira::diagnostic_renderer(sources, false).render_all(diag),
      .error_count = diag.error_count(),
  };
}

auto analyze_test_data_file(std::string_view filename) -> analyzed_session {
  const auto test_data_dir =
      kira::testing::find_test_data_dir("semantic_borrow_check_test");
  const auto text =
      kira::testing::load_test_data_file(test_data_dir.string(), filename);
  return analyze_sources({{.path = std::string(filename), .text = text}});
}

// ==========================================================================
//  Escape rule: a plain-reference borrow is only legal in a passing position
//  (a call argument, the callee, or a projection base). Stored anywhere else,
//  it cannot escape the call it was made for.
// ==========================================================================

auto test_storing_a_borrow_in_a_let_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_store_borrow_in_let.kira");
  expect(analyzed.error_count > 0,
         "expected storing a borrow in a `let` to be rejected");
  expect_diagnostic(analyzed, "cannot escape the call it was made for",
                    "expected an escaping-borrow diagnostic");
}

auto test_returning_a_borrow_is_rejected() -> void {
  const auto analyzed = analyze_test_data_file("reject_return_borrow.kira");
  expect(analyzed.error_count > 0,
         "expected returning a freshly made borrow to be rejected");
  expect_diagnostic(analyzed, "cannot escape the call it was made for",
                    "expected an escaping-borrow diagnostic");
}

auto test_borrow_in_an_aggregate_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_borrow_in_aggregate.kira");
  expect(analyzed.error_count > 0,
         "expected placing a borrow into a tuple to be rejected");
  expect_diagnostic(analyzed, "cannot escape the call it was made for",
                    "expected an escaping-borrow diagnostic");
}

// ==========================================================================
//  Exclusivity within a single call: at most one `&mut`, and a `&mut` cannot
//  coexist with any `&` of the same value.
// ==========================================================================

auto test_two_mutable_borrows_in_one_call_are_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_two_mut_borrows_one_call.kira");
  expect(analyzed.error_count > 0,
         "expected two `&mut` borrows of the same value in one call to be "
         "rejected");
  expect_diagnostic(analyzed,
                    "cannot borrow `x` as mutable more than once in the same "
                    "call",
                    "expected a two-mutable-borrows diagnostic naming `x`");
}

auto test_mutable_and_shared_borrow_in_one_call_are_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_mut_and_shared_one_call.kira");
  expect(analyzed.error_count > 0,
         "expected a `&mut` and a `&` borrow of the same value in one call to "
         "be rejected");
  expect_diagnostic(
      analyzed, "cannot borrow `x` as mutable and immutable at the same time",
      "expected a mutable-and-immutable-borrow diagnostic naming `x`");
}

// ==========================================================================
//  Acceptance: borrows used as intended must check cleanly.
// ==========================================================================

auto test_borrowing_as_a_call_argument_is_accepted() -> void {
  const auto analyzed = analyze_test_data_file("accept_borrow_as_arg.kira");
  expect(analyzed.error_count == 0,
         "expected lending values to calls with `&`/`&mut` to check cleanly");
}

auto test_many_shared_borrows_in_one_call_are_accepted() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_many_shared_borrows.kira");
  expect(analyzed.error_count == 0,
         "expected any number of `&` borrows in one call to check cleanly");
}

auto test_sequential_borrows_are_accepted() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_sequential_borrows.kira");
  expect(analyzed.error_count == 0,
         "expected a `&mut` and a later `&` borrow in separate statements to "
         "check cleanly");
}

auto test_borrows_in_different_nested_calls_are_accepted() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_nested_call_borrows.kira");
  expect(analyzed.error_count == 0,
         "expected borrows in two different nested calls, not simultaneously "
         "live, to check cleanly");
}

auto test_mutable_slice_view_is_not_an_escape() -> void {
  const auto analyzed = analyze_test_data_file("accept_mut_slice_view.kira");
  expect(analyzed.error_count == 0,
         "expected storing a `&mut xs[a..b]` view in a binding to check "
         "cleanly — a view is not a plain borrow");
}

// ==========================================================================
//  Exclusivity across nesting: a borrow made as a direct argument of a call
//  stays live while that call's *other* (nested) arguments are evaluated, so
//  a conflicting borrow made inside one of them must be caught. A per-call
//  local check that never looks past one argument list would miss these.
// ==========================================================================

auto test_mut_borrow_across_nested_call_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_mut_borrow_across_nested_call.kira");
  expect(analyzed.error_count > 0,
         "expected a `&mut` direct argument aliasing a nested `&` argument to "
         "be rejected");
  expect_diagnostic(
      analyzed, "cannot borrow `n` as mutable and immutable at the same time",
      "expected a mutable-and-immutable diagnostic across the nested call");
}

auto test_two_mut_borrows_across_nested_call_are_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_two_mut_across_nested_call.kira");
  expect(analyzed.error_count > 0,
         "expected two `&mut` borrows live across a nested call to be "
         "rejected");
  expect_diagnostic(
      analyzed, "cannot borrow `n` as mutable more than once in the same call",
      "expected a two-mutable-borrows diagnostic across the nested call");
}

auto test_shared_borrows_across_nested_call_are_accepted() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_shared_across_nested_call.kira");
  expect(analyzed.error_count == 0,
         "expected a direct `&` and a nested `&` of the same value to check "
         "cleanly — two shared borrows never conflict");
}

// ==========================================================================
//  Method-receiver borrows: an autoref receiver participates in exclusivity.
//  A `mut self` receiver conflicts with a same-value `&` argument (it is
//  active at the call), but is only *reserved* while its own arguments are
//  evaluated, so a nested shared borrow of the same value is fine (two-phase).
// ==========================================================================

auto test_receiver_mut_with_shared_arg_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_receiver_mut_with_shared_arg.kira");
  expect(analyzed.error_count > 0,
         "expected a `mut self` receiver aliasing an explicit `&` argument of "
         "the same call to be rejected");
  expect_diagnostic(
      analyzed, "cannot borrow `b` as mutable and immutable at the same time",
      "expected a mutable-and-immutable diagnostic for the receiver");
}

auto test_two_phase_receiver_is_accepted() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_two_phase_receiver.kira");
  expect(analyzed.error_count == 0,
         "expected `b.scale(b.val())` to check cleanly — the `mut self` "
         "reservation is compatible with a nested shared borrow");
}

auto test_two_phase_receiver_with_nested_mut_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_two_phase_receiver_nested_mut.kira");
  expect(analyzed.error_count > 0,
         "expected a nested `&mut` of the receiver to conflict with the "
         "`mut self` reservation");
  expect_diagnostic(
      analyzed, "cannot borrow `b` as mutable more than once in the same call",
      "expected a two-mutable-borrows diagnostic for the reserved receiver");
}

// ==========================================================================
//  View-borrow exclusivity: a view (`slice`/`mut slice`) is the one borrowing
//  value allowed to outlive a call, so it keeps its source collection borrowed
//  across statements — from its binding through its last use. A conflicting
//  borrow of that collection while the view is live must be rejected, whether
//  the view came from a direct slice, a slice-returning call, or a struct that
//  stores one.
// ==========================================================================

auto test_mut_view_across_mutation_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_mut_view_across_mutation.kira");
  expect(analyzed.error_count > 0,
         "expected mutating `xs` while a `mut slice` view of it is live to be "
         "rejected");
  expect_diagnostic(analyzed,
                    "cannot borrow `xs` while the view `s` of `xs` is still in "
                    "use",
                    "expected a live-view conflict diagnostic naming `xs`/`s`");
}

auto test_two_mut_views_are_rejected() -> void {
  const auto analyzed = analyze_test_data_file("reject_two_mut_views.kira");
  expect(analyzed.error_count > 0,
         "expected two simultaneously live `mut slice` views of `xs` to be "
         "rejected");
  expect_diagnostic(analyzed, "cannot borrow `xs` while the view `a` of `xs`",
                    "expected a conflict between the two mutable views");
}

auto test_mut_view_with_shared_borrow_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_mut_view_with_shared_borrow.kira");
  expect(analyzed.error_count > 0,
         "expected a shared `&xs` borrow taken while a `mut slice` view of "
         "`xs` is live to be rejected");
  expect_diagnostic(analyzed, "cannot borrow `xs` while the view `s` of `xs`",
                    "expected a mutable-view-vs-shared-borrow conflict");
}

auto test_returned_view_is_tracked() -> void {
  const auto analyzed = analyze_test_data_file("reject_returned_view.kira");
  expect(analyzed.error_count > 0,
         "expected a view returned from a call to keep its source collection "
         "borrowed, so a later mutation of that collection is rejected");
  expect_diagnostic(analyzed, "cannot borrow `xs` while the view `s` of `xs`",
                    "expected the returned view to conflict with `&mut xs`");
}

auto test_view_stored_in_struct_is_tracked() -> void {
  const auto analyzed = analyze_test_data_file("reject_view_in_struct.kira");
  expect(analyzed.error_count > 0,
         "expected a struct that stores a view to keep the sliced collection "
         "borrowed, so a later mutation of it is rejected");
  expect_diagnostic(
      analyzed, "cannot borrow `xs` while the view `w` of `xs`",
      "expected the struct-stored view to conflict with `&mut xs`");
}

auto test_two_shared_views_are_accepted() -> void {
  const auto analyzed = analyze_test_data_file("accept_two_shared_views.kira");
  expect(analyzed.error_count == 0,
         "expected any number of shared `slice` views of `xs` to check "
         "cleanly — two shared borrows never conflict");
}

auto test_view_dead_at_last_use_is_accepted() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_view_last_use_ends.kira");
  expect(analyzed.error_count == 0,
         "expected re-borrowing `xs` after a view's last use to check cleanly "
         "— liveness ends at the last use, not the end of scope");
}

auto test_view_of_other_variable_is_accepted() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_view_of_other_variable.kira");
  expect(analyzed.error_count == 0,
         "expected a live view of `xs` to place no constraint on a borrow of a "
         "different collection `ys`");
}

auto test_two_mut_capture_closures_are_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_two_mut_capture_closures.kira");
  expect(analyzed.error_count != 0,
         "expected two closures holding `&mut total` at once to be rejected");
  expect_diagnostic(analyzed,
                    "cannot borrow `total` while the closure `a` is still in "
                    "use",
                    "expected the closure to be named as the live borrower");
  expect_diagnostic(analyzed, "entry in a capture list borrows that variable",
                    "expected the capture-list rule to be explained, not the "
                    "`slice`/`cell` view rule");
}

auto test_by_value_capture_is_not_a_borrow() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_by_value_capture_is_not_a_borrow.kira");
  expect(analyzed.error_count == 0,
         std::string("expected bare by-value captures to borrow nothing:\n") +
             analyzed.diagnostics);
}

auto test_shared_capture_closures_are_accepted() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_shared_capture_closures.kira");
  expect(analyzed.error_count == 0,
         std::string("expected any number of `&` captures to coexist:\n") +
             analyzed.diagnostics);
}

auto test_owned_return_keeps_args_free_is_accepted() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_owned_return_keeps_args_free.kira");
  expect(analyzed.error_count == 0,
         "expected a call returning an owned (non-view) value to keep none of "
         "its reference arguments borrowed past the call");
}

} // namespace

auto main() -> int {
  try {
    test_storing_a_borrow_in_a_let_is_rejected();
    test_returning_a_borrow_is_rejected();
    test_borrow_in_an_aggregate_is_rejected();
    test_two_mutable_borrows_in_one_call_are_rejected();
    test_mutable_and_shared_borrow_in_one_call_are_rejected();
    test_borrowing_as_a_call_argument_is_accepted();
    test_many_shared_borrows_in_one_call_are_accepted();
    test_sequential_borrows_are_accepted();
    test_borrows_in_different_nested_calls_are_accepted();
    test_mutable_slice_view_is_not_an_escape();
    test_mut_borrow_across_nested_call_is_rejected();
    test_two_mut_borrows_across_nested_call_are_rejected();
    test_shared_borrows_across_nested_call_are_accepted();
    test_receiver_mut_with_shared_arg_is_rejected();
    test_two_phase_receiver_is_accepted();
    test_two_phase_receiver_with_nested_mut_is_rejected();
    test_mut_view_across_mutation_is_rejected();
    test_two_mut_views_are_rejected();
    test_mut_view_with_shared_borrow_is_rejected();
    test_returned_view_is_tracked();
    test_view_stored_in_struct_is_tracked();
    test_two_shared_views_are_accepted();
    test_view_dead_at_last_use_is_accepted();
    test_view_of_other_variable_is_accepted();
    test_two_mut_capture_closures_are_rejected();
    test_by_value_capture_is_not_a_borrow();
    test_shared_capture_closures_are_accepted();
    test_owned_return_keeps_args_free_is_accepted();
  } catch (const std::exception &ex) {
    std::cerr << "borrow_check_test failed: unhandled exception: " << ex.what()
              << '\n';
    std::exit(1);
  }
  return 0;
}
