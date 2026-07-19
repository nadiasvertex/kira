#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analysis.h"
#include "src/parser/parser.h"
#include "src/semantic/check.h"
#include "src/semantic/types.h"
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
    std::cerr << "check_test: missing diagnostic `" << needle << "`\n"
              << "rendered diagnostics were:\n"
              << analyzed.diagnostics << '\n';
    fail(message);
  }
}

/// Locates the real `src/std` package under whichever invocation shape the
/// test binary is running (`bazel test`'s `TEST_SRCDIR`, or a plain
/// `bazel-bin` invocation from the workspace root) — mirrors
/// `find_test_data_dir` (`src/testing/test_data.h`), pointed at `src/std`
/// instead of a `src/testdata/<test_name>` subdirectory.
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

/// Fixtures for the real auto-injected prelude (`std/traits.kira`,
/// `std/iter.kira`, `std/prelude.kira`, `std/io.kira`, `std/console.kira`,
/// `std/fmt.kira`) — mirrors
/// `inject_stdlib_prelude` (`src/driver/driver.cpp`), which every real
/// `kira` invocation prepends to its session, so bound positions like
/// `T: eq` and `impl show for point`, and `prelude.kira`'s `use
/// std.console`, resolve here the same way they do for a real compile.
auto prelude_fixtures() -> std::vector<source_fixture> {
  const auto std_dir = find_std_dir();
  return {
      source_fixture{
          .path = "std/traits.kira",
          .text = kira::testing::load_test_data_file(std_dir.string(),
                                                     "traits.kira"),
      },
      source_fixture{
          .path = "std/iter.kira",
          .text =
              kira::testing::load_test_data_file(std_dir.string(), "iter.kira"),
      },
      source_fixture{
          .path = "std/prelude.kira",
          .text = kira::testing::load_test_data_file(std_dir.string(),
                                                     "prelude.kira"),
      },
      source_fixture{
          .path = "std/io.kira",
          .text =
              kira::testing::load_test_data_file(std_dir.string(), "io.kira"),
      },
      source_fixture{
          .path = "std/console.kira",
          .text = kira::testing::load_test_data_file(std_dir.string(),
                                                     "console.kira"),
      },
      source_fixture{
          .path = "std/fmt.kira",
          .text =
              kira::testing::load_test_data_file(std_dir.string(), "fmt.kira"),
      },
  };
}

auto analyze_sources(const std::vector<source_fixture> &extra_fixtures)
    -> analyzed_session {
  auto fixtures = prelude_fixtures();
  fixtures.insert(fixtures.end(), extra_fixtures.begin(), extra_fixtures.end());

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
    fail("expected check test fixtures to parse");
  }
  [[maybe_unused]] const auto checked =
      kira::semantic::validate_semantics(parsed_modules, diag, file_has_errors);

  return analyzed_session{
      .diagnostics = kira::diagnostic_renderer(sources, false).render_all(diag),
      .error_count = diag.error_count(),
  };
}

auto analyze_test_data_file(std::string_view filename) -> analyzed_session {
  const auto test_data_dir =
      kira::testing::find_test_data_dir("semantic_check_test");
  const auto text =
      kira::testing::load_test_data_file(test_data_dir.string(), filename);
  return analyze_sources({{.path = std::string(filename), .text = text}});
}

auto analyze_test_data_directory(std::string_view dirname) -> analyzed_session {
  const auto test_data_dir =
      kira::testing::find_test_data_dir("semantic_check_test");
  const auto subdir = test_data_dir / dirname;
  auto fixtures = std::vector<source_fixture>{};

  for (const auto &entry : kira::testing::fs::directory_iterator(subdir)) {
    if (entry.is_regular_file() &&
        entry.path().extension().string() == ".kira") {
      const auto filename = entry.path().filename().string();
      const auto path = kira::testing::fs::path(dirname) / filename;
      const auto text = kira::testing::load_test_data_file(
          test_data_dir.string(), path.string());
      fixtures.push_back(source_fixture{
          .path = path.string(),
          .text = text,
      });
    }
  }

  return analyze_sources(fixtures);
}

// ==========================================================================
//  Programs that must check cleanly
// ==========================================================================

auto test_accepts_typed_core_program() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_typed_core_program.kira");
  expect(analyzed.error_count == 0,
         "expected typed core program to check cleanly");
}

auto test_accepts_structs_and_methods() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_structs_and_methods.kira");
  expect(analyzed.error_count == 0,
         "expected struct/impl program to check cleanly");
}

auto test_accepts_packed_struct() -> void {
  const auto analyzed = analyze_test_data_file("accept_packed_struct.kira");
  expect(analyzed.error_count == 0,
         "expected a packed struct declaration to check cleanly");
}

auto test_accepts_collections_and_lambdas() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_collections_and_lambdas.kira");
  expect(analyzed.error_count == 0,
         "expected collection/lambda program to check cleanly");
}

auto test_accepts_option_result_flow() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_option_result_flow.kira");
  expect(analyzed.error_count == 0,
         "expected option/result program to check cleanly");
}

auto test_accepts_cross_module_qualified_types() -> void {
  const auto analyzed =
      analyze_test_data_directory("accept_cross_module_qualified_types");
  expect(analyzed.error_count == 0,
         "expected qualified types from other session modules to resolve");
}

auto test_accepts_module_qualified_call() -> void {
  const auto analyzed =
      analyze_test_data_directory("accept_module_qualified_call");
  expect(analyzed.error_count == 0,
         "expected a call through a whole-module `use` import to resolve");
}

auto test_accepts_member_import_call() -> void {
  const auto analyzed =
      analyze_test_data_directory("accept_member_import_call");
  expect(analyzed.error_count == 0,
         "expected `use module.member` / `use module.{a, b as c}` imports "
         "to resolve unqualified");
}

auto test_accepts_wildcard_import() -> void {
  const auto analyzed = analyze_test_data_directory("accept_wildcard_import");
  expect(analyzed.error_count == 0,
         "expected a session-owned `use module.*` wildcard import to bring "
         "functions, types, traits, statics, and variants into scope");
}

auto test_accepts_fully_qualified_call() -> void {
  const auto analyzed =
      analyze_test_data_directory("accept_fully_qualified_call");
  expect(analyzed.error_count == 0,
         "expected a fully module-qualified call to resolve");
}

auto test_accepts_type_qualified_associated_call() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_type_qualified_associated_call.kira");
  expect(analyzed.error_count == 0,
         "expected a type-qualified associated-function call to resolve");
}

auto test_accepts_indexing_local_bindings() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_indexing_local_bindings.kira");
  expect(analyzed.error_count == 0,
         "expected indexing a local list binding to check cleanly");
}

auto test_accepts_str_byte_index() -> void {
  const auto analyzed = analyze_test_data_file("accept_str_byte_index.kira");
  expect(analyzed.error_count == 0,
         "expected a scalar `str` index to check cleanly as a `byte`");
}

auto test_reports_str_index_non_integer() -> void {
  const auto analyzed =
      analyze_test_data_file("report_str_index_non_integer.kira");
  expect_diagnostic(analyzed, "index must be an integer type",
                    "expected a non-integer `str` index to be rejected");
}

auto test_accepts_for_over_user_iterator() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_for_over_user_iterator.kira");
  expect(analyzed.error_count == 0,
         "expected a `for` loop over a user type implementing "
         "`std.iter.iterator[T]` to check cleanly (the loop variable typed "
         "as the iterator's element type)");
}

auto test_accepts_type_generic_free_function() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_type_generic_free_function.kira");
  expect(analyzed.error_count == 0,
         "expected a type-generic free function to check cleanly, each call "
         "typed at the concrete type its arguments solve for");
}

auto test_reports_type_generic_unsolved() -> void {
  const auto analyzed =
      analyze_test_data_file("report_type_generic_unsolved.kira");
  expect_diagnostic(analyzed, "cannot tell what `T` is",
                    "expected a call that leaves a type parameter unsolved to "
                    "be rejected");
}

auto test_accepts_associated_type_self_output() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_associated_type_self_output.kira");
  expect(analyzed.error_count == 0,
         "expected `self.output` associated-type return to check cleanly");
}

auto test_accepts_extend_on_builtin_type() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_extend_on_builtin_type.kira");
  expect(analyzed.error_count == 0,
         "expected an extend block on a builtin type to check cleanly");
}

auto test_accepts_extend_on_user_type() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_extend_on_user_type.kira");
  expect(analyzed.error_count == 0,
         "expected an extend block on a user type to check cleanly");
}

auto test_reports_extend_method_arity_mismatch() -> void {
  const auto analyzed =
      analyze_test_data_file("report_extend_method_arity_mismatch.kira");
  expect(analyzed.error_count > 0,
         "expected an extend method call to be checked like a real function");
  expect_diagnostic(analyzed, "missing argument `factor`",
                    "expected real arity checking against the extend method");
}

auto test_reports_missing_return_value() -> void {
  const auto analyzed =
      analyze_test_data_file("report_missing_return_value.kira");
  expect(analyzed.error_count > 0,
         "expected a value-returning function whose body never returns to "
         "be rejected");
  expect_diagnostic(analyzed, "not every path through its body returns",
                    "expected the missing-return diagnostic for a body "
                    "ending in a `let`");
}

auto test_reports_missing_return_on_if_without_else() -> void {
  const auto analyzed =
      analyze_test_data_file("report_missing_return_if_without_else.kira");
  expect(analyzed.error_count >= 2,
         "expected both fall-through `if` shapes to be rejected");
  expect_diagnostic(analyzed, "not every path through its body returns",
                    "expected the missing-return diagnostic for a tail "
                    "`if` with no `else`");
}

auto test_accepts_all_paths_returning() -> void {
  const auto analyzed = analyze_test_data_file("accept_all_paths_return.kira");
  expect(analyzed.error_count == 0,
         "expected functions returning on every path (tail expressions, "
         "if/else, match arms, `while true`) to check cleanly: " +
             analyzed.diagnostics);
}

auto test_impl_method_takes_priority_over_extend() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_impl_method_priority_over_extend.kira");
  expect(analyzed.error_count == 0,
         "expected impl/extend method name collision to check cleanly, "
         "with the impl method winning");
}

auto test_reports_associated_type_output_mismatch() -> void {
  const auto analyzed =
      analyze_test_data_file("report_associated_type_output_mismatch.kira");
  expect(analyzed.error_count > 0,
         "expected `self.output` to resolve to the impl's concrete type");
  expect_diagnostic(analyzed, "expected `str`, found `vec2`",
                    "expected the resolved associated type to drive checking");
}

auto test_accepts_string_interpolation() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_string_interpolation.kira");
  expect(analyzed.error_count == 0,
         "expected string interpolation program to check cleanly: " +
             analyzed.diagnostics);
}

// ==========================================================================
//  Programs that must be rejected, with teaching diagnostics
// ==========================================================================

auto test_reports_undefined_name_with_suggestion() -> void {
  const auto analyzed =
      analyze_test_data_file("report_undefined_name_with_suggestion.kira");
  expect(analyzed.error_count > 0, "expected undefined name to fail");
  expect_diagnostic(analyzed, "undefined name `totl`",
                    "expected undefined-name diagnostic");
  expect_diagnostic(analyzed, "did you mean `total`?",
                    "expected a suggestion for the near-miss name");
}

auto test_reports_undefined_type() -> void {
  const auto analyzed = analyze_test_data_file("report_undefined_type.kira");
  expect(analyzed.error_count > 0, "expected undefined type to fail");
  expect_diagnostic(analyzed, "undefined type `pont`",
                    "expected undefined-type diagnostic");
}

auto test_reports_packed_on_sum_type() -> void {
  const auto analyzed =
      analyze_test_data_file("report_packed_on_sum_type.kira");
  expect(analyzed.error_count > 0, "expected `packed` on a sum type to fail");
  expect_diagnostic(analyzed, "`packed` only applies to struct types",
                    "expected a packed-on-non-struct diagnostic");
  expect_diagnostic(analyzed, "has no field layout to pack",
                    "expected the diagnostic to explain why");
}

