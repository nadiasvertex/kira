#include <array>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "src/bytecode/value.h"
#include "src/bytecode/vm.h"
#include "src/bytecode_compiler/compile.h"
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
namespace bc = kira::bytecode;
namespace bcc = kira::bytecode_compiler;

// Parses and checks one fixture source, mirroring src/hir/lower_test.cpp's
// own fixture helper — this compiler's tests exercise the real
// lex -> parse -> check -> lower -> compile -> run pipeline end to end
// rather than hand-assembling HIR, since that's what actually proves the
// compiler produces bytecode the VM agrees with for real surface syntax.
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

auto compile_fixture(const std::string &text) -> bc::bytecode_module {
  auto fixture = check_fixture(text);
  auto module = hir::lower_module(*fixture.ast_file, "sample", fixture.checked);
  expect(module.has_value(), "expected fixture to lower to HIR");
  auto compiled = bcc::compile_module(**module, fixture.checked.types);
  expect(compiled.has_value(), "expected fixture to compile to bytecode");
  return std::move(*compiled);
}

auto function_index(const bc::bytecode_module &module, std::string_view name)
    -> uint16_t {
  for (size_t i = 0; i < module.functions.size(); ++i) {
    if (module.functions[i].name == name) {
      return static_cast<uint16_t>(i);
    }
  }
  fail("expected to find a compiled function by name");
}

auto test_add_compiles_and_runs() -> void {
  auto module = compile_fixture("module sample\n"
                                "def add(x: int32, y: int32) -> int32:\n"
                                "    return x + y\n");
  const auto vm = bc::vm{module};
  const auto args =
      std::array{bc::slot_value{int64_t{10}}, bc::slot_value{int64_t{32}}};
  auto result = vm.run(function_index(module, "add"), args);
  expect(result.has_value(), "expected add() to succeed");
  expect(result->has_value, "expected add() to produce a value");
  expect(result->value.i == 42, "expected 10 + 32 == 42");
}

auto test_implicit_tail_expression_is_the_return_value() -> void {
  // No explicit `return` — the last statement's value is the function's
  // result (spec/typed-ir-design.md's Rust-like trailing-expression rule,
  // enforced by check_function; see check.cpp's "mismatched final
  // expression" diagnostic).
  auto module = compile_fixture("module sample\n"
                                "def double(x: int32) -> int32:\n"
                                "    x * 2\n");
  const auto vm = bc::vm{module};
  const auto args = std::array{bc::slot_value{int64_t{21}}};
  auto result = vm.run(function_index(module, "double"), args);
  expect(result.has_value(), "expected double() to succeed");
  expect(result->value.i == 42, "expected 21 * 2 == 42");
}

auto test_if_expression_selects_branch_value() -> void {
  auto module = compile_fixture("module sample\n"
                                "def abs_val(x: int32) -> int32:\n"
                                "    return if x < 0: -x else: x\n");
  const auto vm = bc::vm{module};

  const auto neg_args = std::array{bc::slot_value{int64_t{-7}}};
  auto neg_result = vm.run(function_index(module, "abs_val"), neg_args);
  expect(neg_result.has_value(), "expected abs_val(-7) to succeed");
  expect(neg_result->value.i == 7, "expected abs_val(-7) == 7");

  const auto pos_args = std::array{bc::slot_value{int64_t{7}}};
  auto pos_result = vm.run(function_index(module, "abs_val"), pos_args);
  expect(pos_result.has_value(), "expected abs_val(7) to succeed");
  expect(pos_result->value.i == 7, "expected abs_val(7) == 7");
}

auto test_while_loop_sums_one_to_n() -> void {
  auto module = compile_fixture("module sample\n"
                                "def sum_to_n(n: int32) -> int32:\n"
                                "    var total = 0\n"
                                "    var i = 1\n"
                                "    while i <= n:\n"
                                "        total = total + i\n"
                                "        i = i + 1\n"
                                "    return total\n");
  const auto vm = bc::vm{module};
  const auto args = std::array{bc::slot_value{int64_t{10}}};
  auto result = vm.run(function_index(module, "sum_to_n"), args);
  expect(result.has_value(), "expected sum_to_n(10) to succeed");
  expect(result->value.i == 55, "expected sum(1..=10) == 55");
}

auto test_recursive_call_computes_factorial() -> void {
  auto module = compile_fixture("module sample\n"
                                "def factorial(n: int32) -> int32:\n"
                                "    if n <= 1:\n"
                                "        return 1\n"
                                "    return n * factorial(n - 1)\n");
  const auto vm = bc::vm{module};
  const auto index = function_index(module, "factorial");

  auto result5 = vm.run(index, std::array{bc::slot_value{int64_t{5}}});
  expect(result5.has_value(), "expected factorial(5) to succeed");
  expect(result5->value.i == 120, "expected factorial(5) == 120");

  auto result0 = vm.run(index, std::array{bc::slot_value{int64_t{0}}});
  expect(result0.has_value(), "expected factorial(0) to succeed");
  expect(result0->value.i == 1, "expected factorial(0) == 1");
}

