#include <cstdlib>
#include <exception>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analysis.h"
#include "ownership_check.h"
#include "src/parser/parser.h"
#include "src/testing/test_assert.h"
#include "src/testing/test_data.h"

namespace {

using cinder::testing::expect;
using cinder::testing::fail;

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
    std::cerr << "ownership_check_test: missing diagnostic `" << needle << "`\n"
              << "rendered diagnostics were:\n"
              << analyzed.diagnostics << '\n';
    fail(message);
  }
}

/// Locates the real `src/std` package under whichever invocation shape the
/// test binary is running — mirrors `find_test_data_dir`.
auto find_std_dir() -> cinder::testing::fs::path {
  namespace fs = cinder::testing::fs;
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
/// (`src/driver/driver.cpp`) lists them — the set the driver injects.
auto prelude_fixtures() -> std::vector<source_fixture> {
  const auto std_dir = find_std_dir();
  auto fixtures = std::vector<source_fixture>{};
  for (const auto *filename : {"intrinsics.cn",
                               "traits.cn",
                               "traits.ord.cn",
                               "traits.show.cn",
                               "traits.numeric.cn",
                               "traits.conversion.cn",
                               "traits.category.cn",
                               "traits.index.cn",
                               "traits.hash.cn",
                               "traits.scalar.cn",
                               "limits.cn",
                               "iter.cn",
                               "prelude.cn",
                               "panic.cn",
                               "option.cn",
                               "result.cn",
                               "mem.cn",
                               "list.cn",
                               "io.cn",
                               "console.cn",
                               "algo.cn",
                               "fmt.cn",
                               "unicode_tables.cn",
                               "unicode.cn",
                               "string.cn",
                               "deriving.cn"}) {
    fixtures.push_back(source_fixture{
        .path = std::string("std/") + filename,
        .text =
            cinder::testing::load_test_data_file(std_dir.string(), filename),
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

  auto sources = cinder::source_manager{};
  auto diag = cinder::diagnostic_bag{};
  auto file_has_errors = std::vector<bool>{};
  auto ast_files = std::vector<cinder::ast::ptr<cinder::ast::file>>{};
  auto parsed_modules = std::vector<cinder::semantic::parsed_module>{};
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
    auto lexer = cinder::lexer(file->source(), file->id(), diag);
    auto tokens = lexer.tokenize();
    auto parser = cinder::parser(std::move(tokens), file->id(), diag);
    auto ast_file = parser.parse_file();

    if (diag.error_count() > errors_before) {
      file_has_errors[*file_id] = true;
    }

    parsed_modules.push_back(cinder::semantic::parsed_module{
        .file_id = *file_id,
        .ast_file = ast_file.get(),
    });
    ast_files.push_back(std::move(ast_file));
  }

  if (diag.error_count() != 0) {
    std::cerr << cinder::diagnostic_renderer(sources, false).render_all(diag);
    fail("expected ownership check test fixtures to parse");
  }

  const auto checked = cinder::semantic::validate_semantics(
      parsed_modules, diag, file_has_errors);

  // Mirror the driver: one ownership pass over the checked result, run only
  // over the user's own files (`stdlib_boundary` skips the injected prelude).
  cinder::semantic::check_ownership(parsed_modules, checked, diag,
                                    file_has_errors,
                                    static_cast<unsigned>(stdlib_boundary));

  return analyzed_session{
      .diagnostics =
          cinder::diagnostic_renderer(sources, false).render_all(diag),
      .error_count = diag.error_count(),
  };
}

auto analyze_test_data_file(std::string_view filename) -> analyzed_session {
  const auto test_data_dir =
      cinder::testing::find_test_data_dir("semantic_ownership_check_test");
  const auto text =
      cinder::testing::load_test_data_file(test_data_dir.string(), filename);
  return analyze_sources({{.path = std::string(filename), .text = text}});
}

// ==========================================================================
//  Stored borrows: a `&`/`&mut` may be bound, stored, and returned like a
//  view — tracked for as long as the value holding it lives, and never
//  allowed to outlive what it borrows.
// ==========================================================================

auto test_stored_borrow_is_accepted() -> void {
  const auto analyzed = analyze_test_data_file("accept_store_borrow_in_let.cn");
  expect(analyzed.error_count == 0,
         std::string("expected a bound borrow with no conflict to check "
                     "cleanly:\n") +
             analyzed.diagnostics);
}

auto test_assign_while_reference_live_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_assign_while_reference_live.cn");
  expect_diagnostic(analyzed,
                    "cannot assign to `n` while the reference `r` to `n` is "
                    "still in use",
                    "expected a bound `&n` to keep `n` borrowed");
}

auto test_assign_while_aggregate_holds_reference_is_rejected() -> void {
  const auto analyzed = analyze_test_data_file(
      "reject_assign_while_aggregate_holds_reference.cn");
  expect_diagnostic(analyzed,
                    "cannot assign to `n` while `t`, which borrows `n`, is "
                    "still in use",
                    "expected a tuple storing `&n` to keep `n` borrowed");
}

auto test_returning_a_borrow_of_a_local_is_rejected() -> void {
  const auto analyzed = analyze_test_data_file("reject_return_borrow.cn");
  expect_diagnostic(analyzed, "cannot return a borrow of the local `n`",
                    "expected `return &n` of a local to be rejected");
}

auto test_returning_a_laundered_borrow_of_a_local_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_return_reference_to_local_via_call.cn");
  expect_diagnostic(analyzed, "cannot return a borrow of the local `n`",
                    "expected `return pass(&n)` of a local to be rejected");
}

auto test_generic_method_result_not_tied_to_receiver_is_accepted() -> void {
  const auto analyzed = analyze_test_data_file(
      "accept_generic_method_result_not_tied_to_receiver.cn");
  expect(analyzed.error_count == 0,
         std::string("expected a `U`-typed method result not to borrow its "
                     "`point` receiver:\n") +
             analyzed.diagnostics);
}

auto test_returning_a_borrow_through_an_implicit_generic_is_rejected()
    -> void {
  const auto analyzed = analyze_test_data_file(
      "reject_return_borrow_through_implicit_generic.cn");
  expect_diagnostic(analyzed, "cannot return a borrow of the local `n`",
                    "expected `first(&n, r)` to carry the borrow of `n`");
}

auto test_returning_a_receiver_borrow_through_a_type_param_is_rejected()
    -> void {
  const auto analyzed = analyze_test_data_file(
      "reject_return_borrow_of_receiver_through_type_param.cn");
  expect_diagnostic(analyzed, "cannot return a borrow of the local `h`",
                    "expected a `T` result to borrow a `holder[T]` receiver");
}

auto test_returning_a_reference_to_a_member_is_accepted() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_return_reference_to_member.cn");
  expect(analyzed.error_count == 0,
         std::string("expected `&q.b` of a borrowed parameter to be "
                     "returnable:\n") +
             analyzed.diagnostics);
}