auto test_reports_annotation_mismatch() -> void {
  const auto analyzed =
      analyze_test_data_file("report_annotation_mismatch.kira");
  expect(analyzed.error_count > 0, "expected annotation mismatch to fail");
  expect_diagnostic(analyzed, "expected `int32`, found `str`",
                    "expected annotation type-mismatch diagnostic");
}

auto test_reports_quote_type_annotation_mismatch() -> void {
  const auto analyzed =
      analyze_test_data_file("report_quote_type_annotation_mismatch.kira");
  expect(analyzed.error_count > 0,
         "expected a quote-type (`expr`) annotation mismatch to fail");
  expect_diagnostic(analyzed, "expected `expr`, found `str`",
                    "expected quote-type annotation mismatch diagnostic; "
                    "quote_type must resolve to its own builtin instead of "
                    "falling through to `<unknown>`, which unifies with "
                    "everything and would silently accept this");
}

auto test_accepts_quote_expr_typed_by_fragment_kind() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_quote_expr_typed_by_fragment_kind.kira");
  expect(analyzed.error_count == 0,
         std::string("expected each quote to type-check against the "
                     "quote-value type matching its own classified "
                     "`fragment_kind` (`expr`/`stmt`/`def_expr`), not a "
                     "hardcoded `expr`:\n") +
             analyzed.diagnostics);
}

auto test_accepts_splice_expr_reifies_quoted_value() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_splice_expr_reifies_quoted_value.kira");
  expect(analyzed.error_count == 0,
         std::string("expected `~doubled` to graft the quoted `21 + 21` "
                     "fragment in as `run`'s real return expression:\n") +
             analyzed.diagnostics);
}

auto test_accepts_splice_stmt_reifies_quoted_statement() -> void {
  const auto analyzed = analyze_test_data_file(
      "accept_splice_stmt_reifies_quoted_statement.kira");
  expect(analyzed.error_count == 0,
         std::string("expected `~make_binding` to graft the quoted `if "
                     "true: let y = 10 return y` statement into `run`'s "
                     "body, with `y`'s declaration and its own reference "
                     "both inside the same quote resolving consistently "
                     "under M5 hygiene renaming:\n") +
             analyzed.diagnostics);
}

auto test_accepts_expr_builder_constructs_fragment() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_expr_builder_constructs_fragment.kira");
  expect(analyzed.error_count == 0,
         std::string("expected `expr.lit(42)` to construct a real `expr` "
                     "quote value, spliceable via `~built`:\n") +
             analyzed.diagnostics);
}

auto test_accepts_item_splice_injects_impl() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_item_splice_injects_impl.kira");
  expect(analyzed.error_count == 0,
         std::string("expected `~make_show_impl()` at module top level to "
                     "inject a real `impl show for point`, callable as "
                     "`p.show()`:\n") +
             analyzed.diagnostics);
}

auto test_accepts_type_reflection_primitives() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_type_reflection_primitives.kira");
  expect(analyzed.error_count == 0,
         std::string("expected `point.field_count()`/`.name()`/`.fields()` "
                     "to reflect over `point`'s own declaration at compile "
                     "time:\n") +
             analyzed.diagnostics);
}

auto test_accepts_module_reflection() -> void {
  const auto analyzed = analyze_test_data_file("accept_module_reflection.kira");
  expect(analyzed.error_count == 0,
         std::string("expected `sample.name()`/`.type_count()`/"
                     "`.function_count()`/`.types()`/`.functions()` to reflect "
                     "over the module's surface at compile time:\n") +
             analyzed.diagnostics);
}

auto test_accepts_expr_keyword_usable_as_identifier() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_expr_keyword_usable_as_identifier.kira");
  expect(analyzed.error_count == 0,
         std::string("expected `expr`/`stmt` to be usable as ordinary "
                     "identifiers outside type position (contextual "
                     "keywords, M4.5):\n") +
             analyzed.diagnostics);
}

auto test_accepts_splice_type_reifies_quoted_type() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_splice_type_reifies_quoted_type.kira");
  expect(analyzed.error_count == 0,
         std::string("expected `~(int_type)` in return-type position to "
                     "resolve to the quoted `int32` type:\n") +
             analyzed.diagnostics);
}

auto test_reports_splice_expr_wrong_fragment_kind() -> void {
  const auto analyzed =
      analyze_test_data_file("report_splice_expr_wrong_fragment_kind.kira");
  expect(analyzed.error_count > 0,
         "expected splicing a `stmt` fragment in expression position to "
         "fail: only a quoted expression can be spliced there");
  expect_diagnostic(analyzed, "must resolve to a quoted expression",
                    "expected a diagnostic explaining that a quoted "
                    "statement can't be spliced into expression position");
}

auto test_reports_hygiene_prevents_spliced_binding_leak() -> void {
  const auto analyzed = analyze_test_data_file(
      "report_hygiene_prevents_spliced_binding_leak.kira");
  expect(analyzed.error_count > 0,
         "expected `y` (declared only inside the spliced `` `let y: int32 = "
         "10` `` fragment) to be undefined once referenced by hand-written "
         "code at the splice site — M5 hygiene renames it to a fresh "
         "synthetic name so it no longer leaks out as plain `y`");
  expect_diagnostic(analyzed, "undefined name `y`",
                    "expected an undefined-name diagnostic for `y`, proving "
                    "hygiene renamed the spliced binding rather than "
                    "leaving it visible under its original name");
}

auto test_reports_splice_requires_quote_value() -> void {
  const auto analyzed =
      analyze_test_data_file("report_splice_requires_quote_value.kira");
  expect(analyzed.error_count > 0,
         "expected `~(42)` to fail: a splice operand must be a quoted "
         "fragment, not an arbitrary value");
  expect_diagnostic(analyzed, "must be a quoted fragment",
                    "expected a diagnostic explaining that the splice "
                    "operand is not quoted syntax");
}

auto test_accepts_static_let_evaluates() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_static_let_evaluates.kira");
  expect(analyzed.error_count == 0,
         std::string("expected `static limit: int32 = 2 + 3 * 4` to check "
                     "cleanly:\n") +
             analyzed.diagnostics);
}

auto test_reports_static_assert_evaluates_false() -> void {
  const auto analyzed =
      analyze_test_data_file("report_static_assert_evaluates_false.kira");
  expect(analyzed.error_count > 0,
         "expected `static assert 1 == 2` to actually be evaluated and fail "
         "compilation, not just type-checked for `bool`-ness");
  expect_diagnostic(analyzed, "one is not two at compile time",
                    "expected the static assert's own message to be used "
                    "in the diagnostic");
}

auto test_accepts_static_def_call_evaluates() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_static_def_call_evaluates.kira");
  expect(analyzed.error_count == 0,
         std::string("expected `static def square` to be callable from "
                     "`static let nine: int32 = square(3)`:\n") +
             analyzed.diagnostics);
}

auto test_reports_static_for_evaluates_each_iteration() -> void {
  const auto analyzed =
      analyze_test_data_file("report_static_for_evaluates_each_iteration.kira");
  expect(analyzed.error_count > 0,
         "expected `static for` to actually iterate and evaluate its body "
         "per element, catching the third-element assertion failure");
  expect_diagnostic(
      analyzed, "should fail on the third element of the list",
      "expected the per-iteration `static assert` message for n == 3");
}

auto test_accepts_static_struct_value_evaluates() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_static_struct_value_evaluates.kira");
  expect(analyzed.error_count == 0,
         std::string("expected a compile-time struct literal and field "
                     "access to evaluate:\n") +
             analyzed.diagnostics);
}

auto test_accepts_static_let_forward_reference() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_static_let_forward_reference.kira");
  expect(analyzed.error_count == 0,
         std::string("expected `static let doubled` to forward-reference "
                     "`static let base` declared later in the file:\n") +
             analyzed.diagnostics);
}

auto test_accepts_static_if_selects_taken_branch() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_static_if_selects_taken_branch.kira");
  expect(analyzed.error_count == 0,
         std::string("expected only the `else` branch (condition `false`) "
                     "to be checked, so the `if` branch's undefined-name "
                     "reference is never reached:\n") +
             analyzed.diagnostics);
}

auto test_accepts_static_if_selects_branch_by_sum_type_equality() -> void {
  const auto analyzed = analyze_test_data_file(
      "accept_static_if_selects_branch_by_sum_type_equality.kira");
  expect(analyzed.error_count == 0,
         std::string("expected `static if CURRENT == @unix` to evaluate the "
                     "sum-type equality at compile time and select the `if` "
                     "branch, so the `else` branch's undefined-name "
                     "reference is never reached:\n") +
             analyzed.diagnostics);
}

auto test_accepts_static_if_variant_equality_selects_else_branch() -> void {
  const auto analyzed = analyze_test_data_file(
      "accept_static_if_variant_equality_selects_else_branch.kira");
  expect(analyzed.error_count == 0,
         std::string("expected `static if CURRENT == @unix` (CURRENT bound "
                     "to `@windows`) to evaluate false at compile time and "
                     "select the `else` branch, so the `if` branch's "
                     "undefined-name reference is never reached:\n") +
             analyzed.diagnostics);
}

auto test_accepts_static_assert_compares_variant_payloads() -> void {
  const auto analyzed = analyze_test_data_file(
      "accept_static_assert_compares_variant_payloads.kira");
  expect(analyzed.error_count == 0,
         std::string("expected `@other(\"bsd\") == @other(\"bsd\")` to "
                     "compare equal, differing payloads/tags to compare "
                     "unequal, all at compile time:\n") +
             analyzed.diagnostics);
}

auto test_reports_integer_literal_overflow() -> void {
  const auto analyzed =
      analyze_test_data_file("report_integer_literal_overflow.kira");
  expect(analyzed.error_count > 0, "expected literal overflow to fail");
  expect_diagnostic(analyzed, "integer literal `300` does not fit in `uint8`",
                    "expected literal-fit diagnostic");
}

auto test_reports_mixed_numeric_types() -> void {
  const auto analyzed =
      analyze_test_data_file("report_mixed_numeric_types.kira");
  expect(analyzed.error_count > 0, "expected mixed numerics to fail");
  expect_diagnostic(analyzed, "mismatched numeric types `int32` and `float64`",
                    "expected mixed-numeric diagnostic");
}

auto test_reports_non_bool_condition() -> void {
  const auto analyzed =
      analyze_test_data_file("report_non_bool_condition.kira");
  expect(analyzed.error_count > 0, "expected non-bool condition to fail");
  expect_diagnostic(analyzed, "must be `bool`, found `int32`",
                    "expected bool-condition diagnostic");
}

auto test_reports_assignment_to_immutable() -> void {
  const auto analyzed =
      analyze_test_data_file("report_assignment_to_immutable.kira");
  expect(analyzed.error_count > 0, "expected immutable assignment to fail");
  expect_diagnostic(analyzed, "cannot assign to immutable binding `count`",
                    "expected immutable-assignment diagnostic");
}

auto test_accepts_let_mut_reassignment() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_let_mut_reassignment.kira");
  expect(analyzed.error_count == 0, "expected `let mut` to allow reassignment");
}

auto test_reports_return_type_mismatch() -> void {
  const auto analyzed =
      analyze_test_data_file("report_return_type_mismatch.kira");
  expect(analyzed.error_count > 0, "expected return mismatch to fail");
  expect_diagnostic(analyzed, "return type mismatch: expected `str`",
                    "expected return-mismatch diagnostic");
}

auto test_reports_call_argument_problems() -> void {
  const auto analyzed =
      analyze_test_data_file("report_call_argument_problems.kira");
  expect(analyzed.error_count > 0, "expected call problems to fail");
  expect_diagnostic(analyzed, "too many arguments to `greet`",
                    "expected arity diagnostic");
  expect_diagnostic(analyzed, "unknown named argument `volume`",
                    "expected named-argument diagnostic");
  expect_diagnostic(analyzed, "missing argument `name`",
                    "expected missing-argument diagnostic");
  expect_diagnostic(analyzed, "expected `str`, found `int32`",
                    "expected argument type diagnostic");
}