auto test_calls_another_function_in_the_same_module() -> void {
  auto module =
      compile_fixture("module sample\n"
                      "def square(x: int32) -> int32:\n"
                      "    return x * x\n"
                      "def sum_of_squares(a: int32, b: int32) -> int32:\n"
                      "    return square(a) + square(b)\n");
  const auto vm = bc::vm{module};
  const auto args =
      std::array{bc::slot_value{int64_t{3}}, bc::slot_value{int64_t{4}}};
  auto result = vm.run(function_index(module, "sum_of_squares"), args);
  expect(result.has_value(), "expected sum_of_squares() to succeed");
  expect(result->value.i == 25, "expected 3^2 + 4^2 == 25");
}

auto test_and_or_short_circuit_to_correct_value() -> void {
  auto module = compile_fixture("module sample\n"
                                "def classify(x: int32) -> bool:\n"
                                "    return x > 0 and x < 10 or x == -1\n");
  const auto vm = bc::vm{module};
  const auto index = function_index(module, "classify");

  auto in_range = vm.run(index, std::array{bc::slot_value{int64_t{5}}});
  expect(in_range.has_value() && in_range->value.i == 1,
         "expected classify(5) == true");

  auto sentinel = vm.run(index, std::array{bc::slot_value{int64_t{-1}}});
  expect(sentinel.has_value() && sentinel->value.i == 1,
         "expected classify(-1) == true");

  auto outside = vm.run(index, std::array{bc::slot_value{int64_t{99}}});
  expect(outside.has_value() && outside->value.i == 0,
         "expected classify(99) == false");
}

auto test_cast_widens_int_to_float() -> void {
  auto module = compile_fixture("module sample\n"
                                "def to_float(x: int32) -> float64:\n"
                                "    return x as float64\n");
  const auto vm = bc::vm{module};
  const auto args = std::array{bc::slot_value{int64_t{21}}};
  auto result = vm.run(function_index(module, "to_float"), args);
  expect(result.has_value(), "expected to_float() to succeed");
  expect(result->value.f == 21.0, "expected int32(21) as float64 == 21.0");
}

auto test_checked_add_panics_on_overflow_end_to_end() -> void {
  auto module = compile_fixture("module sample\n"
                                "def add8(x: int8, y: int8) -> int8:\n"
                                "    return x + y\n");
  const auto vm = bc::vm{module};
  const auto args =
      std::array{bc::slot_value{int64_t{100}}, bc::slot_value{int64_t{100}}};
  auto result = vm.run(function_index(module, "add8"), args);
  expect(!result.has_value(), "expected int8 100+100 to overflow and panic");
  expect(result.error() == bc::panic_reason::integer_overflow,
         "expected the panic reason to be integer_overflow");
}

auto test_string_literal_len_reads_the_heap_header() -> void {
  // No surface `.len()` yet — reads the heap `str` value's own length slot
  // directly via a hand-assembled op_load_slot, mirroring vm_test.cpp's
  // own str test, just compiled from real source this time.
  auto module = compile_fixture("module sample\n"
                                "def greet() -> str:\n"
                                "    return \"hello\"\n");
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "greet"), {});
  expect(result.has_value(), "expected greet() to succeed");
  expect(result->has_value, "expected greet() to produce a value");
  const auto *slots = reinterpret_cast<const bc::slot_value *>(
      static_cast<uintptr_t>(result->value.u));
  expect(slots[0].u == 5, "expected \"hello\" to report length 5");
  const auto *data =
      reinterpret_cast<const char *>(static_cast<uintptr_t>(slots[1].u));
  expect(std::string_view(data, 5) == "hello",
         "expected the str's data slot to point at \"hello\"'s bytes");
}

auto test_tuple_construction_and_projection() -> void {
  auto module = compile_fixture("module sample\n"
                                "def make() -> (int32, int32, int32):\n"
                                "    return (10, 20, 12)\n");
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "make"), {});
  expect(result.has_value(), "expected make() to succeed");
  const auto *slots = reinterpret_cast<const bc::slot_value *>(
      static_cast<uintptr_t>(result->value.u));
  expect(slots[0].i + slots[1].i + slots[2].i == 42,
         "expected the tuple's three elements to sum to 42");
}