auto test_mutating_while_member_reference_live_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_mutate_while_member_reference_live.cn");
  expect_diagnostic(analyzed,
                    "cannot assign to `p` while the reference `s` to `p` is "
                    "still in use",
                    "expected a returned member reference to keep `p` "
                    "borrowed");
}

auto test_copying_a_mut_reference_moves_it() -> void {
  const auto analyzed = analyze_test_data_file("reject_copy_mut_reference.cn");
  expect_diagnostic(analyzed, "use of moved value `p`",
                    "expected binding a `&mut` to a second name to move it");
}

// ==========================================================================
//  Exclusivity within a single call: at most one `&mut`, and a `&mut` cannot
//  coexist with any `&` of the same value.
// ==========================================================================

auto test_two_mutable_borrows_in_one_call_are_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_two_mut_borrows_one_call.cn");
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
      analyze_test_data_file("reject_mut_and_shared_one_call.cn");
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
  const auto analyzed = analyze_test_data_file("accept_borrow_as_arg.cn");
  expect(analyzed.error_count == 0,
         "expected lending values to calls with `&`/`&mut` to check cleanly");
}

auto test_many_shared_borrows_in_one_call_are_accepted() -> void {
  const auto analyzed = analyze_test_data_file("accept_many_shared_borrows.cn");
  expect(analyzed.error_count == 0,
         "expected any number of `&` borrows in one call to check cleanly");
}

auto test_sequential_borrows_are_accepted() -> void {
  const auto analyzed = analyze_test_data_file("accept_sequential_borrows.cn");
  expect(analyzed.error_count == 0,
         "expected a `&mut` and a later `&` borrow in separate statements to "
         "check cleanly");
}