auto test_reports_struct_literal_problems() -> void {
  const auto analyzed =
      analyze_test_data_file("report_struct_literal_problems.kira");
  expect(analyzed.error_count > 0, "expected struct literal problems to fail");
  expect_diagnostic(analyzed, "unknown field `z` in struct literal for `point`",
                    "expected unknown-field diagnostic");
  expect_diagnostic(analyzed, "missing field `y` in struct literal for `point`",
                    "expected missing-field diagnostic");
}

auto test_reports_unknown_field_and_method() -> void {
  const auto analyzed =
      analyze_test_data_file("report_unknown_field_and_method.kira");
  expect(analyzed.error_count > 0, "expected member problems to fail");
  expect_diagnostic(analyzed, "no field `z` on struct `point`",
                    "expected unknown-field diagnostic");
  expect_diagnostic(analyzed, "no method `normalize` on type `point`",
                    "expected unknown-method diagnostic");
}

auto test_reports_non_exhaustive_match() -> void {
  const auto analyzed =
      analyze_test_data_file("report_non_exhaustive_match.kira");
  expect(analyzed.error_count > 0, "expected non-exhaustive match to fail");
  expect_diagnostic(analyzed,
                    "non-exhaustive match on `shape`: `@rect`, `@point` not "
                    "covered",
                    "expected exhaustiveness diagnostic");
}

auto test_reports_unknown_variant_in_pattern() -> void {
  const auto analyzed =
      analyze_test_data_file("report_unknown_variant_in_pattern.kira");
  expect(analyzed.error_count > 0, "expected unknown variant to fail");
  expect_diagnostic(analyzed, "unknown variant `@square` for sum type `shape`",
                    "expected unknown-variant diagnostic");
}

auto test_reports_try_in_plain_function() -> void {
  const auto analyzed =
      analyze_test_data_file("report_try_in_plain_function.kira");
  expect(analyzed.error_count > 0, "expected `?` misuse to fail");
  expect_diagnostic(analyzed,
                    "cannot use `?` in a function that returns `int32`",
                    "expected try-propagation diagnostic");
}

auto test_reports_unannotated_pub_function() -> void {
  const auto analyzed =
      analyze_test_data_file("report_unannotated_pub_function.kira");
  expect(analyzed.error_count > 0, "expected unannotated pub fn to fail");
  expect_diagnostic(analyzed,
                    "public function `run` must annotate its parameters and "
                    "return type",
                    "expected pub-annotation diagnostic");
}

auto test_reports_duplicate_trait_impl() -> void {
  const auto analyzed =
      analyze_test_data_file("report_duplicate_trait_impl.kira");
  expect(analyzed.error_count > 0, "expected duplicate impl to fail");
  expect_diagnostic(analyzed,
                    "duplicate implementation of trait `show2` for `point`",
                    "expected coherence diagnostic");
}

auto test_reports_incomplete_trait_impl() -> void {
  const auto analyzed =
      analyze_test_data_file("report_incomplete_trait_impl.kira");
  expect(analyzed.error_count > 0, "expected incomplete impl to fail");
  expect_diagnostic(analyzed,
                    "implementation of trait `shape2` for `point` is missing "
                    "method `name`",
                    "expected missing-method diagnostic");
  expect_diagnostic(analyzed, "`extra` is not a member of trait `shape2`",
                    "expected extra-method diagnostic");
}

auto test_accepts_drop_impl() -> void {
  const auto analyzed = analyze_test_data_file("accept_drop_impl.kira");
  expect(analyzed.error_count == 0,
         "expected a well-formed `impl drop for T` to typecheck");
}

auto test_reports_duplicate_drop_impl() -> void {
  const auto analyzed =
      analyze_test_data_file("report_duplicate_drop_impl.kira");
  expect(analyzed.error_count > 0, "expected duplicate drop impl to fail");
  expect_diagnostic(analyzed,
                    "duplicate implementation of trait `drop` for `resource`",
                    "expected coherence diagnostic");
}

auto test_reports_incomplete_drop_impl() -> void {
  const auto analyzed =
      analyze_test_data_file("report_incomplete_drop_impl.kira");
  expect(analyzed.error_count > 0, "expected incomplete drop impl to fail");
  expect_diagnostic(analyzed,
                    "implementation of trait `drop` for `resource` is "
                    "missing method `drop`",
                    "expected missing-method diagnostic");
  expect_diagnostic(analyzed, "`close` is not a member of trait `drop`",
                    "expected extra-method diagnostic");
}

auto test_reports_unsatisfied_trait_requirement() -> void {
  const auto analyzed =
      analyze_test_data_file("report_unsatisfied_trait_requirement.kira");
  expect(analyzed.error_count > 0, "expected unsatisfied requires to fail");
  expect_diagnostic(analyzed,
                    "trait `basic_ord` requires `basic_eq`, but `point` does "
                    "not implement it",
                    "expected requires diagnostic");
}

auto test_reports_unknown_deriving() -> void {
  const auto analyzed = analyze_test_data_file("report_unknown_deriving.kira");
  expect(analyzed.error_count > 0, "expected unknown deriving to fail");
  expect_diagnostic(analyzed, "cannot derive `serialize` for `color`",
                    "expected deriving diagnostic");
}

auto test_reports_impure_contract_call() -> void {
  const auto analyzed =
      analyze_test_data_file("report_impure_contract_call.kira");
  expect(analyzed.error_count > 0, "expected impure contract call to fail");
  expect_diagnostic(analyzed,
                    "contract conditions may only call pure functions",
                    "expected contract-purity diagnostic");
}

auto test_reports_string_interpolation_unsupported_style() -> void {
  const auto analyzed = analyze_test_data_file(
      "report_string_interpolation_unsupported_style.kira");
  expect(analyzed.error_count > 0,
         "expected `str` with `:x` to fail — `str` has no `hex` capability");
  expect_diagnostic(analyzed, "does not support x formatting",
                    "expected an unsupported-format-style diagnostic");
}

auto test_reports_string_interpolation_precision_on_integer_style() -> void {
  const auto analyzed = analyze_test_data_file(
      "report_string_interpolation_precision_on_integer_style.kira");
  expect(analyzed.error_count > 0, "expected `.precision` on `x` to fail");
  expect_diagnostic(analyzed, "`.precision` is not allowed with `x`",
                    "expected a precision-on-integer-style diagnostic");
}

auto test_reports_string_interpolation_bad_dynamic_width() -> void {
  const auto analyzed = analyze_test_data_file(
      "report_string_interpolation_bad_dynamic_width.kira");
  expect(analyzed.error_count > 0,
         "expected a non-usize dynamic width to fail");
  expect_diagnostic(analyzed, "format width must be `usize`, found `str`",
                    "expected a dynamic-width-must-be-usize diagnostic");
}

// ==========================================================================
//  Persisted checked types (checked_types / check_program)
// ==========================================================================

// ==========================================================================
//  Dependent and refinement types (`spec/dependent-types-design.md`)
// ==========================================================================

auto test_accepts_dependent_length_arithmetic() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_dependent_length_arithmetic.kira");
  expect(analyzed.error_count == 0,
         "expected symbolic const-generic arithmetic to check cleanly");
}

auto test_reports_dependent_length_refuted() -> void {
  const auto analyzed =
      analyze_test_data_file("report_dependent_length_refuted.kira");
  expect(analyzed.error_count > 0,
         "expected `head` on an empty vec to be rejected");
  // The diagnostic must explain the *arithmetic*, not just report a mismatch:
  // the whole point is that the reader learns why no `n` could ever work.
  expect_diagnostic(analyzed, "`n + 1` can never equal `0` for `n: usize`",
                    "expected the arithmetic impossibility to be explained");
}

auto test_accepts_proved_refinements() -> void {
  const auto analyzed = analyze_test_data_file("accept_refinement_proved.kira");
  expect(analyzed.error_count == 0,
         "expected provable refinement obligations to check cleanly");
}

auto test_reports_unproven_refinement() -> void {
  const auto analyzed =
      analyze_test_data_file("report_refinement_unproven.kira");
  expect(analyzed.error_count > 0,
         "expected an unprovable refinement obligation to be reported");
  // The failure UX *is* the feature (design doc section 6): name the goal, and
  // give both ways out. A bad message here makes the compiler feel broken
  // rather than conservative, so it is asserted on rather than left to drift.
  expect_diagnostic(analyzed, "cannot prove `y > 0`",
                    "expected the unproven goal to be named in source terms");
  expect_diagnostic(analyzed, "try_from",
                    "expected the runtime-proof escape hatch to be offered");
}

auto test_reports_refuted_refinement() -> void {
  const auto analyzed =
      analyze_test_data_file("report_refinement_refuted.kira");
  expect(analyzed.error_count > 0,
         "expected refuted refinement obligations to be reported");
  expect_diagnostic(analyzed, "`0 - 3 > 0` is never true here",
                    "expected a refuted narrowing to be reported as refuted, "
                    "not merely unproven");
  expect_diagnostic(analyzed, "`9 < 4` is never true here",
                    "expected an out-of-range refined index to be refuted");
  expect_diagnostic(analyzed, "index out of bounds: `9 < 4` is never true",
                    "expected a provably out-of-bounds index to be rejected");
}

auto test_accepts_flow_narrowed_refinement() -> void {
  const auto analyzed = analyze_test_data_file("accept_refinement_flow.kira");
  expect(analyzed.error_count == 0,
         "expected a path condition to discharge a refinement obligation");
}

auto test_reports_refinement_outside_narrowed_path() -> void {
  const auto analyzed = analyze_test_data_file("report_refinement_flow.kira");
  expect(analyzed.error_count > 0,
         "expected narrowing to be scoped to the region it dominates");
  // In the `else`, the *negation* holds — so this isn't merely unproven, it
  // is refuted, and saying so is more useful than saying "cannot prove".
  expect_diagnostic(analyzed, "`y > 0` is never true here",
                    "expected the `else` branch to refute the predicate");
  // And an assignment must forget what was known: the value the condition
  // was about no longer exists.
  expect_diagnostic(analyzed, "cannot prove `y > 0`",
                    "expected assignment to invalidate a narrowed fact");
}

auto test_accepts_proved_contracts() -> void {
  const auto analyzed = analyze_test_data_file("accept_contract_proved.kira");
  expect(analyzed.error_count == 0,
         "expected provable and unprovable contracts alike to check cleanly");
}

auto test_reports_refuted_precondition() -> void {
  const auto analyzed = analyze_test_data_file("report_contract_refuted.kira");
  expect(analyzed.error_count > 0,
         "expected a statically violated precondition to be a compile error");
  expect_diagnostic(analyzed, "precondition `0 > 0` is never true at this call",
                    "expected a refuted precondition to name the condition");
  expect_diagnostic(analyzed,
                    "precondition `9000 < 4096` is never true at this call",
                    "expected every refuted precondition to be reported");
  // The contract's own message is the best explanation available; use it.
  expect_diagnostic(analyzed, "cannot reserve zero bytes",
                    "expected the contract's message to be surfaced");
}

auto test_reports_return_outside_a_postcondition() -> void {
  const auto analyzed =
      analyze_test_data_file("report_contract_return_misuse.kira");
  expect(analyzed.error_count > 0,
         "expected `return` outside a postcondition to be an error");
  expect_diagnostic(analyzed, "may only appear in a `post` condition",
                    "expected `pre return > 0` to explain that a precondition "
                    "runs before there is a returned value");
  expect_diagnostic(analyzed, "this function returns `unit`",
                    "expected a `post` about a `unit` result to say why it "
                    "can never constrain anything");
}