auto test_struct_literal_and_field_access() -> void {
  auto module = compile_fixture("module sample\n"
                                "type point = { pub x: int32, pub y: int32 }\n"
                                "def sum_fields(x: int32, y: int32) -> int32:\n"
                                "    let p: point = { x: x, y: y }\n"
                                "    return (p).x + (p).y\n");
  const auto vm = bc::vm{module};
  const auto args =
      std::array{bc::slot_value{int64_t{18}}, bc::slot_value{int64_t{24}}};
  auto result = vm.run(function_index(module, "sum_fields"), args);
  expect(result.has_value(), "expected sum_fields() to succeed");
  expect(result->value.i == 42, "expected 18 + 24 == 42 via field access");
}

auto test_fixed_array_construction_and_indexing() -> void {
  auto module =
      compile_fixture("module sample\n"
                      "def third(i: usize) -> int32:\n"
                      "    let a: array[int32, 4] = [10, 20, 30, 40]\n"
                      "    return a[i]\n");
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "third"),
                       std::array{bc::slot_value{uint64_t{2}}});
  expect(result.has_value(), "expected third(2) to succeed");
  expect(result->value.i == 30, "expected a[2] == 30");
}

auto test_array_index_out_of_bounds_panics() -> void {
  auto module = compile_fixture("module sample\n"
                                "def get(i: usize) -> int32:\n"
                                "    let a: array[int32, 3] = [1, 2, 3]\n"
                                "    return a[i]\n");
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "get"),
                       std::array{bc::slot_value{uint64_t{5}}});
  expect(!result.has_value(), "expected a[5] on a 3-element array to panic");
  expect(result.error() == bc::panic_reason::index_out_of_bounds,
         "expected the panic reason to be index_out_of_bounds");
}

auto test_array_fill_form_repeats_the_same_value() -> void {
  auto module = compile_fixture("module sample\n"
                                "def sum_of_fives() -> int32:\n"
                                "    let a: array[int32, 4] = [5; 4]\n"
                                "    return a[0] + a[1] + a[2] + a[3]\n");
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "sum_of_fives"), {});
  expect(result.has_value(), "expected sum_of_fives() to succeed");
  expect(result->value.i == 20, "expected four 5s to sum to 20");
}

auto test_sum_type_variant_with_payload_encodes_tag_and_slot() -> void {
  // No `match` yet (increment 4), so this reads the heap block's raw
  // {tag; payload} slots directly, mirroring how
  // test_string_literal_len_reads_the_heap_header reads a str's header.
  auto module =
      compile_fixture("module sample\n"
                      "type shape = @circle(float64) | @square(float64)\n"
                      "def make(r: float64) -> shape:\n"
                      "    return @circle(r)\n");
  const auto vm = bc::vm{module};
  auto result =
      vm.run(function_index(module, "make"), std::array{bc::slot_value{3.5}});
  expect(result.has_value(), "expected make() to succeed");
  const auto *slots = reinterpret_cast<const bc::slot_value *>(
      static_cast<uintptr_t>(result->value.u));
  expect(slots[0].i == 0, "expected @circle to encode as tag 0");
  expect(slots[1].f == 3.5, "expected the payload slot to hold 3.5");
}

auto test_sum_type_unit_variant_encodes_its_tag() -> void {
  auto module = compile_fixture("module sample\n"
                                "type shape = @circle(float64) | @empty\n"
                                "def make_empty() -> shape:\n"
                                "    return @empty\n");
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "make_empty"), {});
  expect(result.has_value(), "expected make_empty() to succeed");
  const auto *slots = reinterpret_cast<const bc::slot_value *>(
      static_cast<uintptr_t>(result->value.u));
  expect(slots[0].i == 1, "expected @empty to encode as tag 1");
}

} // namespace

auto main() -> int {
  try {
    test_add_compiles_and_runs();
    test_implicit_tail_expression_is_the_return_value();
    test_if_expression_selects_branch_value();
    test_while_loop_sums_one_to_n();
    test_recursive_call_computes_factorial();
    test_calls_another_function_in_the_same_module();
    test_and_or_short_circuit_to_correct_value();
    test_cast_widens_int_to_float();
    test_checked_add_panics_on_overflow_end_to_end();
    test_string_literal_len_reads_the_heap_header();
    test_tuple_construction_and_projection();
    test_struct_literal_and_field_access();
    test_fixed_array_construction_and_indexing();
    test_array_index_out_of_bounds_panics();
    test_array_fill_form_repeats_the_same_value();
    test_sum_type_variant_with_payload_encodes_tag_and_slot();
    test_sum_type_unit_variant_encodes_its_tag();
  } catch (const std::exception &ex) {
    std::cerr << "compile_test failed: unhandled exception: " << ex.what()
              << '\n';
    std::exit(1);
  }
  return 0;
}