auto test_borrows_in_different_nested_calls_are_accepted() -> void {
  const auto analyzed = analyze_test_data_file("accept_nested_call_borrows.cn");
  expect(analyzed.error_count == 0,
         "expected borrows in two different nested calls, not simultaneously "
         "live, to check cleanly");
}

auto test_mutable_slice_view_is_accepted() -> void {
  const auto analyzed = analyze_test_data_file("accept_mut_slice_view.cn");
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
      analyze_test_data_file("reject_mut_borrow_across_nested_call.cn");
  expect(analyzed.error_count > 0,
         "expected a `&mut` direct argument aliasing a nested `&` argument to "
         "be rejected");
  expect_diagnostic(
      analyzed, "cannot borrow `n` as mutable and immutable at the same time",
      "expected a mutable-and-immutable diagnostic across the nested call");
}

auto test_two_mut_borrows_across_nested_call_are_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_two_mut_across_nested_call.cn");
  expect(analyzed.error_count > 0,
         "expected two `&mut` borrows live across a nested call to be "
         "rejected");
  expect_diagnostic(
      analyzed, "cannot borrow `n` as mutable more than once in the same call",
      "expected a two-mutable-borrows diagnostic across the nested call");
}

auto test_shared_borrows_across_nested_call_are_accepted() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_shared_across_nested_call.cn");
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
      analyze_test_data_file("reject_receiver_mut_with_shared_arg.cn");
  expect(analyzed.error_count > 0,
         "expected a `mut self` receiver aliasing an explicit `&` argument of "
         "the same call to be rejected");
  expect_diagnostic(
      analyzed, "cannot borrow `b` as mutable and immutable at the same time",
      "expected a mutable-and-immutable diagnostic for the receiver");
}

auto test_two_phase_receiver_is_accepted() -> void {
  const auto analyzed = analyze_test_data_file("accept_two_phase_receiver.cn");
  expect(analyzed.error_count == 0,
         "expected `b.scale(b.val())` to check cleanly — the `mut self` "
         "reservation is compatible with a nested shared borrow");
}

auto test_two_phase_receiver_with_nested_mut_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_two_phase_receiver_nested_mut.cn");
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
      analyze_test_data_file("reject_mut_view_across_mutation.cn");
  expect(analyzed.error_count > 0,
         "expected mutating `xs` while a `mut slice` view of it is live to be "
         "rejected");
  expect_diagnostic(analyzed,
                    "cannot borrow `xs` while the view `s` of `xs` is still in "
                    "use",
                    "expected a live-view conflict diagnostic naming `xs`/`s`");
}

auto test_two_mut_views_are_rejected() -> void {
  const auto analyzed = analyze_test_data_file("reject_two_mut_views.cn");
  expect(analyzed.error_count > 0,
         "expected two simultaneously live `mut slice` views of `xs` to be "
         "rejected");
  expect_diagnostic(analyzed, "cannot borrow `xs` while the view `a` of `xs`",
                    "expected a conflict between the two mutable views");
}

auto test_mut_view_with_shared_borrow_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_mut_view_with_shared_borrow.cn");
  expect(analyzed.error_count > 0,
         "expected a shared `&xs` borrow taken while a `mut slice` view of "
         "`xs` is live to be rejected");
  expect_diagnostic(analyzed, "cannot borrow `xs` while the view `s` of `xs`",
                    "expected a mutable-view-vs-shared-borrow conflict");
}

auto test_returned_view_is_tracked() -> void {
  const auto analyzed = analyze_test_data_file("reject_returned_view.cn");
  expect(analyzed.error_count > 0,
         "expected a view returned from a call to keep its source collection "
         "borrowed, so a later mutation of that collection is rejected");
  expect_diagnostic(analyzed, "cannot borrow `xs` while the view `s` of `xs`",
                    "expected the returned view to conflict with `&mut xs`");
}

auto test_view_stored_in_struct_is_tracked() -> void {
  const auto analyzed = analyze_test_data_file("reject_view_in_struct.cn");
  expect(analyzed.error_count > 0,
         "expected a struct that stores a view to keep the sliced collection "
         "borrowed, so a later mutation of it is rejected");
  expect_diagnostic(
      analyzed, "cannot borrow `xs` while `w`, which borrows `xs`,",
      "expected the struct-stored view to conflict with `&mut xs`");
}