auto test_check_program_persists_expression_types() -> void {
  // `check_program` used to return `void`, discarding every type it computed
  // the moment it returned. This confirms the replacement `checked_types` —
  // the interned `type_table` plus a node -> type_id map — actually carries
  // a real expression's resolved type back to the caller, not just that the
  // API compiles.
  auto sources = kira::source_manager{};
  auto diag = kira::diagnostic_bag{};
  auto file_has_errors = std::vector<bool>{};

  const auto test_data_dir =
      kira::testing::find_test_data_dir("semantic_check_test");
  const auto text = kira::testing::load_test_data_file(
      test_data_dir.string(), "check_program_persists_expression_types.kira");
  const auto file_id = sources.add_file("sample.kira", text);
  expect(file_id.has_value(), "expected fixture source to register");
  file_has_errors.resize(static_cast<size_t>(*file_id) + 1, false);

  const auto *file = sources.get(*file_id);
  expect(file != nullptr, "expected registered fixture source");
  auto lexer = kira::lexer(file->source(), file->id(), diag);
  auto tokens = lexer.tokenize();
  auto parser = kira::parser(std::move(tokens), file->id(), diag);
  auto ast_file = parser.parse_file();
  expect(diag.error_count() == 0, "expected fixture to parse cleanly");

  const auto parsed_modules = std::vector<kira::semantic::parsed_module>{
      kira::semantic::parsed_module{.file_id = *file_id,
                                    .ast_file = ast_file.get()},
  };
  auto checked =
      kira::semantic::check_program(parsed_modules, diag, file_has_errors);
  expect(diag.error_count() == 0, "expected fixture to check cleanly");

  const kira::ast::func_decl *add_decl = nullptr;
  for (const auto &item : ast_file->items) {
    if (item != nullptr && item->kind == kira::ast::node_kind::func_decl) {
      const auto &decl = dynamic_cast<const kira::ast::func_decl &>(*item);
      if (decl.name == "add") {
        add_decl = &decl;
        break;
      }
    }
  }
  expect(add_decl != nullptr, "expected to find `add`'s declaration");
  expect(add_decl->body_stmts.size() == 1,
         "expected `add`'s body to be a single `return` statement");

  const auto &return_stmt = dynamic_cast<const kira::ast::return_stmt &>(
      *add_decl->body_stmts.front());
  expect(return_stmt.value != nullptr,
         "expected `return x + y` to have a value");

  const auto it = checked.node_types.find(return_stmt.value.get());
  expect(it != checked.node_types.end(),
         "expected `x + y`'s resolved type to be persisted in node_types");
  expect(it->second == checked.types.builtin("int32"),
         "expected `x + y` to have been resolved to `int32`");
}

// ==========================================================================
//  Local parameter-usage inference
// ==========================================================================

auto test_bare_literal_never_forces_a_concrete_param_type() -> void {
  // Per spec/kira-reference.md: "def double(x): return x * 2" must stay
  // callable with every numeric type, chosen at each call — arithmetic
  // against a bare literal must never collapse `x` to whichever type the
  // literal happens to default to (int32). Both an int and a float call
  // must be accepted.
  const auto analyzed = analyze_test_data_file(
      "bare_literal_never_forces_concrete_param_type.kira");
  expect(analyzed.error_count == 0,
         "expected `double` to stay generic over every numeric type, not "
         "collapse to whichever type the literal `2` defaults to");
}

auto test_infers_param_type_from_annotated_sibling() -> void {
  const auto analyzed =
      analyze_test_data_file("infer_param_type_from_annotated_sibling.kira");
  expect(analyzed.error_count > 0,
         "expected `x` to be inferred as `int32` from unifying with the "
         "annotated `y`");
  expect_diagnostic(analyzed, "expected `int32`, found `str`",
                    "expected a type mismatch naming the inferred parameter "
                    "type");
}

auto test_infers_param_type_from_call_to_annotated_function() -> void {
  const auto analyzed = analyze_test_data_file(
      "infer_param_type_from_call_to_annotated_function.kira");
  expect(analyzed.error_count > 0,
         "expected `relay`'s `x` to be inferred as `int32` from the call to "
         "`sink`");
  expect_diagnostic(analyzed, "expected `int32`, found `str`",
                    "expected a type mismatch naming the inferred parameter "
                    "type");
}

auto test_pass_through_param_stays_unannotated() -> void {
  const auto analyzed =
      analyze_test_data_file("pass_through_param_stays_unannotated.kira");
  expect(analyzed.error_count == 0,
         "a parameter with no recognizable usage constraint must stay "
         "compatible with any argument type, exactly as it was before "
         "inference existed");
}

auto test_infers_param_type_from_struct_field_type() -> void {
  const auto analyzed =
      analyze_test_data_file("infer_param_type_from_struct_field_type.kira");
  expect(analyzed.error_count > 0,
         "expected `make`'s `x` to be inferred as `int32` from `point`'s "
         "`x` field");
  expect_diagnostic(analyzed, "expected `int32`, found `str`",
                    "expected a type mismatch naming the inferred parameter "
                    "type");
}

auto test_infers_param_type_from_struct_field_shorthand() -> void {
  const auto analyzed = analyze_test_data_file(
      "infer_param_type_from_struct_field_shorthand.kira");
  expect(analyzed.error_count > 0,
         "expected `make`'s `x` to be inferred as `int32` from `point`'s "
         "`x` field via shorthand initialization");
  expect_diagnostic(analyzed, "expected `int32`, found `str`",
                    "expected a type mismatch naming the inferred parameter "
                    "type");
}

auto test_infers_param_type_from_annotated_let_binding() -> void {
  const auto analyzed = analyze_test_data_file(
      "infer_param_type_from_annotated_let_binding.kira");
  expect(analyzed.error_count > 0,
         "expected `relay`'s `x` to be inferred as `int32` from the "
         "explicitly annotated local `y`");
  expect_diagnostic(analyzed, "expected `int32`, found `str`",
                    "expected a type mismatch naming the inferred parameter "
                    "type");
}

auto test_infers_param_type_from_annotated_var_binding() -> void {
  const auto analyzed = analyze_test_data_file(
      "infer_param_type_from_annotated_var_binding.kira");
  expect(analyzed.error_count > 0,
         "expected `relay`'s `x` to be inferred as `int32` from the "
         "explicitly annotated local `var y`");
  expect_diagnostic(analyzed, "expected `int32`, found `str`",
                    "expected a type mismatch naming the inferred parameter "
                    "type");
}

auto test_infers_param_type_from_declared_return_type() -> void {
  const auto analyzed =
      analyze_test_data_file("infer_param_type_from_declared_return_type.kira");
  expect(analyzed.error_count > 0,
         "expected `identity`'s `x` to be inferred as `int32` from its own "
         "declared return type");
  expect_diagnostic(analyzed, "expected `int32`, found `str`",
                    "expected a type mismatch naming the inferred parameter "
                    "type");
}

auto test_infers_param_type_from_declared_return_type_on_expr_body() -> void {
  const auto analyzed = analyze_test_data_file(
      "infer_param_type_from_declared_return_type_on_expr_body.kira");
  expect(analyzed.error_count > 0,
         "expected `identity`'s `x` to be inferred as `int32` from its own "
         "declared return type on a compact expression body");
  expect_diagnostic(analyzed, "expected `int32`, found `str`",
                    "expected a type mismatch naming the inferred parameter "
                    "type");
}

auto test_method_call_receiver_subexpr_still_inferred() -> void {
  // `a`'s only usage is inside `(a + b)`, which becomes the *receiver* of a
  // method call (`.to_uppercase()`). The method call itself can't anchor
  // anything (no method-resolution table in this pass), but the arithmetic
  // link between `a` and the annotated `b` lives inside that receiver
  // subtree and must still be walked, not silently skipped because the
  // call's callee isn't a plain name.
  const auto analyzed =
      analyze_test_data_file("infer_param_in_method_call_receiver.kira");
  expect(analyzed.error_count > 0,
         "expected `combine`'s `a` to be inferred as `int32` via arithmetic "
         "with `b` inside a method-call receiver expression");
  expect_diagnostic(analyzed, "expected `int32`, found `str`",
                    "expected a type mismatch naming the inferred parameter "
                    "type");
}

auto test_recursive_function_param_inference_terminates() -> void {
  // `n`'s only usage is comparison/arithmetic against bare literals, so it
  // must stay generic (no anchor forces it to `int32`); this test exists to
  // confirm inference over a self-recursive call doesn't hang or crash, not
  // that `n` gets pinned to one type.
  const auto analyzed =
      analyze_test_data_file("infer_param_recursive_function.kira");
  expect(analyzed.error_count == 0,
         "expected self-recursive parameter inference to terminate cleanly "
         "without wrongly narrowing `n`");
}

auto test_reports_const_generic_value_mismatch() -> void {
  const auto analyzed =
      analyze_test_data_file("report_const_generic_value_mismatch.kira");
  expect(analyzed.error_count > 0,
         "expected mismatched const-generic arguments to fail");
  expect_diagnostic(
      analyzed, "expected `vec[int32, 3]`, found `vec[int32, 5]`",
      "expected a diagnostic distinguishing const-generic instantiations");
}

auto test_accepts_const_generic_value_match() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_const_generic_value_match.kira");
  expect(analyzed.error_count == 0,
         "expected matching const-generic arguments to check cleanly");
}

auto test_accepts_const_generic_try_from() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_const_generic_try_from.kira");
  expect(analyzed.error_count == 0,
         "expected `index[n].try_from` inside a function generic over `n` to "
         "check cleanly — each call site monomorphizes it against a constant");
}

auto test_reports_unsolved_const_generic_value_param() -> void {
  const auto analyzed =
      analyze_test_data_file("report_const_generic_unsolved_value_param.kira");
  expect(analyzed.error_count > 0,
         "expected a call that fixes no value for `n` to fail");
  expect_diagnostic(analyzed, "cannot tell what `n` is in this call to `zeros`",
                    "expected the diagnostic to name the value parameter no "
                    "argument determines");
}

auto test_accepts_explicit_generic_args() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_explicit_generic_args.kira");
  expect(analyzed.error_count == 0,
         "expected a call to give its callee's compile-time arguments in "
         "brackets, including for a parameter no argument could determine");
}

auto test_accepts_generic_methods() -> void {
  const auto analyzed = analyze_test_data_file("accept_generic_methods.kira");
  expect(analyzed.error_count == 0,
         "expected const-generic, type-generic, and mixed generic methods on "
         "an `extend` target to monomorphize like generic free functions");
}

auto test_reports_bad_explicit_generic_args() -> void {
  const auto analyzed =
      analyze_test_data_file("report_bad_explicit_generic_args.kira");
  expect(analyzed.error_count > 0,
         "expected unusable compile-time arguments to fail");
  expect_diagnostic(
      analyzed, "`zeros`'s compile-time argument `n` is not a constant",
      "expected a value argument that doesn't fold to be reported");
  expect_diagnostic(analyzed,
                    "`identity`'s compile-time argument `T` is not a type this "
                    "call can name",
                    "expected a type argument naming no type to be reported");
  expect_diagnostic(
      analyzed, "`zeros` takes 1 compile-time argument(s), and this call gives",
      "expected too many bracket arguments to be reported");
}

auto test_accepts_return_position_inference() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_return_position_inference.kira");
  expect(analyzed.error_count == 0,
         "expected a type parameter appearing only in the return type to be "
         "solved from the type each call site expects, in every position the "
         "checker knows one");
}

auto test_reports_unsolved_return_position_type_param() -> void {
  const auto analyzed =
      analyze_test_data_file("report_unsolved_return_position_type_param.kira");
  expect(analyzed.error_count > 0,
         "expected a call with neither arguments nor context to fail");
  expect_diagnostic(analyzed,
                    "cannot tell what `T` is in this call to `nothing`",
                    "expected an unannotated binding to leave `T` unsolved");
  // The expected type is a fallback, never a constraint: it must not be able
  // to re-solve `T` to `int64` and retype the call.
  expect_diagnostic(analyzed, "expected `int64`, found `int32`",
                    "expected an annotation disagreeing with the arguments to "
                    "stay an ordinary type mismatch");
}

auto test_accepts_method_explicit_generic_args() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_method_explicit_generic_args.kira");
  expect(analyzed.error_count == 0,
         "expected brackets on a method call to be honored, and a nested "
         "generic to be usable as an explicit type argument");
}

