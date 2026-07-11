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
/// `std/prelude.kira`, `std/io.kira`, `std/console.kira`, `std/fmt.kira`) —
/// mirrors
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
  const auto analyzed = analyze_test_data_file(
      "report_static_for_evaluates_each_iteration.kira");
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
    test_accepts_fully_qualified_call();
    test_accepts_type_qualified_associated_call();
    test_accepts_indexing_local_bindings();
    test_accepts_associated_type_self_output();
    test_accepts_extend_on_builtin_type();
    test_accepts_extend_on_user_type();
    test_impl_method_takes_priority_over_extend();
    test_accepts_intrinsic_decl();
    test_accepts_string_interpolation();
    test_accepts_static_let_evaluates();
    test_accepts_static_if_selects_taken_branch();
    test_accepts_static_def_call_evaluates();
    test_accepts_static_struct_value_evaluates();
    test_accepts_static_let_forward_reference();

    test_reports_undefined_name_with_suggestion();
    test_reports_undefined_type();
    test_reports_packed_on_sum_type();
    test_reports_annotation_mismatch();
    test_reports_quote_type_annotation_mismatch();
    test_reports_static_assert_evaluates_false();
    test_reports_static_for_evaluates_each_iteration();
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
    test_reports_const_generic_value_mismatch();
    test_accepts_const_generic_value_match();
    test_reports_refinement_predicate_not_bool();
    test_accepts_refinement_predicate();
    test_accepts_concept_bound();
    test_reports_state_machine_mismatch();
    test_accepts_state_machine_match();

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
  } catch (const std::exception &ex) {
    std::cerr << "check_test failed: unhandled exception: " << ex.what()
              << '\n';
    std::exit(1);
  }
  return 0;
}