auto test_two_shared_views_are_accepted() -> void {
  const auto analyzed = analyze_test_data_file("accept_two_shared_views.cn");
  expect(analyzed.error_count == 0,
         "expected any number of shared `slice` views of `xs` to check "
         "cleanly — two shared borrows never conflict");
}

auto test_view_dead_at_last_use_is_accepted() -> void {
  const auto analyzed = analyze_test_data_file("accept_view_last_use_ends.cn");
  expect(analyzed.error_count == 0,
         "expected re-borrowing `xs` after a view's last use to check cleanly "
         "— liveness ends at the last use, not the end of scope");
}

auto test_slice_arg_with_live_view_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_slice_arg_with_live_view.cn");
  expect(analyzed.error_count > 0,
         "expected `poke(&mut xs[0..xs.len()])` to be rejected while the view "
         "`middle` of `xs` is still live");
  expect_diagnostic(analyzed,
                    "cannot borrow `xs` while the view `middle` of `xs`",
                    "expected the slice argument to conflict with `middle`");
}

auto test_slice_args_in_interpolation_are_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_slice_args_in_interpolation.cn");
  expect(analyzed.error_count > 0,
         "expected conflicting slice arguments inside an interpolation to be "
         "rejected");
  expect_diagnostic(analyzed,
                    "cannot borrow `xs` as mutable and immutable at the same "
                    "time",
                    "expected a mut/shared conflict between the two slices");
}

auto test_slice_arg_after_view_use_is_accepted() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_slice_arg_after_view_use.cn");
  expect(analyzed.error_count == 0,
         "expected slice arguments to check cleanly once earlier views are "
         "dead, with bounds reading the source, and with shared siblings");
}

auto test_view_of_other_variable_is_accepted() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_view_of_other_variable.cn");
  expect(analyzed.error_count == 0,
         "expected a live view of `xs` to place no constraint on a borrow of a "
         "different collection `ys`");
}

auto test_two_mut_capture_closures_are_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_two_mut_capture_closures.cn");
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
      analyze_test_data_file("accept_by_value_capture_is_not_a_borrow.cn");
  expect(analyzed.error_count == 0,
         std::string("expected bare by-value captures to borrow nothing:\n") +
             analyzed.diagnostics);
}

auto test_shared_capture_closures_are_accepted() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_shared_capture_closures.cn");
  expect(analyzed.error_count == 0,
         std::string("expected any number of `&` captures to coexist:\n") +
             analyzed.diagnostics);
}

auto test_owned_return_keeps_args_free_is_accepted() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_owned_return_keeps_args_free.cn");
  expect(analyzed.error_count == 0,
         "expected a call returning an owned (non-view) value to keep none of "
         "its reference arguments borrowed past the call");
}

// ==========================================================================
//  `for x in &v` (spec/todo.md #20): the loop holds its iterable's borrow for
//  the whole body.
// ==========================================================================

auto test_for_over_ref_borrow_is_accepted() -> void {
  const auto analyzed = analyze_test_data_file("accept_for_over_ref_borrow.cn");
  expect(analyzed.error_count == 0,
         std::string("expected `for x in &xs` to check cleanly:\n") +
             analyzed.diagnostics);
}

auto test_mutation_during_ref_for_loop_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_mutation_during_ref_for_loop.cn");
  expect(analyzed.error_count > 0,
         "expected freeing `xs` while `for x in &xs` is still iterating it "
         "to be rejected");
  expect_diagnostic(analyzed,
                    "cannot borrow `xs` while the `for` loop over `xs` is "
                    "still running",
                    "expected the loop's borrow of `xs` to be live for the "
                    "whole body");
}

// ==========================================================================
//  Moves: a by-value use transfers ownership; any later use on some path is
//  rejected. Receivers, captures, and loops follow the same rule.
// ==========================================================================

auto test_reuse_after_by_value_ufcs_call_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_reuse_after_for_each.cn");
  expect(analyzed.error_count > 0,
         "expected reusing an iterator after `for_each` moved it to be "
         "rejected");
  expect_diagnostic(analyzed, "use of moved value `nv`",
                    "expected a use-after-move diagnostic naming `nv`");
}