auto test_reports_bad_method_explicit_generic_args() -> void {
  const auto analyzed =
      analyze_test_data_file("report_bad_method_explicit_generic_args.kira");
  expect(analyzed.error_count > 0,
         "expected unusable brackets on a method call to fail");
  expect_diagnostic(analyzed,
                    "`echo`'s `T` is given as `int64` in brackets, but the "
                    "arguments make it `int32`",
                    "expected brackets disagreeing with the arguments to be "
                    "reported rather than silently discarded");
  expect_diagnostic(analyzed, "`plain` takes no compile-time parameters",
                    "expected brackets on a non-generic method to be reported");
}

auto test_accepts_type_param_static_dispatch() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_type_param_static_dispatch.kira");
  expect(analyzed.error_count == 0,
         "expected a static member reached through a solved type parameter to "
         "resolve against the type that parameter was solved to");
}

auto test_reports_type_param_static_not_found() -> void {
  const auto analyzed =
      analyze_test_data_file("report_type_param_static_not_found.kira");
  expect(analyzed.error_count > 0,
         "expected a type parameter solved to a type without the needed "
         "static to fail");
  expect_diagnostic(analyzed, "no static `make` on type `int32`",
                    "expected the diagnostic to name the solved type rather "
                    "than report `C` as an undefined name");
  expect_diagnostic(analyzed, "instantiated from here",
                    "expected an instantiation-site note pointing back at the "
                    "call that asked for this instance");
  expect_diagnostic(analyzed,
                    "`C` was solved to `int32` from the type expected here",
                    "expected a note saying where the solution came from, "
                    "since the user never wrote `int32` as a type argument");
}

auto test_reports_refinement_predicate_not_bool() -> void {
  const auto analyzed =
      analyze_test_data_file("report_refinement_predicate_not_bool.kira");
  expect(analyzed.error_count > 0,
         "expected a non-bool refinement predicate to fail");
  expect_diagnostic(analyzed,
                    "a refinement predicate must be `bool`, found `int32`",
                    "expected a refinement-predicate bool-ness diagnostic");
}

auto test_accepts_refinement_predicate() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_refinement_predicate.kira");
  expect(analyzed.error_count == 0,
         "expected a well-formed refinement predicate to check cleanly");
}

auto test_accepts_concept_bound() -> void {
  const auto analyzed = analyze_test_data_file("accept_concept_bound.kira");
  expect(analyzed.error_count == 0,
         "expected a concept composing trait and value constraints, used as "
         "a function bound, to check cleanly");
}

auto test_reports_state_machine_mismatch() -> void {
  const auto analyzed =
      analyze_test_data_file("report_state_machine_mismatch.kira");
  expect(analyzed.error_count > 0,
         "expected a connection[closed] argument where connection[open] is "
         "required to fail");
  expect_diagnostic(
      analyzed, "expected `connection[open]`, found `connection[closed]`",
      "expected a diagnostic rejecting the wrong connection state");
}

auto test_accepts_state_machine_match() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_state_machine_match.kira");
  expect(analyzed.error_count == 0,
         "expected a connection[open] argument where connection[open] is "
         "required to check cleanly");
}

auto test_accepts_intrinsic_decl() -> void {
  const auto analyzed = analyze_test_data_file("accept_intrinsic_decl.kira");
  expect(analyzed.error_count == 0,
         "expected recognized, fully-annotated intrinsic decls to check "
         "cleanly");
}

auto test_reports_unknown_intrinsic() -> void {
  const auto analyzed = analyze_test_data_file("report_unknown_intrinsic.kira");
  expect(analyzed.error_count > 0, "expected unknown intrinsic to fail");
  expect_diagnostic(analyzed, "is not a recognized intrinsic",
                    "expected unknown-intrinsic diagnostic");
  expect_diagnostic(analyzed, "did you mean `rt_write`?",
                    "expected a did-you-mean suggestion for `rt_writ`");
}

auto test_reports_unannotated_intrinsic() -> void {
  const auto analyzed =
      analyze_test_data_file("report_unannotated_intrinsic.kira");
  expect(analyzed.error_count > 0,
         "expected unannotated intrinsic parameter to fail");
  expect_diagnostic(analyzed, "must annotate every parameter",
                    "expected missing-annotation diagnostic");
}

auto test_accepts_generator_yields_typed_values() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_generator_yields_typed_values.kira");
  expect(analyzed.error_count == 0,
         "expected a well-typed generator to check cleanly");
}

auto test_accepts_for_loop_over_generator() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_for_loop_over_generator.kira");
  expect(analyzed.error_count == 0,
         "expected `for x in <generator>` to check cleanly, binding `x` to "
         "the generator's item type");
}

auto test_reports_yield_outside_generator() -> void {
  const auto analyzed =
      analyze_test_data_file("report_yield_outside_generator.kira");
  expect(analyzed.error_count > 0,
         "expected `yield` outside a generator to fail");
  expect_diagnostic(analyzed, "`yield` outside a `generator def` body",
                    "expected a yield-outside-generator diagnostic");
}

auto test_reports_generator_missing_return_type() -> void {
  const auto analyzed =
      analyze_test_data_file("report_generator_missing_return_type.kira");
  expect(analyzed.error_count > 0,
         "expected a generator with no declared return type to fail");
  expect_diagnostic(analyzed, "must declare a `some iterator[T]` return type",
                    "expected a missing-generator-return-type diagnostic");
}

auto test_reports_generator_yield_type_mismatch() -> void {
  const auto analyzed =
      analyze_test_data_file("report_generator_yield_type_mismatch.kira");
  expect(analyzed.error_count > 0, "expected a mismatched yield value to fail");
  expect_diagnostic(analyzed, "expected `int32`, found `str`",
                    "expected a yield type-mismatch diagnostic");
}

auto test_reports_generator_return_with_value() -> void {
  const auto analyzed =
      analyze_test_data_file("report_generator_return_with_value.kira");
  expect(analyzed.error_count > 0,
         "expected `return <value>` inside a generator to fail");
  expect_diagnostic(analyzed,
                    "`return <value>` is not allowed inside a `generator def`",
                    "expected a return-with-value-in-generator diagnostic");
}

auto test_reports_generator_postcondition() -> void {
  const auto analyzed =
      analyze_test_data_file("report_generator_postcondition.kira");
  expect(analyzed.error_count > 0,
         "expected a `post` condition on a `generator def` to fail");
  expect_diagnostic(
      analyzed,
      "a `post` condition cannot be written on the `generator def` `counter`",
      "expected the diagnostic to name the generator the `post` is on");
  expect_diagnostic(analyzed, "a generator has no single result to describe",
                    "expected the diagnostic to explain why");
  // The `pre` on the same declaration keeps its ordinary meaning — only the
  // `post` face is refused, so the refusal is the *only* error reported.
  expect(analyzed.error_count == 1,
         "expected the generator's `pre` to be accepted, leaving the rejected "
         "`post` as the sole error");
}

auto test_reports_existential_type_outside_generator() -> void {
  // `some iterator[T]` is a perfectly legal *general* existential return
  // type now (not only generator sugar) — but `yield` still requires a
  // real `generator def`, and a bare `yield 1` gives this function no
  // concrete value to back the existential type with either, so it still
  // fails, just via two different (and more specific) diagnostics than the
  // old blanket "only supported on generator def" rejection.
  const auto analyzed =
      analyze_test_data_file("report_existential_type_outside_generator.kira");
  expect(analyzed.error_count > 0,
         "expected `yield` outside a generator, with no concrete return "
         "value, to fail");
  expect_diagnostic(analyzed, "`yield` outside a `generator def` body",
                    "expected a yield-outside-generator diagnostic");
  expect_diagnostic(analyzed,
                    "could not infer a concrete return type for `counter`",
                    "expected a no-concrete-value diagnostic");
}

auto test_accepts_existential_return_type() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_existential_return_type.kira");
  expect(analyzed.error_count == 0,
         "expected a general `some Trait` return type, backed by a real "
         "impl, to check cleanly");
}

auto test_accepts_existential_return_type_combined_bounds() -> void {
  const auto analyzed = analyze_test_data_file(
      "accept_existential_return_type_combined_bounds.kira");
  expect(analyzed.error_count == 0,
         "expected `some A + B` and a chained call staying opaque to check "
         "cleanly");
}

auto test_reports_existential_return_type_in_parameter() -> void {
  const auto analyzed = analyze_test_data_file(
      "report_existential_return_type_in_parameter.kira");
  expect(analyzed.error_count > 0,
         "expected `some Trait` in parameter position to fail");
  expect_diagnostic(
      analyzed,
      "`some Trait[Args]` is only supported as a function's return type",
      "expected an existential-not-allowed-here diagnostic");
}

auto test_reports_existential_bound_not_satisfied() -> void {
  const auto analyzed =
      analyze_test_data_file("report_existential_bound_not_satisfied.kira");
  expect(analyzed.error_count > 0,
         "expected a concrete type missing the bound trait to fail");
  expect_diagnostic(analyzed, "`box_thing` does not implement `describable`",
                    "expected an existential-bound-not-satisfied diagnostic");
}

auto test_reports_existential_return_type_mismatch() -> void {
  const auto analyzed =
      analyze_test_data_file("report_existential_return_type_mismatch.kira");
  expect(analyzed.error_count > 0,
         "expected two branches returning different concrete types to fail");
  expect_diagnostic(analyzed,
                    "existential return branches have mismatched "
                    "types",
                    "expected an existential-return-mismatch diagnostic");
}

auto test_reports_existential_method_not_in_bound() -> void {
  const auto analyzed =
      analyze_test_data_file("report_existential_method_not_in_bound.kira");
  expect(analyzed.error_count > 0,
         "expected field access on an opaque existential value to fail");
  expect_diagnostic(analyzed, "no field `n` on the existential type",
                    "expected an existential-field-access diagnostic");
}

// ==========================================================================
//  Higher-kinded traits
// ==========================================================================

auto test_accepts_higher_kinded_functor_monad() -> void {
  const auto analyzed =
      analyze_test_data_file("accept_higher_kinded_functor_monad.kira");
  expect(analyzed.error_count == 0,
         "expected the spec functor/monad program (HK traits, impls for "
         "`option`, and calls through them) to check cleanly");
}

auto test_reports_hk_impl_target_wrong_kind() -> void {
  const auto analyzed =
      analyze_test_data_file("report_hk_impl_target_wrong_kind.kira");
  expect(analyzed.error_count > 0,
         "expected `impl functor for point` (a plain type) to be rejected");
  expect_diagnostic(analyzed, "impl target has the wrong kind",
                    "expected the wrong-kind impl-target diagnostic");
}

auto test_reports_hk_ctor_arity_mismatch() -> void {
  const auto analyzed =
      analyze_test_data_file("report_hk_ctor_arity_mismatch.kira");
  expect(analyzed.error_count > 0,
         "expected `impl functor for result` (arity 2 vs required 1) to be "
         "rejected");
  expect_diagnostic(analyzed, "type constructor of the wrong kind",
                    "expected the constructor-arity kind diagnostic");
}

auto test_reports_hk_param_wrong_arity() -> void {
  const auto analyzed =
      analyze_test_data_file("report_hk_param_wrong_arity.kira");
  expect(analyzed.error_count > 0,
         "expected `F[A, B]` where `F` was declared `F[_]` to be rejected");
  expect_diagnostic(analyzed, "expects 1 type argument, found 2",
                    "expected the parameter-application arity diagnostic");
}

auto test_reports_hk_plain_param_applied() -> void {
  const auto analyzed =
      analyze_test_data_file("report_hk_plain_param_applied.kira");
  expect(analyzed.error_count > 0,
         "expected `T[int32]` where `T` is an ordinary parameter to be "
         "rejected");
  expect_diagnostic(analyzed,
                    "kind mismatch: applying an ordinary type parameter",
                    "expected the kind-* application diagnostic");
}

auto test_reports_hk_param_used_as_type() -> void {
  const auto analyzed =
      analyze_test_data_file("report_hk_param_used_as_type.kira");
  expect(analyzed.error_count > 0,
         "expected bare `F` where `F` was declared `F[_]` to be rejected");
  expect_diagnostic(analyzed, "higher-kinded parameter used as a type",
                    "expected the bare-constructor-parameter diagnostic");
}

// ==========================================================================
//  Modules as compile-time values: signatures and functors
// ==========================================================================

