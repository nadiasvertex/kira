#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

#include "src/bytecode/panic.h"
#include "src/bytecode/value.h"
#include "src/hir/lower.h"
#include "src/hir/nodes.h"
#include "src/k-parser/diagnostic.h"
#include "src/k-parser/lexer.h"
#include "src/k-parser/parser.h"
#include "src/k-parser/source_location.h"
#include "src/llvm_codegen/codegen.h"
#include "src/llvm_codegen/jit_support.h"
#include "src/semantic/analysis.h"
#include "src/semantic/check.h"
#include "src/semantic/types.h"
#include "src/testing/test_assert.h"

namespace {

using kira::testing::expect;
using kira::testing::fail;
namespace hir = kira::hir;
namespace bc = kira::bytecode;
namespace lc = kira::llvm_codegen;

// Mirrors src/bytecode_compiler/compile_test.cpp's own fixture helper: these
// tests exercise the real lex -> parse -> check -> lower -> codegen -> JIT
// pipeline end to end, since that's what actually proves the lowering
// produces IR the JIT agrees with for real surface syntax.
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

struct jit_fixture {
  checked_fixture fixture;
  lc::jit_module jit;
};

auto jit_fixture_for(const std::string &text) -> jit_fixture {
  auto fixture = check_fixture(text);
  auto module = hir::lower_module(*fixture.ast_file, "sample", fixture.checked);
  expect(module.has_value(), "expected fixture to lower to HIR");
  auto compiled = lc::compile_module(**module, fixture.checked.types);
  expect(compiled.has_value(),
         "expected fixture to compile to an llvm::Module");
  auto jit = lc::jit_module::create(std::move(*compiled));
  expect(jit.has_value(), "expected the compiled module to JIT successfully");
  return jit_fixture{.fixture = std::move(fixture), .jit = std::move(*jit)};
}

auto test_add_compiles_and_runs() -> void {
  auto jf = jit_fixture_for("module sample\n"
                            "def main() -> int32:\n"
                            "    return 10 + 32\n");
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 42, "expected 10 + 32 == 42");
}

auto test_implicit_tail_expression_is_the_return_value() -> void {
  auto jf = jit_fixture_for("module sample\n"
                            "def double(x: int32) -> int32:\n"
                            "    x * 2\n"
                            "def main() -> int32:\n"
                            "    return double(21)\n");
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 42, "expected 21 * 2 == 42");
}

auto test_if_expression_selects_branch_value() -> void {
  auto jf = jit_fixture_for("module sample\n"
                            "def abs_val(x: int32) -> int32:\n"
                            "    return if x < 0: -x else: x\n"
                            "def main() -> int32:\n"
                            "    return abs_val(-7) + abs_val(7)\n");
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 14, "expected abs(-7) + abs(7) == 14");
}

auto test_while_loop_sums_one_to_n() -> void {
  auto jf = jit_fixture_for("module sample\n"
                            "def sum_to_n(n: int32) -> int32:\n"
                            "    var total = 0\n"
                            "    var i = 1\n"
                            "    while i <= n:\n"
                            "        total = total + i\n"
                            "        i = i + 1\n"
                            "    return total\n"
                            "def main() -> int32:\n"
                            "    return sum_to_n(10)\n");
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 55, "expected sum(1..=10) == 55");
}

auto test_recursive_call_computes_factorial() -> void {
  auto jf = jit_fixture_for("module sample\n"
                            "def factorial(n: int32) -> int32:\n"
                            "    if n <= 1:\n"
                            "        return 1\n"
                            "    return n * factorial(n - 1)\n"
                            "def main() -> int32:\n"
                            "    return factorial(5)\n");
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 120, "expected factorial(5) == 120");
}

auto test_and_or_short_circuit_to_correct_value() -> void {
  auto jf = jit_fixture_for("module sample\n"
                            "def classify(x: int32) -> bool:\n"
                            "    return x > 0 and x < 10 or x == -1\n"
                            "def main() -> bool:\n"
                            "    return classify(5) and classify(-1) and "
                            "not classify(99)\n");
  auto result = jf.jit.run("main", bc::numeric_kind::boolean);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 1,
         "expected the combined classification to be true");
}

auto test_cast_widens_int_to_float() -> void {
  auto jf = jit_fixture_for("module sample\n"
                            "def to_float(x: int32) -> float64:\n"
                            "    return x as float64\n"
                            "def main() -> float64:\n"
                            "    return to_float(21)\n");
  auto result = jf.jit.run("main", bc::numeric_kind::f64);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.f == 21.0, "expected int32(21) as float64 == 21.0");
}

auto test_checked_add_panics_on_overflow_end_to_end() -> void {
  auto jf = jit_fixture_for("module sample\n"
                            "def main() -> int8:\n"
                            "    var x: int8 = 100\n"
                            "    return x + x\n");
  auto result = jf.jit.run("main", bc::numeric_kind::i8);
  expect(!result.has_value(), "expected int8 100+100 to overflow and panic");
  expect(result.error() == bc::panic_reason::integer_overflow,
         "expected the panic reason to be integer_overflow");
}

auto test_checked_div_panics_on_divide_by_zero() -> void {
  auto jf = jit_fixture_for("module sample\n"
                            "def main() -> int32:\n"
                            "    var y = 0\n"
                            "    return 10 / y\n");
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(!result.has_value(), "expected division by zero to panic");
  expect(result.error() == bc::panic_reason::integer_divide_by_zero,
         "expected the panic reason to be integer_divide_by_zero");
}

auto test_string_literal_is_unsupported_type() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def greet() -> str:\n"
                               "    return \"hi\"\n");
  auto module = hir::lower_module(*fixture.ast_file, "sample", fixture.checked);
  expect(module.has_value(), "expected fixture to lower to HIR");
  auto compiled = lc::compile_module(**module, fixture.checked.types);
  expect(!compiled.has_value(),
         "expected a str-returning function to fail llvm_codegen — heap "
         "types are increment 6, not this increment");
  expect(compiled.error().kind == lc::codegen_error_kind::unsupported_type,
         "expected the failure reason to be unsupported_type");
}

} // namespace

auto main() -> int {
  try {
    test_add_compiles_and_runs();
    test_implicit_tail_expression_is_the_return_value();
    test_if_expression_selects_branch_value();
    test_while_loop_sums_one_to_n();
    test_recursive_call_computes_factorial();
    test_and_or_short_circuit_to_correct_value();
    test_cast_widens_int_to_float();
    test_checked_add_panics_on_overflow_end_to_end();
    test_checked_div_panics_on_divide_by_zero();
    test_string_literal_is_unsupported_type();
  } catch (const std::exception &ex) {
    std::cerr << "codegen_test failed: unhandled exception: " << ex.what()
              << '\n';
    std::exit(1);
  }
  return 0;
}