auto test_reuse_after_into_iter_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_reuse_after_into_iter.cn");
  expect(analyzed.error_count > 0,
         "expected reusing a list after `into_iter` moved it to be rejected");
  expect_diagnostic(analyzed, "use of moved value `xs`",
                    "expected a use-after-move diagnostic naming `xs`");
}

auto test_repeated_self_method_calls_are_accepted() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_repeated_self_method_calls.cn");
  expect(analyzed.error_count == 0,
         std::string("expected repeated `self`-receiver method calls on the "
                     "same binding to check cleanly:\n") +
             analyzed.diagnostics);
}

auto test_repeated_borrowing_ufcs_calls_are_accepted() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_repeated_borrowing_ufcs_calls.cn");
  expect(analyzed.error_count == 0,
         std::string("expected repeated UFCS calls through a `&`-typed first "
                     "parameter to check cleanly:\n") +
             analyzed.diagnostics);
}

auto test_repeated_iter_values_chains_are_accepted() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_repeated_iter_values_chains.cn");
  expect(analyzed.error_count == 0,
         std::string("expected rebuilding a fresh `iter().values()` chain per "
                     "use to check cleanly:\n") +
             analyzed.diagnostics);
}

auto test_copy_values_reused_are_accepted() -> void {
  const auto analyzed = analyze_test_data_file("accept_copy_values_reused.cn");
  expect(analyzed.error_count == 0,
         std::string("expected `str`, `char`, `&T` and `T: copy` values to "
                     "stay usable after a by-value use:\n") +
             analyzed.diagnostics);
}

auto test_type_param_reused_after_move_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_type_param_reused_after_move.cn");
  expect(analyzed.error_count > 0,
         "expected a `T` without a `copy` bound to move, so reusing it after "
         "passing it by value is rejected");
  expect(analyzed.diagnostics.find("use of moved value `x`") !=
             std::string::npos,
         std::string("expected a use-after-move of `x`:\n") +
             analyzed.diagnostics);
  expect(analyzed.diagnostics.find("where T: copy") != std::string::npos,
         std::string("expected the help to offer a `copy` bound:\n") +
             analyzed.diagnostics);
}

auto test_partial_moves_are_accepted() -> void {
  const auto analyzed = analyze_test_data_file("accept_partial_moves.cn");
  expect(analyzed.error_count == 0,
         std::string("expected partial moves, field refills, matching a "
                     "field, and raw-pointer reads to check cleanly:\n") +
             analyzed.diagnostics);
}

auto test_moves_out_of_places_are_rejected() -> void {
  const auto analyzed = analyze_test_data_file("reject_moves_out_of_places.cn");
  for (const auto *needle :
       {"cannot move out of `self.items`",
        "cannot move out of an element of `xs`", "cannot move out of `*r`",
        "cannot move out of `g.inner`", "use of partly moved value `j`",
        "`[v; n]` needs a `copy` value"}) {
    expect(analyzed.diagnostics.find(needle) != std::string::npos,
           std::string("expected `") + needle + "`:\n" +
               analyzed.diagnostics);
  }
  expect(analyzed.error_count == 6,
         std::string("expected exactly the six rejected moves:\n") +
             analyzed.diagnostics);
}

auto test_outlived_temporaries_are_rejected() -> void {
  const auto analyzed = analyze_test_data_file("reject_temporary_outlived.cn");
  for (const auto *needle :
       {"`it` still borrows the temporary after the statement ends",
        "`r` still borrows the temporary after the statement ends",
        "cannot move out of `make_guarded().inner`"}) {
    expect(analyzed.diagnostics.find(needle) != std::string::npos,
           std::string("expected `") + needle + "`:\n" +
               analyzed.diagnostics);
  }
  expect(analyzed.error_count == 3,
         std::string("expected exactly the three rejected temporaries:\n") +
             analyzed.diagnostics);
}

auto test_consuming_loop_over_borrow_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_consuming_loop_over_borrow.cn");
  expect_diagnostic(analyzed, "cannot iterate over `xs` by value",
                    "expected a loop over a borrowed list of non-`copy` "
                    "elements to be rejected");
  expect(analyzed.error_count == 1,
         std::string("expected only the non-`copy` loop to be rejected:\n") +
             analyzed.diagnostics);
}