auto test_accepts_wellformed_signature_and_functor() -> void {
  // A signature, a module that satisfies it, and a functor bounded by it all
  // check without well-formedness errors. (The `use` instantiation itself
  // reports "elaboration pending", so it is checked separately below.)
  const auto analyzed = analyze_sources({{
      .path = "sig_ok.kira",
      .text = "module sample\n"
              "\n"
              "signature backend:\n"
              "    type conn\n"
              "    def connect(url: str) -> conn\n"
              "\n"
              "module postgres:\n"
              "    pub type conn = int32\n"
              "    pub def connect(url: str) -> conn:\n"
              "        0\n"
              "\n"
              "module audited[DB: backend]:\n"
              "    pub def connect(url: str) -> DB.conn:\n"
              "        DB.connect(url)\n",
  }});
  expect(analyzed.error_count == 0,
         "expected a well-formed signature + satisfying module + functor "
         "declaration to check cleanly");
}

auto test_reports_module_missing_signature_member() -> void {
  const auto analyzed = analyze_sources({{
      .path = "sig_missing.kira",
      .text = "module sample\n"
              "\n"
              "signature backend:\n"
              "    type conn\n"
              "    def connect(url: str) -> conn\n"
              "    def close(c: conn) -> unit\n"
              "\n"
              "module sqlite:\n"
              "    pub type conn = int32\n"
              "    pub def connect(url: str) -> conn:\n"
              "        0\n"
              "\n"
              "use audited[sqlite] as db\n"
              "\n"
              "module audited[DB: backend]:\n"
              "    pub def go() -> unit:\n"
              "        unit\n",
  }});
  expect(analyzed.error_count > 0,
         "expected a module missing a required signature member to be "
         "rejected at instantiation");
  expect_diagnostic(analyzed, "does not satisfy signature `backend`",
                    "expected the structural-satisfaction failure header");
  expect_diagnostic(analyzed, "missing function `close`",
                    "expected the specific missing-member note");
}

auto test_reports_signature_member_not_pub() -> void {
  const auto analyzed = analyze_sources({{
      .path = "sig_priv.kira",
      .text = "module sample\n"
              "\n"
              "signature backend:\n"
              "    type conn\n"
              "\n"
              "module mysql:\n"
              "    type conn = int32\n"
              "\n"
              "use audited[mysql] as db\n"
              "\n"
              "module audited[DB: backend]:\n"
              "    pub def go() -> unit:\n"
              "        unit\n",
  }});
  expect(analyzed.error_count > 0,
         "expected a non-`pub` signature member to be rejected");
  expect_diagnostic(analyzed, "not `pub`",
                    "expected the visibility-failure note");
}

auto test_reports_signature_member_type_mismatch() -> void {
  // The module provides `connect` with the right name/arity/visibility but the
  // wrong parameter type (`int32` where the signature requires `str`). Deep
  // type-equality under the abstract-type binding must reject it.
  const auto analyzed = analyze_sources({{
      .path = "sig_type_mismatch.kira",
      .text = "module sample\n"
              "\n"
              "signature backend:\n"
              "    type conn\n"
              "    def connect(url: str) -> conn\n"
              "\n"
              "module sqlite:\n"
              "    pub type conn = int32\n"
              "    pub def connect(url: int32) -> conn:\n"
              "        0\n"
              "\n"
              "use audited[sqlite] as db\n"
              "\n"
              "module audited[DB: backend]:\n"
              "    pub def go() -> unit:\n"
              "        unit\n",
  }});
  expect(analyzed.error_count > 0,
         "expected a module whose member type mismatches the signature to be "
         "rejected");
  expect_diagnostic(analyzed, "does not satisfy signature `backend`",
                    "expected the structural-satisfaction failure header");
  expect_diagnostic(analyzed, "parameter 1 has type",
                    "expected the parameter-type-mismatch note");
}

auto test_reports_signature_return_type_mismatch() -> void {
  // Right name/arity/params but the wrong return type: signature requires
  // `-> conn`, the module returns `-> str`.
  const auto analyzed = analyze_sources({{
      .path = "sig_ret_mismatch.kira",
      .text = "module sample\n"
              "\n"
              "signature backend:\n"
              "    type conn\n"
              "    def connect(url: str) -> conn\n"
              "\n"
              "module sqlite:\n"
              "    pub type conn = int32\n"
              "    pub def connect(url: str) -> str:\n"
              "        url\n"
              "\n"
              "use audited[sqlite] as db\n"
              "\n"
              "module audited[DB: backend]:\n"
              "    pub def go() -> unit:\n"
              "        unit\n",
  }});
  expect(analyzed.error_count > 0,
         "expected a return-type mismatch against the signature to be "
         "rejected");
  expect_diagnostic(analyzed, "returns `str`, but the signature requires",
                    "expected the return-type-mismatch note");
}

auto test_reports_unknown_functor_instantiation() -> void {
  const auto analyzed = analyze_sources({{
      .path = "sig_unknown.kira",
      .text = "module sample\n"
              "\n"
              "use nonexistent[postgres] as db\n",
  }});
  expect(analyzed.error_count > 0,
         "expected instantiating an unknown functor to be rejected");
  expect_diagnostic(analyzed, "no parameterized module named `nonexistent`",
                    "expected the unknown-functor diagnostic");
}

auto test_materializes_functor_and_resolves_alias() -> void {
  // `use audited[postgres] as db` materializes the instantiated module; a
  // call through the alias resolves and type-checks, and the functor body's
  // `DB.conn` projection resolves to postgres's concrete `conn`.
  const auto analyzed = analyze_sources({{
      .path = "mat_ok.kira",
      .text = "module main\n"
              "\n"
              "signature backend:\n"
              "    type conn\n"
              "    def connect(url: str) -> conn\n"
              "\n"
              "module postgres:\n"
              "    pub type conn = int32\n"
              "    pub def connect(url: str) -> conn:\n"
              "        0\n"
              "\n"
              "module audited[DB: backend]:\n"
              "    pub def connect(url: str) -> DB.conn:\n"
              "        DB.connect(url)\n"
              "\n"
              "use main.audited[main.postgres] as db\n"
              "\n"
              "def go() -> int32:\n"
              "    db.connect(\"x\")\n",
  }});
  expect(analyzed.error_count == 0,
         "expected a materialized functor instantiation and a call through "
         "its alias to check cleanly");
}

auto test_functor_body_impl_and_extend_members_check() -> void {
  // A functor body declares a local `type`, an `impl` of a trait for it, and
  // an `extend` block adding an inherent method — plus a `def` that calls both
  // through a value of the local type. All must materialize, register into the
  // method table/coherence, and check cleanly per instantiation.
  const auto analyzed = analyze_sources({{
      .path = "mat_impl.kira",
      .text = "module main\n"
              "\n"
              "signature backend:\n"
              "    type conn\n"
              "    def connect(url: str) -> conn\n"
              "\n"
              "module postgres:\n"
              "    pub type conn = int32\n"
              "    pub def connect(url: str) -> conn:\n"
              "        0\n"
              "\n"
              "trait greet:\n"
              "    def hello(self) -> int32\n"
              "\n"
              "module wrap[DB: backend]:\n"
              "    pub type widget = { value: int32 }\n"
              "    impl greet for widget:\n"
              "        def hello(self) -> int32:\n"
              "            self.value\n"
              "    extend widget:\n"
              "        def doubled(self) -> int32:\n"
              "            self.value + self.value\n"
              "    pub def run() -> int32:\n"
              "        let b = widget { value: 21 }\n"
              "        b.hello() + b.doubled()\n"
              "\n"
              "use main.wrap[main.postgres] as w\n"
              "\n"
              "def go() -> int32:\n"
              "    w.run()\n",
  }});
  expect(analyzed.error_count == 0,
         "expected a functor body with `impl`/`extend` on a local type to "
         "materialize and check cleanly");
}

auto test_functor_body_impl_coherence_across_instantiations() -> void {
  // Two instantiations of a functor that implements a trait for its own local
  // type must NOT collide in coherence: each instantiation clones the local
  // type into a distinct synthetic module, so the (trait, type) coherence key
  // differs. This would be a false "duplicate implementation" if the impls
  // shared a coherence key.
  const auto analyzed = analyze_sources({{
      .path = "mat_coherence.kira",
      .text = "module main\n"
              "\n"
              "signature backend:\n"
              "    type conn\n"
              "    def connect(url: str) -> conn\n"
              "\n"
              "module postgres:\n"
              "    pub type conn = int32\n"
              "    pub def connect(url: str) -> conn:\n"
              "        0\n"
              "\n"
              "module sqlite:\n"
              "    pub type conn = int32\n"
              "    pub def connect(url: str) -> conn:\n"
              "        1\n"
              "\n"
              "trait greet:\n"
              "    def hello(self) -> int32\n"
              "\n"
              "module wrap[DB: backend]:\n"
              "    pub type widget = { value: int32 }\n"
              "    impl greet for widget:\n"
              "        def hello(self) -> int32:\n"
              "            self.value\n"
              "\n"
              "use main.wrap[main.postgres] as wp\n"
              "use main.wrap[main.sqlite] as ws\n",
  }});
  expect(analyzed.error_count == 0,
         "expected two functor instantiations each implementing a trait for "
         "their own local type to be coherent (no false duplicate)");
}

auto test_impl_type_param_substituted_at_call_site() -> void {
  // G1: an `impl` block's own type parameter is in scope over its methods'
  // signatures without appearing on the method's `type_params`. Resolving
  // `def get(self) -> T` against the method's parameters alone left `T`
  // unresolvable, and the call's type came back as "unknown" — which unifies
  // with everything, so binding the result to a `str` was accepted in
  // silence. The bug here is a *missing* diagnostic, so what is asserted is
  // that the mismatch is now reported.
  const auto analyzed = analyze_sources({{
      .path = "impl_type_param.kira",
      .text = "module main\n"
              "\n"
              "trait get_it[T]:\n"
              "    def get(self) -> T\n"
              "\n"
              "type holder[T] = { value: T }\n"
              "\n"
              "impl[T] get_it[T] for holder[T]:\n"
              "    def get(self) -> T:\n"
              "        return self.value\n"
              "\n"
              "def main() -> int32:\n"
              "    let h = holder { value: 7 }\n"
              "    let s: str = h.get()\n"
              "    return 0\n",
  }});
  expect(analyzed.error_count > 0,
         "expected `let s: str = h.get()` on a `holder[int32]` to be rejected "
         "— the impl's `T` must be solved to `int32` from the receiver");
  expect(analyzed.diagnostics.find("expected `str`, found `int32`") !=
             std::string::npos,
         std::format("expected a concrete `str`/`int32` mismatch, got: {}",
                     analyzed.diagnostics));
}

auto test_impls_on_distinct_instantiations_are_coherent() -> void {
  // Coherence is keyed at the *declaration* level, because `type_has_trait`
  // has to find `impl[T] show for holder[T]` when asked about
  // `holder[int32]`. That key alone, used as the conflict test, rejected two
  // impls for two genuinely different concrete types — which the reference
  // explicitly permits ("no two can apply to the same concrete type").
  const auto analyzed = analyze_sources({{
      .path = "instantiation_coherence.kira",
      .text = "module main\n"
              "\n"
              "trait get_it[T]:\n"
              "    def get(self) -> T\n"
              "\n"
              "type boxed[T] = { item: T }\n"
              "\n"
              "impl get_it[int32] for boxed[int32]:\n"
              "    def get(self) -> int32:\n"
              "        return self.item\n"
              "\n"
              "impl get_it[int64] for boxed[int64]:\n"
              "    def get(self) -> int64:\n"
              "        return self.item\n",
  }});
  expect(analyzed.error_count == 0,
         std::format("expected impls for `boxed[int32]` and `boxed[int64]` to "
                     "coexist — they are different concrete types, got: {}",
                     analyzed.diagnostics));
}

