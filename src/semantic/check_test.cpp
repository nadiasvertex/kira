#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analysis.h"
#include "src/k-parser/parser.h"

namespace {

struct source_fixture {
  std::string path;
  std::string text;
};

struct analyzed_session {
  std::string diagnostics;
  uint32_t error_count = 0;
};

[[noreturn]] auto fail(std::string_view message) -> void {
  std::cerr << "check_test failed: " << message << '\n';
  std::exit(1);
}

auto expect(bool condition, std::string_view message) -> void {
  if (!condition) {
    fail(message);
  }
}

auto expect_diagnostic(const analyzed_session &analyzed,
                       std::string_view needle, // NOLINT(bugprone-easily-swappable-parameters)
                       std::string_view message)
    -> void {
  if (analyzed.diagnostics.find(needle) == std::string::npos) {
    std::cerr << "check_test: missing diagnostic `" << needle << "`\n"
              << "rendered diagnostics were:\n"
              << analyzed.diagnostics << '\n';
    fail(message);
  }
}

auto analyze_sources(const std::vector<source_fixture> &fixtures)
    -> analyzed_session {
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
  kira::semantic::validate_semantics(parsed_modules, diag, file_has_errors);

  return analyzed_session{
      .diagnostics = kira::diagnostic_renderer(sources, false).render_all(diag),
      .error_count = diag.error_count(),
  };
}

auto analyze_one(const std::string& text) -> analyzed_session {
  return analyze_sources({{.path = "sample.kira", .text = text}});
}

// ==========================================================================
//  Programs that must check cleanly
// ==========================================================================

auto test_accepts_typed_core_program() -> void {
  const auto analyzed = analyze_one(
      "module sample\n"
      "type shape = @circle(float64) | @rect(float64, float64) | @point\n"
      "def area(s: shape) -> float64:\n"
      "    match s:\n"
      "        @circle(r) if r <= 0.0 => 0.0\n"
      "        @circle(r) => 3.14159 * r * r\n"
      "        @rect(w, h) => w * h\n"
      "        @point => 0.0\n"
      "def describe(score: int32) -> str:\n"
      "    if score > 90:\n"
      "        return \"excellent\"\n"
      "    elif score > 70:\n"
      "        return \"good\"\n"
      "    else:\n"
      "        return \"needs work\"\n"
      "def main() -> unit:\n"
      "    let s = @circle(2.0)\n"
      "    let a = area(s)\n"
      "    println(\"area {a}\")\n");
  expect(analyzed.error_count == 0,
         "expected typed core program to check cleanly");
}

auto test_accepts_structs_and_methods() -> void {
  const auto analyzed = analyze_one(
      "module sample\n"
      "type point = { x: float64, y: float64 }\n"
      "trait show:\n"
      "    def show(self) -> str\n"
      "impl show for point:\n"
      "    def show(self) -> str:\n"
      "        return \"({self.x}, {self.y})\"\n"
      "def run() -> str:\n"
      "    let p = point { x: 1.0, y: 2.0 }\n"
      "    let d = p.x * p.x + p.y * p.y\n"
      "    return p.show()\n");
  expect(analyzed.error_count == 0,
         "expected struct/impl program to check cleanly");
}

auto test_accepts_collections_and_lambdas() -> void {
  const auto analyzed = analyze_one(
      "module sample\n"
      "def run() -> unit:\n"
      "    let numbers = [1, 2, 3, 4, 5]\n"
      "    let doubled = numbers.filter(x => x > 2)\n"
      "    var total = 0\n"
      "    for n in numbers:\n"
      "        total += n\n"
      "    let squares = for x in 0..5 => x * x\n"
      "    while total > 0:\n"
      "        total -= 1\n"
      "    println(\"{total}\")\n");
  expect(analyzed.error_count == 0,
         "expected collection/lambda program to check cleanly");
}

auto test_accepts_option_result_flow() -> void {
  const auto analyzed = analyze_one(
      "module sample\n"
      "type app_error = @not_found | @bad_input(str)\n"
      "def find(id: int32) -> option[int32]:\n"
      "    if id > 0:\n"
      "        return @some(id)\n"
      "    return @none\n"
      "def load(id: int32) -> result[int32, app_error]:\n"
      "    match find(id):\n"
      "        @some(v) => @ok(v)\n"
      "        @none => @err(@not_found)\n"
      "def total(id: int32) -> result[int32, app_error]:\n"
      "    let value = load(id)?\n"
      "    return @ok(value + 1)\n");
  expect(analyzed.error_count == 0,
         "expected option/result program to check cleanly");
}

auto test_accepts_cross_module_qualified_types() -> void {
  const auto analyzed = analyze_sources({
      {
          .path = "app.kira",
          .text = "module app\n"
                  "module tools\n"
                  "pub def flip(p: app.tools.point) -> app.tools.point:\n"
                  "    let x = p.x\n"
                  "    return p\n",
      },
      {
          .path = "app_tools.kira",
          .text = "module app.tools\n"
                  "pub type point = { pub x: float64, pub y: float64 }\n",
      },
  });
  expect(analyzed.error_count == 0,
         "expected qualified types from other session modules to resolve");
}

// ==========================================================================
//  Programs that must be rejected, with teaching diagnostics
// ==========================================================================

auto test_reports_undefined_name_with_suggestion() -> void {
  const auto analyzed = analyze_one("module sample\n"
                                    "def run() -> int32:\n"
                                    "    let total = 1\n"
                                    "    return totl\n");
  expect(analyzed.error_count > 0, "expected undefined name to fail");
  expect_diagnostic(analyzed, "undefined name `totl`",
                    "expected undefined-name diagnostic");
  expect_diagnostic(analyzed, "did you mean `total`?",
                    "expected a suggestion for the near-miss name");
}

auto test_reports_undefined_type() -> void {
  const auto analyzed = analyze_one("module sample\n"
                                    "def run(x: pont) -> unit:\n"
                                    "    println(\"{x}\")\n");
  expect(analyzed.error_count > 0, "expected undefined type to fail");
  expect_diagnostic(analyzed, "undefined type `pont`",
                    "expected undefined-type diagnostic");
}

auto test_reports_annotation_mismatch() -> void {
  const auto analyzed = analyze_one("module sample\n"
                                    "def run() -> unit:\n"
                                    "    let x: int32 = \"hello\"\n");
  expect(analyzed.error_count > 0, "expected annotation mismatch to fail");
  expect_diagnostic(analyzed, "expected `int32`, found `str`",
                    "expected annotation type-mismatch diagnostic");
}

auto test_reports_integer_literal_overflow() -> void {
  const auto analyzed = analyze_one("module sample\n"
                                    "def run() -> unit:\n"
                                    "    let c: uint8 = 300\n");
  expect(analyzed.error_count > 0, "expected literal overflow to fail");
  expect_diagnostic(analyzed, "integer literal `300` does not fit in `uint8`",
                    "expected literal-fit diagnostic");
}

auto test_reports_mixed_numeric_types() -> void {
  const auto analyzed = analyze_one("module sample\n"
                                    "def run(a: int32, b: float64) -> float64:\n"
                                    "    return a + b\n");
  expect(analyzed.error_count > 0, "expected mixed numerics to fail");
  expect_diagnostic(analyzed,
                    "mismatched numeric types `int32` and `float64`",
                    "expected mixed-numeric diagnostic");
}

auto test_reports_non_bool_condition() -> void {
  const auto analyzed = analyze_one("module sample\n"
                                    "def run(n: int32) -> unit:\n"
                                    "    if n:\n"
                                    "        println(\"yes\")\n");
  expect(analyzed.error_count > 0, "expected non-bool condition to fail");
  expect_diagnostic(analyzed, "must be `bool`, found `int32`",
                    "expected bool-condition diagnostic");
}

auto test_reports_assignment_to_immutable() -> void {
  const auto analyzed = analyze_one("module sample\n"
                                    "def run() -> unit:\n"
                                    "    let count = 0\n"
                                    "    count = count + 1\n");
  expect(analyzed.error_count > 0, "expected immutable assignment to fail");
  expect_diagnostic(analyzed, "cannot assign to immutable binding `count`",
                    "expected immutable-assignment diagnostic");
}

auto test_reports_return_type_mismatch() -> void {
  const auto analyzed = analyze_one("module sample\n"
                                    "def run() -> str:\n"
                                    "    return 42\n");
  expect(analyzed.error_count > 0, "expected return mismatch to fail");
  expect_diagnostic(analyzed, "return type mismatch: expected `str`",
                    "expected return-mismatch diagnostic");
}

auto test_reports_call_argument_problems() -> void {
  const auto analyzed = analyze_one(
      "module sample\n"
      "def greet(name: str, loud: bool = false) -> str:\n"
      "    return name\n"
      "def run() -> unit:\n"
      "    greet(\"a\", \"b\", \"c\")\n"
      "    greet(\"a\", volume: true)\n"
      "    greet()\n"
      "    greet(1)\n");
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
  const auto analyzed = analyze_one(
      "module sample\n"
      "type point = { x: float64, y: float64 }\n"
      "def run() -> unit:\n"
      "    let a = point { x: 1.0, z: 2.0 }\n"
      "    let b = point { x: 1.0 }\n");
  expect(analyzed.error_count > 0, "expected struct literal problems to fail");
  expect_diagnostic(analyzed, "unknown field `z` in struct literal for `point`",
                    "expected unknown-field diagnostic");
  expect_diagnostic(analyzed, "missing field `y` in struct literal for `point`",
                    "expected missing-field diagnostic");
}

auto test_reports_unknown_field_and_method() -> void {
  const auto analyzed = analyze_one(
      "module sample\n"
      "type point = { x: float64, y: float64 }\n"
      "def run(p: point) -> unit:\n"
      "    let a = p.z\n"
      "    p.normalize()\n");
  expect(analyzed.error_count > 0, "expected member problems to fail");
  expect_diagnostic(analyzed, "no field `z` on struct `point`",
                    "expected unknown-field diagnostic");
  expect_diagnostic(analyzed, "no method `normalize` on type `point`",
                    "expected unknown-method diagnostic");
}

auto test_reports_non_exhaustive_match() -> void {
  const auto analyzed = analyze_one(
      "module sample\n"
      "type shape = @circle(float64) | @rect(float64, float64) | @point\n"
      "def area(s: shape) -> float64:\n"
      "    match s:\n"
      "        @circle(r) => r\n");
  expect(analyzed.error_count > 0, "expected non-exhaustive match to fail");
  expect_diagnostic(analyzed,
                    "non-exhaustive match on `shape`: `@rect`, `@point` not "
                    "covered",
                    "expected exhaustiveness diagnostic");
}

auto test_reports_unknown_variant_in_pattern() -> void {
  const auto analyzed = analyze_one(
      "module sample\n"
      "type shape = @circle(float64) | @point\n"
      "def check(s: shape) -> bool:\n"
      "    match s:\n"
      "        @square(n) => true\n"
      "        _ => false\n");
  expect(analyzed.error_count > 0, "expected unknown variant to fail");
  expect_diagnostic(analyzed, "unknown variant `@square` for sum type `shape`",
                    "expected unknown-variant diagnostic");
}

auto test_reports_try_in_plain_function() -> void {
  const auto analyzed = analyze_one(
      "module sample\n"
      "def find(id: int32) -> option[int32]:\n"
      "    return @some(id)\n"
      "def run(id: int32) -> int32:\n"
      "    let v = find(id)?\n"
      "    return v\n");
  expect(analyzed.error_count > 0, "expected `?` misuse to fail");
  expect_diagnostic(analyzed,
                    "cannot use `?` in a function that returns `int32`",
                    "expected try-propagation diagnostic");
}

auto test_reports_unannotated_pub_function() -> void {
  const auto analyzed = analyze_one("module sample\n"
                                    "pub def run(x):\n"
                                    "    return x\n");
  expect(analyzed.error_count > 0, "expected unannotated pub fn to fail");
  expect_diagnostic(analyzed,
                    "public function `run` must annotate its parameters and "
                    "return type",
                    "expected pub-annotation diagnostic");
}

auto test_reports_duplicate_trait_impl() -> void {
  const auto analyzed = analyze_one(
      "module sample\n"
      "type point = { x: float64 }\n"
      "trait show2:\n"
      "    def show(self) -> str\n"
      "impl show2 for point:\n"
      "    def show(self) -> str:\n"
      "        return \"a\"\n"
      "impl show2 for point:\n"
      "    def show(self) -> str:\n"
      "        return \"b\"\n");
  expect(analyzed.error_count > 0, "expected duplicate impl to fail");
  expect_diagnostic(analyzed,
                    "duplicate implementation of trait `show2` for `point`",
                    "expected coherence diagnostic");
}

auto test_reports_incomplete_trait_impl() -> void {
  const auto analyzed = analyze_one(
      "module sample\n"
      "type point = { x: float64 }\n"
      "trait shape2:\n"
      "    def area(self) -> float64\n"
      "    def name(self) -> str\n"
      "impl shape2 for point:\n"
      "    def area(self) -> float64:\n"
      "        return self.x\n"
      "    def extra(self) -> str:\n"
      "        return \"?\"\n");
  expect(analyzed.error_count > 0, "expected incomplete impl to fail");
  expect_diagnostic(analyzed,
                    "implementation of trait `shape2` for `point` is missing "
                    "method `name`",
                    "expected missing-method diagnostic");
  expect_diagnostic(analyzed, "`extra` is not a member of trait `shape2`",
                    "expected extra-method diagnostic");
}

auto test_reports_unsatisfied_trait_requirement() -> void {
  const auto analyzed = analyze_one(
      "module sample\n"
      "type point = { x: float64 }\n"
      "trait basic_eq:\n"
      "    def equal(self, other: self) -> bool\n"
      "trait basic_ord requires basic_eq:\n"
      "    def less(self, other: self) -> bool\n"
      "impl basic_ord for point:\n"
      "    def less(self, other: self) -> bool:\n"
      "        return true\n");
  expect(analyzed.error_count > 0, "expected unsatisfied requires to fail");
  expect_diagnostic(analyzed,
                    "trait `basic_ord` requires `basic_eq`, but `point` does "
                    "not implement it",
                    "expected requires diagnostic");
}

auto test_reports_unknown_deriving() -> void {
  const auto analyzed = analyze_one(
      "module sample\n"
      "type color = @red | @green | @blue\n"
      "    deriving eq, serialize\n");
  expect(analyzed.error_count > 0, "expected unknown deriving to fail");
  expect_diagnostic(analyzed, "cannot derive `serialize` for `color`",
                    "expected deriving diagnostic");
}

auto test_reports_impure_contract_call() -> void {
  const auto analyzed = analyze_one(
      "module sample\n"
      "def log_it(x: int32) -> bool:\n"
      "    println(\"{x}\")\n"
      "    return true\n"
      "def run(x: int32) -> int32\n"
      "pre log_it(x)\n"
      ": x\n");
  expect(analyzed.error_count > 0, "expected impure contract call to fail");
  expect_diagnostic(analyzed,
                    "contract conditions may only call pure functions",
                    "expected contract-purity diagnostic");
}

} // namespace

auto main() -> int {
  try {
    test_accepts_typed_core_program();
    test_accepts_structs_and_methods();
    test_accepts_collections_and_lambdas();
    test_accepts_option_result_flow();
    test_accepts_cross_module_qualified_types();

    test_reports_undefined_name_with_suggestion();
    test_reports_undefined_type();
    test_reports_annotation_mismatch();
    test_reports_integer_literal_overflow();
    test_reports_mixed_numeric_types();
    test_reports_non_bool_condition();
    test_reports_assignment_to_immutable();
    test_reports_return_type_mismatch();
    test_reports_call_argument_problems();
    test_reports_struct_literal_problems();
    test_reports_unknown_field_and_method();
    test_reports_non_exhaustive_match();
    test_reports_unknown_variant_in_pattern();
    test_reports_try_in_plain_function();
    test_reports_unannotated_pub_function();
    test_reports_duplicate_trait_impl();
    test_reports_incomplete_trait_impl();
    test_reports_unsatisfied_trait_requirement();
    test_reports_unknown_deriving();
    test_reports_impure_contract_call();
  } catch (const std::exception &ex) {
    std::cerr << "check_test failed: unhandled exception: " << ex.what() << '\n';
    std::exit(1);
  }
  return 0;
}