auto test_overlapping_owned_binding_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_overlapping_owned_binding.cn");
  expect_diagnostic(analyzed, "cannot move `r` out of the matched value",
                    "expected a non-`copy` binding under an alias to be "
                    "rejected");
  expect(analyzed.error_count == 1,
         std::string("expected an alias of the whole value to own it, and "
                     "only `r` to be rejected:\n") +
             analyzed.diagnostics);
}

auto test_reuse_after_move_capture_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_reuse_after_move_capture.cn");
  expect(analyzed.error_count > 0,
         "expected reusing a name after `move [...]` captured it to be "
         "rejected");
  expect_diagnostic(analyzed, "use of moved value `name`",
                    "expected a use-after-move diagnostic naming `name`");
}

auto test_reuse_after_plain_value_capture_is_accepted() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_reuse_after_plain_value_capture.cn");
  expect(analyzed.error_count == 0,
         std::string("expected a bare capture-list entry without `move` to "
                     "stay a copy:\n") +
             analyzed.diagnostics);
}

auto test_reuse_after_for_x_in_ref_is_accepted() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_reuse_after_for_x_in_ref.cn");
  expect(analyzed.error_count == 0,
         std::string("expected `for x in &xs` not to move `xs`:\n") +
             analyzed.diagnostics);
}

auto test_generic_lambda_moving_capture_twice_is_explained() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_generic_lambda_moves_capture_twice.cn");
  expect_diagnostic(analyzed,
                    "each use of `f` makes a closure that moves `s`",
                    "expected two copies of a generic lambda that each move "
                    "a capture to be explained in terms of the copies");
  expect(analyzed.diagnostics.find("use of moved value") == std::string::npos,
         std::string("expected no plain use-after-move report as well:\n") +
             analyzed.diagnostics);
}

auto test_move_out_of_capture_is_rejected() -> void {
  const auto analyzed = analyze_test_data_file("reject_move_out_of_capture.cn");
  for (const auto *place : {"bare", "implicit", "moved", "generic", "h.items"}) {
    expect_diagnostic(analyzed,
                      std::format("cannot move `{}` out of the closure", place),
                      std::format("expected moving the capture `{}` out of "
                                  "a closure body to be rejected",
                                  place));
  }
  expect(analyzed.error_count == 5,
         std::string("expected one error per moved capture, and one for both "
                     "copies of the generic lambda:\n") +
             analyzed.diagnostics);
}

auto test_reads_of_captures_are_accepted() -> void {
  const auto analyzed = analyze_test_data_file("accept_reads_of_captures.cn");
  expect(analyzed.error_count == 0,
         std::string("expected reading or borrowing a capture to be "
                     "accepted:\n") +
             analyzed.diagnostics);
}

auto test_move_in_loop_is_rejected() -> void {
  const auto analyzed = analyze_test_data_file("reject_move_in_loop.cn");
  expect_diagnostic(analyzed, "use of moved value `xs`",
                    "expected the second iteration's move of `xs` to be a "
                    "use after move");
}

auto test_move_in_one_branch_is_rejected() -> void {
  const auto analyzed = analyze_test_data_file("reject_move_in_one_branch.cn");
  expect_diagnostic(analyzed, "use of moved value `xs`",
                    "expected a use after a move on one path to be rejected");
}

auto test_repeated_autoref_calls_are_accepted() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_repeated_autoref_calls.cn");
  expect(analyzed.error_count == 0,
         std::string("expected a bare argument to a `&` parameter to be lent, "
                     "not moved:\n") +
             analyzed.diagnostics);
}

auto test_autoref_aliasing_is_rejected() -> void {
  const auto analyzed = analyze_test_data_file("reject_autoref_aliasing.cn");
  expect_diagnostic(
      analyzed, "cannot borrow `xs` as mutable and immutable at the same time",
      "expected two implicit borrows of `xs` in one call to conflict");
}

// ==========================================================================
//  Every access to a borrowed value is checked, not just new borrows: a
//  write, a replacement, a move, or (under a `mut` view) a read.
// ==========================================================================

auto test_assign_while_view_live_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_assign_while_view_live.cn");
  expect_diagnostic(analyzed,
                    "cannot assign to `xs` while the view `s` of `xs` is "
                    "still in use",
                    "expected an element write under a live view to conflict");
}