auto test_overlapping_generic_impl_still_conflicts() -> void {
  // The other direction, and the reason the conflict test stayed
  // conservative: a blanket impl and a concrete one for the same declaration
  // *do* both apply to `boxed[int32]`, so relaxing the key must not let this
  // through. Without this case, `impl_targets_overlap` could be weakened to
  // plain type-id equality and the suite would stay green.
  const auto analyzed = analyze_sources({{
      .path = "overlapping_impl.kira",
      .text = "module main\n"
              "\n"
              "trait get_it[T]:\n"
              "    def get(self) -> T\n"
              "\n"
              "type boxed[T] = { item: T }\n"
              "\n"
              "impl[T] get_it[T] for boxed[T]:\n"
              "    def get(self) -> T:\n"
              "        return self.item\n"
              "\n"
              "impl get_it[int32] for boxed[int32]:\n"
              "    def get(self) -> int32:\n"
              "        return self.item\n",
  }});
  expect(analyzed.error_count > 0,
         "expected a blanket `impl[T] ... for boxed[T]` to conflict with a "
         "concrete `impl ... for boxed[int32]` — both apply to `boxed[int32]`");
  expect(analyzed.diagnostics.find("duplicate implementation") !=
             std::string::npos,
         std::format("expected a duplicate-implementation diagnostic, got: {}",
                     analyzed.diagnostics));
}

auto test_functor_type_projection_resolves_concretely() -> void {
  // A `DB.conn` projection must resolve to the argument module's *concrete*
  // type (`postgres.conn` == int32), not silently to `k_unknown` — otherwise
  // a type error inside the functor body would go unreported (and lowering
  // would later choke on the un-typed nodes). Returning a `str` where the
  // projection demands an `int32` must be diagnosed.
  const auto analyzed = analyze_sources({{
      .path = "mat_proj.kira",
      .text = "module main\n"
              "\n"
              "signature backend:\n"
              "    type conn\n"
              "    def connect(url: str) -> conn\n"
              "\n"
              "module postgres:\n"
              "    pub type conn = int32\n"
              "    pub def connect(url: str) -> conn:\n"
              "        0\n"
              "\n"
              "module audited[DB: backend]:\n"
              "    pub def bad() -> DB.conn:\n"
              "        \"not an int\"\n"
              "\n"
              "use main.audited[main.postgres] as db\n",
  }});
  expect(analyzed.error_count > 0,
         "expected a `str` body under a `DB.conn` (int32) return to be "
         "rejected, proving the projection resolves concretely");
}

auto test_functor_body_type_and_static_members_check() -> void {
  // A functor body may declare `type` and `static` members alongside `def`s;
  // `type row = DB.conn` projects through the parameter and a `static` is a
  // required constant. All are cloned and checked per instantiation.
  const auto analyzed = analyze_sources({{
      .path = "mat_members.kira",
      .text = "module main\n"
              "\n"
              "signature backend:\n"
              "    type conn\n"
              "    def connect(url: str) -> conn\n"
              "\n"
              "module postgres:\n"
              "    pub type conn = int32\n"
              "    pub def connect(url: str) -> conn:\n"
              "        0\n"
              "\n"
              "module audited[DB: backend]:\n"
              "    type row = DB.conn\n"
              "    static bump: int32 = 5\n"
              "    pub def open(url: str) -> int32:\n"
              "        let c: row = DB.connect(url)\n"
              "        c + bump\n"
              "\n"
              "use main.audited[main.postgres] as db\n"
              "\n"
              "def go() -> int32:\n"
              "    db.open(\"x\")\n",
  }});
  expect(analyzed.error_count == 0,
         "expected a functor with `type`/`static` members to check cleanly");
}

auto test_functor_instantiation_arity_mismatch() -> void {
  const auto analyzed = analyze_sources({{
      .path = "mat_arity.kira",
      .text = "module main\n"
              "\n"
              "signature backend:\n"
              "    type conn\n"
              "\n"
              "module postgres:\n"
              "    pub type conn = int32\n"
              "\n"
              "module audited[DB: backend]:\n"
              "    pub def go() -> unit:\n"
              "        unit\n"
              "\n"
              "use main.audited[main.postgres, main.postgres] as db\n",
  }});
  expect(analyzed.error_count > 0,
         "expected a wrong-arity functor instantiation to be rejected");
  expect_diagnostic(analyzed, "takes 1 argument(s), but 2 were supplied",
                    "expected the arity-mismatch diagnostic");
}

/// The parenthesis-free `for k, v in pairs` head used to bind every name to
/// `k_unknown_type`, which unifies with everything — so a body using a loop
/// variable at the wrong type was accepted in silence, and the loop was then
/// lowered against a type nobody had checked. Asserting a *rejection* here is
/// the point: the previous behavior was "compiles cleanly", not "wrong
/// diagnostic".
auto test_bare_for_names_bind_tuple_components() -> void {
  const auto analyzed = analyze_sources({{
      .path = "for_bare.kira",
      .text = "module main\n"
              "\n"
              "def main() -> int32:\n"
              "    let pairs = [(1, 10)]\n"
              "    for k, v in pairs:\n"
              "        let s: str = v\n"
              "    return 0\n",
  }});
  expect(analyzed.error_count > 0,
         "expected `v` to be bound as int32, not as an unknown type");
  expect_diagnostic(analyzed, "expected `str`, found `int32`",
                    "expected the loop variable to carry its component type");
}

auto test_for_head_arity_must_match_element() -> void {
  const auto analyzed = analyze_sources({{
      .path = "for_arity.kira",
      .text = "module main\n"
              "\n"
              "def main() -> int32:\n"
              "    let pairs = [(1, 10)]\n"
              "    for k, v, w in pairs:\n"
              "        let x = k\n"
              "    return 0\n",
  }});
  expect(analyzed.error_count > 0,
         "expected three loop variables over a 2-tuple to be rejected");
  expect_diagnostic(analyzed, "but each element of",
                    "expected the loop-head arity diagnostic");
}

auto test_for_head_cannot_split_a_non_tuple() -> void {
  const auto analyzed = analyze_sources({{
      .path = "for_non_tuple.kira",
      .text = "module main\n"
              "\n"
              "def main() -> int32:\n"
              "    let nums = [1, 2, 3]\n"
              "    for a, b in nums:\n"
              "        let x = a\n"
              "    return 0\n",
  }});
  expect(analyzed.error_count > 0,
         "expected splitting a non-tuple element to be rejected");
  expect_diagnostic(analyzed, "is not a tuple",
                    "expected the non-tuple loop-head diagnostic");
}

/// UFCS: a free function is callable with method syntax, and the receiver
/// really does fill the *first* parameter — `label` takes a second argument
/// of a different type, so routing the receiver into the wrong slot would
/// be a type error rather than a silent success.
///
/// The receiver is a struct, not an `int32`, for the reason spelled out on
/// `test_ufcs_skips_private_functions_in_other_modules`: on a builtin
/// receiver an unresolved method is silently `unknown`, so this assertion
/// would hold there even with UFCS removed outright.
auto test_free_function_callable_as_method() -> void {
  const auto analyzed = analyze_sources({{
      .path = "ufcs_basic.kira",
      .text = "module main\n"
              "\n"
              "type tag = { id: int32 }\n"
              "\n"
              "pub def label(t: tag, prefix: str) -> str:\n"
              "    return prefix\n"
              "\n"
              "def main() -> int32:\n"
              "    let t = tag{id: 1}\n"
              "    let s: str = t.label(\"n=\")\n"
              "    return 0\n",
  }});
  expect(analyzed.error_count == 0,
         "expected a free function to be callable as a method");
}

/// The safety property the whole feature rests on: UFCS is the last arm, so
/// a free function of the same name can never take a call away from a real
/// method. Asserting the *return type* is what makes this able to fail — if
/// the free `tag` won, `s` would be `str` and the `int32` annotation would
/// be the error this expects not to see.
auto test_method_wins_over_ufcs() -> void {
  const auto analyzed = analyze_sources({{
      .path = "ufcs_shadow.kira",
      .text = "module main\n"
              "\n"
              "type counter = { value: int32 }\n"
              "\n"
              "extend counter:\n"
              "    def tag(self) -> int32:\n"
              "        return self.value\n"
              "\n"
              "pub def tag(c: counter) -> str:\n"
              "    return \"free\"\n"
              "\n"
              "def main() -> int32:\n"
              "    let c = counter{value: 1}\n"
              "    let n: int32 = c.tag()\n"
              "    return n\n",
  }});
  expect(analyzed.error_count == 0,
         "expected the inherent method to win over the UFCS candidate");
}

/// Two visible free functions both accepting the receiver is the user's to
/// resolve — there is deliberately no most-specific tie-break.
auto test_ufcs_ambiguity_is_an_error() -> void {
  const auto analyzed = analyze_sources({
      {
          .path = "amb_a.kira",
          .text = "module amb.a\n"
                  "\n"
                  "pub def scale(x: int32) -> int32:\n"
                  "    return x * 2\n",
      },
      {
          .path = "amb_b.kira",
          .text = "module amb.b\n"
                  "\n"
                  "pub def scale(x: int32) -> int32:\n"
                  "    return x * 3\n",
      },
      {
          .path = "amb_main.kira",
          .text = "module main\n"
                  "\n"
                  "use amb.a.*\n"
                  "use amb.b.*\n"
                  "\n"
                  "def main() -> int32:\n"
                  "    return 5.scale()\n",
      },
  });
  expect(analyzed.error_count > 0,
         "expected an ambiguous UFCS call to be rejected");
  expect_diagnostic(analyzed, "is ambiguous here",
                    "expected the UFCS ambiguity diagnostic");
}

/// A `&mut` first parameter against a non-`mut` binding is a hard error,
/// never a silently skipped candidate — otherwise `v.iter_mut()` on a `let`
/// collapses into a baffling "no method `iter_mut`", which is the opposite
/// of what the user needs to be told.
auto test_ufcs_mut_receiver_must_be_mutable() -> void {
  const auto analyzed = analyze_sources({{
      .path = "ufcs_mut.kira",
      .text = "module main\n"
              "\n"
              "type acc = { total: int32 }\n"
              "\n"
              "pub def bump(a: &mut acc) -> int32:\n"
              "    return a.total\n"
              "\n"
              "def main() -> int32:\n"
              "    let box = acc{total: 1}\n"
              "    return box.bump()\n",
  }});
  expect(analyzed.error_count > 0,
         "expected a `&mut` UFCS receiver on a `let` binding to be rejected");
  expect_diagnostic(analyzed, "needs to mutate its receiver",
                    "expected the immutable-receiver diagnostic");
}

/// The name exists but nothing accepts this receiver — the message a
/// forgotten `.iter()` produces, and the one worth getting right.
/// An unknown method on a *builtin* receiver is an error, and suggests the
/// method that was probably meant.
///
/// This used to be silent. A user-defined receiver has always reported "no
/// method `x` on type `y`", but a builtin one fell through every lookup and
/// returned `k_unknown_type` with no diagnostic — and because
/// `k_unknown_type` deliberately unifies with everything so one gap does not
/// cascade, the bad call then propagated quietly through whatever consumed
/// it. Two committed fixtures were calling a `str` method that did not exist
/// anywhere, and checked "cleanly".
auto test_unknown_method_on_builtin_receiver_is_reported() -> void {
  const auto analyzed = analyze_sources({{
      .path = "builtin_unknown_method.kira",
      .text = "module main\n"
              "\n"
              "def main() -> int32:\n"
              "    let nums = [1, 2, 3]\n"
              "    let picked = nums.fliter()\n"
              "    return 0\n",
  }});
  expect(analyzed.error_count > 0,
         "expected an unknown method on a `list` receiver to be rejected");
  expect_diagnostic(analyzed, "no method `fliter` on type `list[int32]`",
                    "expected the unknown-method error to name the builtin "
                    "receiver's full type");
  expect_diagnostic(analyzed, "did you mean `filter`?",
                    "expected a suggestion drawn from the builtin method set");
}

/// The suggestion pool includes `extend` methods on the builtin, not just the
/// compiler's own inherent ones — otherwise a near-miss on a standard-library
/// method would report "no method" while listing an unrelated set.
auto test_builtin_method_suggestion_includes_extend_methods() -> void {
  const auto analyzed = analyze_sources({{
      .path = "builtin_extend_suggestion.kira",
      .text = "module main\n"
              "\n"
              "extend str:\n"
              "    def shout(self) -> str:\n"
              "        return self\n"
              "\n"
              "def main() -> int32:\n"
              "    let noise = \"hi\".shout()\n"
              "    let bad = \"hi\".shou()\n"
              "    return 0\n",
  }});
  expect(analyzed.error_count > 0,
         "expected an unknown method on a `str` receiver to be rejected");
  expect_diagnostic(analyzed, "did you mean `shout`?",
                    "expected an `extend` method to be offered as a "
                    "suggestion for a near-miss on a builtin receiver");
}

/// ...but a receiver whose type is still a type *parameter* must stay silent,
/// and be reported once against the concrete type instead.
///
/// A generic body is checked as a template and again per instantiation. Only
/// the instantiation knows what `T` actually is and therefore what methods it
/// has, so reporting from the template too would produce two errors for one
/// mistake — the second of them against `T`, a type whose method set is not
/// knowable there. The restriction to builtin receiver kinds is what prevents
/// that; this pins the single-error outcome it buys.
auto test_unknown_method_on_generic_receiver_reports_once() -> void {
  const auto analyzed = analyze_sources({{
      .path = "generic_receiver.kira",
      .text = "module main\n"
              "\n"
              "def call_it[T](x: T) -> int32:\n"
              "    let r = x.whatever()\n"
              "    return 0\n"
              "\n"
              "def main() -> int32:\n"
              "    return call_it(5)\n",
  }});
  expect(analyzed.error_count == 1,
         "an unknown method in a generic body should be reported once, from "
         "the instantiation that knows the concrete type");
  expect_diagnostic(analyzed, "no method `whatever` on type `int32`",
                    "expected the error to name the instantiated type");
  expect(analyzed.diagnostics.find("on type `T`") == std::string::npos,
         "a type parameter's method set is not knowable in the template, so "
         "no error may be reported against `T` itself");
}

auto test_ufcs_reports_receiver_mismatch() -> void {
  const auto analyzed = analyze_sources({{
      .path = "ufcs_mismatch.kira",
      .text = "module main\n"
              "\n"
              "pub def total_of(items: list[int32]) -> int32:\n"
              "    return 0\n"
              "\n"
              "def main() -> int32:\n"
              "    return 5.total_of()\n",
  }});
  expect(analyzed.error_count > 0,
         "expected a UFCS call whose receiver does not fit to be rejected");
  expect_diagnostic(analyzed, "cannot be called on",
                    "expected the receiver-mismatch diagnostic");
  expect_diagnostic(analyzed, "wants `list[int32]` first",
                    "expected the mismatch note to name the parameter type");
}

/// A non-`pub` function in another module is not UFCS-eligible, so it never
/// enters the candidate pool — the call falls through to the ordinary
/// not-found error rather than reaching across the module boundary.
///
/// The receiver is a struct deliberately. On a *builtin* receiver an
/// unrecognized method name resolves to `unknown` with no diagnostic at all
/// (builtin method lookup is best-effort), so the same assertion made there
/// would pass whether or not eligibility were enforced — it has to be made
/// somewhere a missing method is actually reported.
auto test_ufcs_skips_private_functions_in_other_modules() -> void {
  const auto analyzed = analyze_sources({
      {
          .path = "priv_lib.kira",
          .text = "module hidden.lib\n"
                  "\n"
                  "pub type token = { id: int32 }\n"
                  "\n"
                  "def unwrap(t: token) -> int32:\n"
                  "    return t.id\n",
      },
      {
          .path = "priv_main.kira",
          .text = "module main\n"
                  "\n"
                  "use hidden.lib.*\n"
                  "\n"
                  "def main() -> int32:\n"
                  "    let t = token{id: 1}\n"
                  "    return t.unwrap()\n",
      },
  });
  expect(analyzed.error_count > 0,
         "expected a private function in another module to stay out of the "
         "UFCS candidate pool");
  expect_diagnostic(analyzed, "no method `unwrap`",
                    "expected the ordinary not-found diagnostic");
}

} // namespace

auto main() -> int {
  try {
    test_accepts_typed_core_program();
    test_accepts_structs_and_methods();
    test_accepts_packed_struct();
    test_accepts_collections_and_lambdas();
    test_accepts_option_result_flow();
    test_accepts_cross_module_qualified_types();
    test_accepts_module_qualified_call();
    test_accepts_member_import_call();
    test_accepts_wildcard_import();
    test_accepts_fully_qualified_call();
    test_accepts_type_qualified_associated_call();
    test_accepts_indexing_local_bindings();
    test_accepts_str_byte_index();
    test_accepts_for_over_user_iterator();
    test_accepts_type_generic_free_function();
    test_reports_type_generic_unsolved();
    test_reports_str_index_non_integer();
    test_accepts_associated_type_self_output();
    test_accepts_extend_on_builtin_type();
    test_accepts_extend_on_user_type();
    test_impl_method_takes_priority_over_extend();
    test_accepts_intrinsic_decl();
    test_accepts_string_interpolation();
    test_accepts_static_let_evaluates();
    test_accepts_static_if_selects_taken_branch();
    test_accepts_static_if_selects_branch_by_sum_type_equality();
    test_accepts_static_if_variant_equality_selects_else_branch();
    test_accepts_static_assert_compares_variant_payloads();
    test_accepts_quote_expr_typed_by_fragment_kind();
    test_accepts_static_def_call_evaluates();
    test_accepts_static_struct_value_evaluates();
    test_accepts_static_let_forward_reference();
    test_accepts_splice_expr_reifies_quoted_value();
    test_accepts_splice_stmt_reifies_quoted_statement();
    test_accepts_splice_type_reifies_quoted_type();
    test_accepts_expr_keyword_usable_as_identifier();
    test_accepts_expr_builder_constructs_fragment();
    test_accepts_item_splice_injects_impl();
    test_accepts_type_reflection_primitives();
    test_accepts_module_reflection();
    test_accepts_generator_yields_typed_values();
    test_accepts_for_loop_over_generator();
    test_accepts_existential_return_type();
    test_accepts_existential_return_type_combined_bounds();

    test_reports_undefined_name_with_suggestion();
    test_reports_undefined_type();
    test_reports_packed_on_sum_type();
    test_reports_annotation_mismatch();
    test_reports_quote_type_annotation_mismatch();
    test_reports_static_assert_evaluates_false();
    test_reports_static_for_evaluates_each_iteration();
    test_reports_splice_expr_wrong_fragment_kind();
    test_reports_hygiene_prevents_spliced_binding_leak();
    test_reports_splice_requires_quote_value();
    test_reports_integer_literal_overflow();
    test_reports_mixed_numeric_types();
    test_reports_non_bool_condition();
    test_reports_assignment_to_immutable();
    test_accepts_let_mut_reassignment();
    test_reports_return_type_mismatch();
    test_reports_call_argument_problems();
    test_reports_struct_literal_problems();
    test_reports_unknown_field_and_method();
    test_reports_non_exhaustive_match();
    test_reports_unknown_variant_in_pattern();
    test_reports_try_in_plain_function();
    test_reports_unknown_intrinsic();
    test_reports_unannotated_intrinsic();
    test_reports_yield_outside_generator();
    test_reports_generator_missing_return_type();
    test_reports_generator_yield_type_mismatch();
    test_reports_generator_return_with_value();
    test_reports_generator_postcondition();
    test_reports_existential_type_outside_generator();
    test_reports_existential_return_type_in_parameter();
    test_reports_existential_bound_not_satisfied();
    test_reports_existential_return_type_mismatch();
    test_reports_existential_method_not_in_bound();
    test_accepts_higher_kinded_functor_monad();
    test_reports_hk_impl_target_wrong_kind();
    test_reports_hk_ctor_arity_mismatch();
    test_reports_hk_param_wrong_arity();
    test_reports_hk_plain_param_applied();
    test_reports_hk_param_used_as_type();
    test_reports_unannotated_pub_function();
    test_reports_duplicate_trait_impl();
    test_reports_incomplete_trait_impl();
    test_accepts_drop_impl();
    test_reports_duplicate_drop_impl();
    test_reports_incomplete_drop_impl();
    test_reports_unsatisfied_trait_requirement();
    test_reports_unknown_deriving();
    test_reports_impure_contract_call();
    test_reports_string_interpolation_unsupported_style();
    test_reports_string_interpolation_precision_on_integer_style();
    test_reports_string_interpolation_bad_dynamic_width();
    test_reports_associated_type_output_mismatch();
    test_reports_extend_method_arity_mismatch();
    test_reports_missing_return_value();
    test_reports_missing_return_on_if_without_else();
    test_accepts_all_paths_returning();
    test_reports_const_generic_value_mismatch();
    test_accepts_const_generic_value_match();
    test_accepts_const_generic_try_from();
    test_reports_unsolved_const_generic_value_param();
    test_accepts_explicit_generic_args();
    test_accepts_generic_methods();
    test_reports_bad_explicit_generic_args();
    test_accepts_return_position_inference();
    test_reports_unsolved_return_position_type_param();
    test_accepts_method_explicit_generic_args();
    test_reports_bad_method_explicit_generic_args();
    test_accepts_type_param_static_dispatch();
    test_reports_type_param_static_not_found();
    test_reports_refinement_predicate_not_bool();
    test_accepts_refinement_predicate();
    test_accepts_concept_bound();
    test_reports_state_machine_mismatch();
    test_accepts_state_machine_match();

    test_accepts_dependent_length_arithmetic();
    test_reports_dependent_length_refuted();
    test_accepts_proved_refinements();
    test_reports_unproven_refinement();
    test_reports_refuted_refinement();
    test_accepts_flow_narrowed_refinement();
    test_reports_refinement_outside_narrowed_path();
    test_accepts_proved_contracts();
    test_reports_return_outside_a_postcondition();
    test_reports_refuted_precondition();

    test_check_program_persists_expression_types();

    test_bare_literal_never_forces_a_concrete_param_type();
    test_infers_param_type_from_annotated_sibling();
    test_infers_param_type_from_call_to_annotated_function();
    test_infers_param_type_from_struct_field_type();
    test_infers_param_type_from_struct_field_shorthand();
    test_infers_param_type_from_annotated_let_binding();
    test_infers_param_type_from_annotated_var_binding();
    test_infers_param_type_from_declared_return_type();
    test_infers_param_type_from_declared_return_type_on_expr_body();
    test_method_call_receiver_subexpr_still_inferred();
    test_pass_through_param_stays_unannotated();
    test_recursive_function_param_inference_terminates();

    test_accepts_wellformed_signature_and_functor();
    test_reports_module_missing_signature_member();
    test_reports_signature_member_not_pub();
    test_reports_signature_member_type_mismatch();
    test_reports_signature_return_type_mismatch();
    test_reports_unknown_functor_instantiation();
    test_materializes_functor_and_resolves_alias();
    test_functor_type_projection_resolves_concretely();
    test_functor_body_type_and_static_members_check();
    test_functor_body_impl_and_extend_members_check();
    test_functor_body_impl_coherence_across_instantiations();
    test_functor_instantiation_arity_mismatch();
    test_bare_for_names_bind_tuple_components();
    test_for_head_arity_must_match_element();
    test_for_head_cannot_split_a_non_tuple();
    test_free_function_callable_as_method();
    test_method_wins_over_ufcs();
    test_ufcs_ambiguity_is_an_error();
    test_ufcs_mut_receiver_must_be_mutable();
    test_unknown_method_on_builtin_receiver_is_reported();
    test_builtin_method_suggestion_includes_extend_methods();
    test_unknown_method_on_generic_receiver_reports_once();
    test_ufcs_reports_receiver_mismatch();
    test_ufcs_skips_private_functions_in_other_modules();
    test_impl_type_param_substituted_at_call_site();
    test_impls_on_distinct_instantiations_are_coherent();
    test_overlapping_generic_impl_still_conflicts();
  } catch (const std::exception &ex) {
    std::cerr << "check_test failed: unhandled exception: " << ex.what()
              << '\n';
    std::exit(1);
  }
  return 0;
}