auto test_replace_while_view_live_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_replace_while_view_live.cn");
  expect_diagnostic(analyzed,
                    "cannot assign to `xs` while the view `s` of `xs` is "
                    "still in use",
                    "expected replacing a viewed collection to conflict");
}

auto test_move_while_view_live_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_move_while_view_live.cn");
  expect_diagnostic(analyzed,
                    "cannot move `xs` while the view `s` of `xs` is still in "
                    "use",
                    "expected moving a viewed collection to conflict");
}

auto test_read_under_mut_view_is_rejected() -> void {
  const auto analyzed = analyze_test_data_file("reject_read_under_mut_view.cn");
  expect_diagnostic(analyzed,
                    "cannot use `xs` while the view `m` of `xs` is still in "
                    "use",
                    "expected a direct read under a live `mut` view to "
                    "conflict");
}

auto test_assign_while_capture_live_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_assign_while_capture_live.cn");
  expect_diagnostic(analyzed,
                    "cannot assign to `n` while the closure `c` is still in "
                    "use",
                    "expected assigning a variable a live closure borrows to "
                    "conflict");
}

// ==========================================================================
//  Loan lifetimes follow control flow: loops, branches, expression-valued
//  `if`/`match`, destructuring, `if let`, and returned references.
// ==========================================================================

auto test_view_assigned_in_branch_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_view_assigned_in_branch.cn");
  expect_diagnostic(analyzed, "cannot borrow `xs` while the view `s` of `xs`",
                    "expected a view re-pointed inside an `if` to stay live "
                    "after it");
}

auto test_loop_carried_view_is_rejected() -> void {
  const auto analyzed = analyze_test_data_file("reject_loop_carried_view.cn");
  expect_diagnostic(analyzed, "cannot borrow `xs` while the view `s` of `xs`",
                    "expected a view read on the next iteration to be live "
                    "across the loop's back edge");
}

auto test_view_dead_before_reassign_in_loop_is_accepted() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_view_dead_before_reassign_in_loop.cn");
  expect(analyzed.error_count == 0,
         std::string("expected a view overwritten before its next read to be "
                     "dead:\n") +
             analyzed.diagnostics);
}

auto test_view_from_if_expr_is_rejected() -> void {
  const auto analyzed = analyze_test_data_file("reject_view_from_if_expr.cn");
  expect_diagnostic(analyzed, "cannot borrow `xs` while the view `s` of `xs`",
                    "expected an `if` expression's view to borrow `xs`");
}

auto test_view_from_match_expr_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_view_from_match_expr.cn");
  expect_diagnostic(analyzed, "cannot borrow `xs` while the view `s` of `xs`",
                    "expected a `match` expression's view to borrow `xs`");
}

auto test_view_in_tuple_pattern_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_view_in_tuple_pattern.cn");
  expect_diagnostic(analyzed, "cannot borrow `xs` while the view `a` of `xs`",
                    "expected a destructured view to borrow `xs`");
}

auto test_view_in_if_let_is_rejected() -> void {
  const auto analyzed = analyze_test_data_file("reject_view_in_if_let.cn");
  expect_diagnostic(analyzed,
                    "cannot borrow `xs` while `o`, which borrows `xs`,",
                    "expected an option holding a view to borrow `xs`");
}

auto test_returned_reference_is_tracked() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_returned_reference_kept_live.cn");
  expect_diagnostic(analyzed,
                    "cannot borrow `x` while the reference `r` to `x` is still "
                    "in use",
                    "expected a returned `&int32` to keep `x` borrowed");
}

// ==========================================================================
//  Dangling borrows: a borrow outliving the local it borrows.
// ==========================================================================

auto test_return_view_of_local_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_return_view_of_local.cn");
  expect_diagnostic(analyzed, "cannot return a borrow of the local `xs`",
                    "expected returning a view of a local to be rejected");
}

auto test_return_borrowing_closure_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_return_borrowing_closure.cn");
  expect_diagnostic(analyzed, "cannot return a closure that borrows `total`",
                    "expected returning a closure over a local to be "
                    "rejected");
}

auto test_view_outliving_source_scope_is_rejected() -> void {
  const auto analyzed =
      analyze_test_data_file("reject_view_outlives_source_scope.cn");
  expect_diagnostic(analyzed, "`xs` does not live long enough",
                    "expected a view outliving its source's block to be "
                    "rejected");
}

auto test_return_view_of_parameter_is_accepted() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_return_view_of_parameter.cn");
  expect(analyzed.error_count == 0,
         std::string("expected views of borrowed parameters and of `self` to "
                     "be returnable:\n") +
             analyzed.diagnostics);
}

} // namespace

auto main() -> int {
  try {
    test_stored_borrow_is_accepted();
    test_assign_while_reference_live_is_rejected();
    test_assign_while_aggregate_holds_reference_is_rejected();
    test_returning_a_borrow_of_a_local_is_rejected();
    test_returning_a_laundered_borrow_of_a_local_is_rejected();
    test_generic_method_result_not_tied_to_receiver_is_accepted();
    test_returning_a_borrow_through_an_implicit_generic_is_rejected();
    test_returning_a_receiver_borrow_through_a_type_param_is_rejected();
    test_returning_a_reference_to_a_member_is_accepted();
    test_mutating_while_member_reference_live_is_rejected();
    test_copying_a_mut_reference_moves_it();
    test_two_mutable_borrows_in_one_call_are_rejected();
    test_mutable_and_shared_borrow_in_one_call_are_rejected();
    test_borrowing_as_a_call_argument_is_accepted();
    test_many_shared_borrows_in_one_call_are_accepted();
    test_sequential_borrows_are_accepted();
    test_borrows_in_different_nested_calls_are_accepted();
    test_mutable_slice_view_is_accepted();
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
    test_slice_arg_with_live_view_is_rejected();
    test_slice_args_in_interpolation_are_rejected();
    test_slice_arg_after_view_use_is_accepted();
    test_two_mut_capture_closures_are_rejected();
    test_by_value_capture_is_not_a_borrow();
    test_shared_capture_closures_are_accepted();
    test_owned_return_keeps_args_free_is_accepted();
    test_for_over_ref_borrow_is_accepted();
    test_mutation_during_ref_for_loop_is_rejected();
    test_reuse_after_by_value_ufcs_call_is_rejected();
    test_reuse_after_into_iter_is_rejected();
    test_repeated_self_method_calls_are_accepted();
    test_repeated_borrowing_ufcs_calls_are_accepted();
    test_repeated_iter_values_chains_are_accepted();
    test_copy_values_reused_are_accepted();
  test_type_param_reused_after_move_is_rejected();
  test_partial_moves_are_accepted();
  test_moves_out_of_places_are_rejected();
  test_outlived_temporaries_are_rejected();
  test_consuming_loop_over_borrow_is_rejected();
  test_overlapping_owned_binding_is_rejected();
  test_reuse_after_move_capture_is_rejected();
    test_reuse_after_plain_value_capture_is_accepted();
    test_reuse_after_for_x_in_ref_is_accepted();
    test_move_in_loop_is_rejected();
    test_move_in_one_branch_is_rejected();
    test_repeated_autoref_calls_are_accepted();
    test_autoref_aliasing_is_rejected();
    test_assign_while_view_live_is_rejected();
    test_replace_while_view_live_is_rejected();
    test_move_while_view_live_is_rejected();
    test_read_under_mut_view_is_rejected();
    test_assign_while_capture_live_is_rejected();
    test_view_assigned_in_branch_is_rejected();
    test_loop_carried_view_is_rejected();
    test_view_dead_before_reassign_in_loop_is_accepted();
    test_view_from_if_expr_is_rejected();
    test_view_from_match_expr_is_rejected();
    test_view_in_tuple_pattern_is_rejected();
    test_view_in_if_let_is_rejected();
    test_returned_reference_is_tracked();
    test_return_view_of_local_is_rejected();
    test_return_borrowing_closure_is_rejected();
    test_view_outliving_source_scope_is_rejected();
    test_return_view_of_parameter_is_accepted();
    test_generic_lambda_moving_capture_twice_is_explained();
    test_move_out_of_capture_is_rejected();
    test_reads_of_captures_are_accepted();
  } catch (const std::exception &ex) {
    std::cerr << "ownership_check_test failed: unhandled exception: "
              << ex.what() << '\n';
    std::exit(1);
  }
  return 0;
}
