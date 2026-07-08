#include <array>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "src/bytecode/value.h"
#include "src/bytecode/vm.h"
#include "src/bytecode_compiler/compile.h"
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
#include "src/testing/test_data.h"

namespace {

using kira::testing::expect;
using kira::testing::fail;
namespace hir = kira::hir;
namespace bc = kira::bytecode;
namespace bcc = kira::bytecode_compiler;

// Shared with src/llvm_codegen/codegen_test.cpp: every fixture here carries
// both its named helper functions (so this file can still assert on
// individual functions directly through the VM) and a zero-argument
// `main()` that drives them with the same fixed inputs the LLVM JIT test
// uses, so both tiers can be checked against the exact same expected
// values from the exact same source.
auto test_data_dir = kira::testing::find_test_data_dir("codegen_test");

auto load_fixture(std::string_view filename) -> std::string {
  return kira::testing::load_test_data_file(test_data_dir.string(), filename);
}

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

// Every fixture's `main()` is the same zero-argument entry point
// src/llvm_codegen/codegen_test.cpp drives through the JIT, with the same
// fixed inputs baked in — running it here too means both tiers are checked
// against the identical expected value from the identical source, not just
// each tier's own hand-picked call.
auto run_main(const bc::bytecode_module &module)
    -> std::expected<bc::vm_result, bc::panic_reason> {
  return bc::vm{module}.run(function_index(module, "main"), {});
}

auto test_add_compiles_and_runs() -> void {
  auto module = compile_fixture(load_fixture("add.kira"));
  const auto vm = bc::vm{module};
  const auto args =
      std::array{bc::slot_value{int64_t{10}}, bc::slot_value{int64_t{32}}};
  auto result = vm.run(function_index(module, "add"), args);
  expect(result.has_value(), "expected add() to succeed");
  expect(result->has_value, "expected add() to produce a value");
  expect(result->value.i == 42, "expected 10 + 32 == 42");

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  expect(main_result->value.i == 42, "expected main()'s add(10, 32) == 42");
}

auto test_implicit_tail_expression_is_the_return_value() -> void {
  // No explicit `return` — the last statement's value is the function's
  // result (spec/typed-ir-design.md's Rust-like trailing-expression rule,
  // enforced by check_function; see check.cpp's "mismatched final
  // expression" diagnostic).
  auto module = compile_fixture(load_fixture("implicit_tail_expression.kira"));
  const auto vm = bc::vm{module};
  const auto args = std::array{bc::slot_value{int64_t{21}}};
  auto result = vm.run(function_index(module, "double"), args);
  expect(result.has_value(), "expected double() to succeed");
  expect(result->value.i == 42, "expected 21 * 2 == 42");

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  expect(main_result->value.i == 42, "expected main()'s double(21) == 42");
}

auto test_if_expression_selects_branch_value() -> void {
  auto module = compile_fixture(load_fixture("if_expression.kira"));
  const auto vm = bc::vm{module};

  const auto neg_args = std::array{bc::slot_value{int64_t{-7}}};
  auto neg_result = vm.run(function_index(module, "abs_val"), neg_args);
  expect(neg_result.has_value(), "expected abs_val(-7) to succeed");
  expect(neg_result->value.i == 7, "expected abs_val(-7) == 7");

  const auto pos_args = std::array{bc::slot_value{int64_t{7}}};
  auto pos_result = vm.run(function_index(module, "abs_val"), pos_args);
  expect(pos_result.has_value(), "expected abs_val(7) to succeed");
  expect(pos_result->value.i == 7, "expected abs_val(7) == 7");

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  expect(main_result->value.i == 14,
         "expected main()'s abs_val(-7) + abs_val(7) == 14");
}

auto test_while_loop_sums_one_to_n() -> void {
  auto module = compile_fixture(load_fixture("while_loop.kira"));
  const auto vm = bc::vm{module};
  const auto args = std::array{bc::slot_value{int64_t{10}}};
  auto result = vm.run(function_index(module, "sum_to_n"), args);
  expect(result.has_value(), "expected sum_to_n(10) to succeed");
  expect(result->value.i == 55, "expected sum(1..=10) == 55");

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  expect(main_result->value.i == 55, "expected main()'s sum_to_n(10) == 55");
}

auto test_recursive_call_computes_factorial() -> void {
  auto module = compile_fixture(load_fixture("recursive_factorial.kira"));
  const auto vm = bc::vm{module};
  const auto index = function_index(module, "factorial");

  auto result5 = vm.run(index, std::array{bc::slot_value{int64_t{5}}});
  expect(result5.has_value(), "expected factorial(5) to succeed");
  expect(result5->value.i == 120, "expected factorial(5) == 120");

  auto result0 = vm.run(index, std::array{bc::slot_value{int64_t{0}}});
  expect(result0.has_value(), "expected factorial(0) to succeed");
  expect(result0->value.i == 1, "expected factorial(0) == 1");

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  expect(main_result->value.i == 120, "expected main()'s factorial(5) == 120");
}

auto test_calls_another_function_in_the_same_module() -> void {
  auto module = compile_fixture(load_fixture("function_calls.kira"));
  const auto vm = bc::vm{module};
  const auto args =
      std::array{bc::slot_value{int64_t{3}}, bc::slot_value{int64_t{4}}};
  auto result = vm.run(function_index(module, "sum_of_squares"), args);
  expect(result.has_value(), "expected sum_of_squares() to succeed");
  expect(result->value.i == 25, "expected 3^2 + 4^2 == 25");

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  expect(main_result->value.i == 25,
         "expected main()'s sum_of_squares(3, 4) == 25");
}

auto test_and_or_short_circuit_to_correct_value() -> void {
  auto module = compile_fixture(load_fixture("and_or_short_circuit.kira"));
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

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  expect(main_result->value.i == 1,
         "expected main()'s classify(5) and classify(-1) and "
         "not classify(99) == true");
}

auto test_cast_widens_int_to_float() -> void {
  auto module = compile_fixture(load_fixture("cast_int_to_float.kira"));
  const auto vm = bc::vm{module};
  const auto args = std::array{bc::slot_value{int64_t{21}}};
  auto result = vm.run(function_index(module, "to_float"), args);
  expect(result.has_value(), "expected to_float() to succeed");
  expect(result->value.f == 21.0, "expected int32(21) as float64 == 21.0");

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  expect(main_result->value.f == 21.0,
         "expected main()'s to_float(21) == 21.0");
}

auto test_checked_add_panics_on_overflow_end_to_end() -> void {
  auto module = compile_fixture(load_fixture("checked_add_overflow.kira"));
  const auto vm = bc::vm{module};
  const auto args =
      std::array{bc::slot_value{int64_t{100}}, bc::slot_value{int64_t{100}}};
  auto result = vm.run(function_index(module, "add8"), args);
  expect(!result.has_value(), "expected int8 100+100 to overflow and panic");
  expect(result.error() == bc::panic_reason::integer_overflow,
         "expected the panic reason to be integer_overflow");

  auto main_result = run_main(module);
  expect(!main_result.has_value(),
         "expected main()'s add8(100, 100) to overflow and panic");
  expect(main_result.error() == bc::panic_reason::integer_overflow,
         "expected the panic reason to be integer_overflow");
}

auto test_checked_div_panics_on_divide_by_zero_end_to_end() -> void {
  auto module = compile_fixture(load_fixture("checked_div_by_zero.kira"));
  auto main_result = run_main(module);
  expect(!main_result.has_value(), "expected division by zero to panic");
  expect(main_result.error() == bc::panic_reason::integer_divide_by_zero,
         "expected the panic reason to be integer_divide_by_zero");
}

auto test_string_literal_len_reads_the_heap_header() -> void {
  // No surface `.len()` yet — reads the heap `str` value's own length slot
  // directly via a hand-assembled op_load_slot, mirroring vm_test.cpp's
  // own str test, just compiled from real source this time.
  auto module = compile_fixture(load_fixture("string_literal.kira"));
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

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  const auto *main_slots = reinterpret_cast<const bc::slot_value *>(
      static_cast<uintptr_t>(main_result->value.u));
  expect(main_slots[0].u == 5, "expected main()'s greet() to report length 5");
  const auto *main_data =
      reinterpret_cast<const char *>(static_cast<uintptr_t>(main_slots[1].u));
  expect(std::string_view(main_data, 5) == "hello",
         "expected main()'s greet() data slot to point at \"hello\"'s bytes");
}

auto test_tuple_construction_and_projection() -> void {
  auto module = compile_fixture(load_fixture("tuple_construction.kira"));
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "make"), {});
  expect(result.has_value(), "expected make() to succeed");
  const auto *slots = reinterpret_cast<const bc::slot_value *>(
      static_cast<uintptr_t>(result->value.u));
  expect(slots[0].i + slots[1].i + slots[2].i == 42,
         "expected the tuple's three elements to sum to 42");

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  expect(main_result->value.i == 42,
         "expected main()'s sum3() (destructuring make()'s tuple) == 42");
}

auto test_struct_literal_and_field_access() -> void {
  auto module = compile_fixture(load_fixture("struct_field_access.kira"));
  const auto vm = bc::vm{module};
  const auto args =
      std::array{bc::slot_value{int64_t{18}}, bc::slot_value{int64_t{24}}};
  auto result = vm.run(function_index(module, "sum_fields"), args);
  expect(result.has_value(), "expected sum_fields() to succeed");
  expect(result->value.i == 42, "expected 18 + 24 == 42 via field access");

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  expect(main_result->value.i == 42,
         "expected main()'s sum_fields(18, 24) == 42");
}

auto test_fixed_array_construction_and_indexing() -> void {
  auto module = compile_fixture(load_fixture("fixed_array_indexing.kira"));
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "third"),
                       std::array{bc::slot_value{uint64_t{2}}});
  expect(result.has_value(), "expected third(2) to succeed");
  expect(result->value.i == 30, "expected a[2] == 30");

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  expect(main_result->value.i == 30, "expected main()'s third(2) == 30");
}

auto test_array_index_out_of_bounds_panics() -> void {
  auto module = compile_fixture(load_fixture("array_out_of_bounds.kira"));
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "get"),
                       std::array{bc::slot_value{uint64_t{5}}});
  expect(!result.has_value(), "expected a[5] on a 3-element array to panic");
  expect(result.error() == bc::panic_reason::index_out_of_bounds,
         "expected the panic reason to be index_out_of_bounds");

  auto main_result = run_main(module);
  expect(!main_result.has_value(),
         "expected main()'s get(5) on a 3-element array to panic");
  expect(main_result.error() == bc::panic_reason::index_out_of_bounds,
         "expected the panic reason to be index_out_of_bounds");
}

auto test_array_fill_form_repeats_the_same_value() -> void {
  auto module = compile_fixture(load_fixture("array_fill.kira"));
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "sum_of_fives"), {});
  expect(result.has_value(), "expected sum_of_fives() to succeed");
  expect(result->value.i == 20, "expected four 5s to sum to 20");

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  expect(main_result->value.i == 20, "expected main()'s sum_of_fives() == 20");
}

auto test_sum_type_variant_with_payload_encodes_tag_and_slot() -> void {
  auto module = compile_fixture(load_fixture("sum_type_variant.kira"));
  const auto vm = bc::vm{module};
  auto result =
      vm.run(function_index(module, "make"), std::array{bc::slot_value{3.5}});
  expect(result.has_value(), "expected make() to succeed");
  const auto *slots = reinterpret_cast<const bc::slot_value *>(
      static_cast<uintptr_t>(result->value.u));
  expect(slots[0].i == 0, "expected @circle to encode as tag 0");
  expect(slots[1].f == 3.5, "expected the payload slot to hold 3.5");

  // main() is `make(3.5)` — src/llvm_codegen/codegen_test.cpp decodes the
  // same heap value through the JIT's `run_ptr_result`, so this and that
  // test check the exact same tag/payload encoding for the exact same
  // input, just through two different compilers.
  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  const auto *main_slots = reinterpret_cast<const bc::slot_value *>(
      static_cast<uintptr_t>(main_result->value.u));
  expect(main_slots[0].i == 0, "expected main()'s @circle to encode as tag 0");
  expect(main_slots[1].f == 3.5, "expected main()'s payload slot to hold 3.5");
}

auto test_sum_type_unit_variant_encodes_its_tag() -> void {
  auto module = compile_fixture(load_fixture("sum_type_unit_variant.kira"));
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "make_empty"), {});
  expect(result.has_value(), "expected make_empty() to succeed");
  const auto *slots = reinterpret_cast<const bc::slot_value *>(
      static_cast<uintptr_t>(result->value.u));
  expect(slots[0].i == 1, "expected @empty to encode as tag 1");

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  const auto *main_slots = reinterpret_cast<const bc::slot_value *>(
      static_cast<uintptr_t>(main_result->value.u));
  expect(main_slots[0].i == 1, "expected main()'s @empty to encode as tag 1");
}

auto test_match_dispatches_on_literal_and_wildcard_patterns() -> void {
  auto module = compile_fixture(load_fixture("match_literal_wildcard.kira"));
  const auto vm = bc::vm{module};
  const auto idx = function_index(module, "classify");
  auto r0 = vm.run(idx, std::array{bc::slot_value{int64_t{0}}});
  expect(r0.has_value() && r0->value.i == 100, "expected classify(0) == 100");
  auto r1 = vm.run(idx, std::array{bc::slot_value{int64_t{1}}});
  expect(r1.has_value() && r1->value.i == 200, "expected classify(1) == 200");
  auto r2 = vm.run(idx, std::array{bc::slot_value{int64_t{9}}});
  expect(r2.has_value() && r2->value.i == 300, "expected classify(9) == 300");

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  expect(main_result->value.i == 200, "expected main()'s classify(1) == 200");
}

auto test_match_or_pattern_matches_any_alternative() -> void {
  auto module = compile_fixture(load_fixture("match_or_pattern.kira"));
  const auto vm = bc::vm{module};
  const auto idx = function_index(module, "is_small");
  auto r1 = vm.run(idx, std::array{bc::slot_value{int64_t{1}}});
  expect(r1.has_value() && r1->value.i == 1, "expected is_small(1) == true");
  auto r5 = vm.run(idx, std::array{bc::slot_value{int64_t{5}}});
  expect(r5.has_value() && r5->value.i == 0, "expected is_small(5) == false");

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  expect(main_result->value.i == 1,
         "expected main()'s is_small(2) and not is_small(5) == true");
}

auto test_match_range_pattern() -> void {
  auto module = compile_fixture(load_fixture("match_range_pattern.kira"));
  const auto vm = bc::vm{module};
  const auto idx = function_index(module, "bucket");
  auto r5 = vm.run(idx, std::array{bc::slot_value{int64_t{5}}});
  expect(r5.has_value() && r5->value.i == 1, "expected bucket(5) == 1");
  auto r20 = vm.run(idx, std::array{bc::slot_value{int64_t{20}}});
  expect(r20.has_value() && r20->value.i == 2, "expected bucket(20) == 2");
  auto r99 = vm.run(idx, std::array{bc::slot_value{int64_t{99}}});
  expect(r99.has_value() && r99->value.i == 3, "expected bucket(99) == 3");

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  expect(main_result->value.i == 2, "expected main()'s bucket(20) == 2");
}

auto test_match_guard_refines_a_pattern() -> void {
  // The guard references `x` (the enclosing function parameter, already
  // bound before the `match` starts) rather than a name the pattern itself
  // introduces — a guard that reads a pattern-bound name is a known
  // pre-existing lowering gap (`hir_match_arm.guard` is lowered before the
  // pattern's synthetic `hir_let` bindings, which only live in `body`), out
  // of scope for this increment's bytecode_compiler/llvm_codegen work.
  auto module = compile_fixture(load_fixture("match_guard.kira"));
  const auto vm = bc::vm{module};
  const auto idx = function_index(module, "sign");
  auto rpos = vm.run(idx, std::array{bc::slot_value{int64_t{7}}});
  expect(rpos.has_value() && rpos->value.i == 1, "expected sign(7) == 1");
  auto rneg = vm.run(idx, std::array{bc::slot_value{int64_t{-7}}});
  expect(rneg.has_value() && rneg->value.i == -1, "expected sign(-7) == -1");
  auto rzero = vm.run(idx, std::array{bc::slot_value{int64_t{0}}});
  expect(rzero.has_value() && rzero->value.i == 0, "expected sign(0) == 0");

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  expect(main_result->value.i == -1, "expected main()'s sign(-7) == -1");
}

auto test_match_tuple_pattern_with_literal_and_binding() -> void {
  auto module = compile_fixture(load_fixture("match_tuple_pattern.kira"));
  const auto vm = bc::vm{module};
  const auto idx = function_index(module, "describe");
  auto r_zero_first = vm.run(
      idx, std::array{bc::slot_value{int64_t{0}}, bc::slot_value{int64_t{42}}});
  expect(r_zero_first.has_value() && r_zero_first->value.i == 42,
         "expected describe(0, 42) == 42 via the first arm's binding");
  auto r_general = vm.run(idx, std::array{bc::slot_value{int64_t{18}},
                                          bc::slot_value{int64_t{24}}});
  expect(r_general.has_value() && r_general->value.i == 42,
         "expected describe(18, 24) == 42 via the second arm");

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  expect(main_result->value.i == 84,
         "expected main()'s describe(0, 42) + describe(18, 24) == 84");
}

auto test_match_constructor_pattern_over_a_sum_type() -> void {
  auto module = compile_fixture(load_fixture("match_constructor_pattern.kira"));
  const auto vm = bc::vm{module};
  auto circle_result = vm.run(function_index(module, "circle_area"),
                              std::array{bc::slot_value{4.0}});
  expect(circle_result.has_value(), "expected circle_area(4.0) to succeed");
  expect(circle_result->value.f == 16.0, "expected area(@circle(4.0)) == 16.0");

  auto empty_result = vm.run(function_index(module, "empty_area"), {});
  expect(empty_result.has_value(), "expected empty_area() to succeed");
  expect(empty_result->value.f == 0.0, "expected area(@empty) == 0.0");

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  expect(main_result->value.f == 16.0,
         "expected main()'s circle_area(4.0) + empty_area() == 16.0");
}

auto test_match_struct_pattern_destructures_named_fields() -> void {
  auto module = compile_fixture(load_fixture("match_struct_pattern.kira"));
  const auto vm = bc::vm{module};
  const auto args =
      std::array{bc::slot_value{int64_t{18}}, bc::slot_value{int64_t{24}}};
  auto result = vm.run(function_index(module, "sum_fields"), args);
  expect(result.has_value(), "expected sum_fields() to succeed");
  expect(result->value.i == 42, "expected 18 + 24 == 42 via struct pattern");

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  expect(main_result->value.i == 42,
         "expected main()'s sum_fields(18, 24) == 42 via struct pattern");
}

auto test_list_literal_construction_and_indexing() -> void {
  auto module = compile_fixture(load_fixture("list_literal_indexing.kira"));
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "third"),
                       std::array{bc::slot_value{uint64_t{2}}});
  expect(result.has_value(), "expected third(2) to succeed");
  expect(result->value.i == 30, "expected xs[2] == 30");

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  expect(main_result->value.i == 30, "expected main()'s third(2) == 30");
}

auto test_list_index_out_of_bounds_panics() -> void {
  auto module = compile_fixture(load_fixture("list_out_of_bounds.kira"));
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "get"),
                       std::array{bc::slot_value{uint64_t{5}}});
  expect(!result.has_value(), "expected xs[5] on a 3-element list to panic");
  expect(result.error() == bc::panic_reason::index_out_of_bounds,
         "expected the panic reason to be index_out_of_bounds");

  auto main_result = run_main(module);
  expect(!main_result.has_value(),
         "expected main()'s xs[5] on a 3-element list to panic");
  expect(main_result.error() == bc::panic_reason::index_out_of_bounds,
         "expected the panic reason to be index_out_of_bounds");
}

auto test_list_fill_form_grows_to_a_runtime_count() -> void {
  auto module = compile_fixture(load_fixture("list_fill.kira"));
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "sum_of_fives"),
                       std::array{bc::slot_value{uint64_t{4}}});
  expect(result.has_value(), "expected sum_of_fives(4) to succeed");
  expect(result->value.i == 20,
         "expected four 5s (a runtime fill count) to sum to 20");

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  expect(main_result->value.i == 20, "expected main()'s sum_of_fives(4) == 20");
}

auto test_list_for_loop_sums_every_element() -> void {
  auto module = compile_fixture(load_fixture("list_for_loop.kira"));
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "main"), {});
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 15, "expected 1+2+3+4+5 == 15");
}

auto test_while_let_loops_until_the_pattern_stops_matching() -> void {
  auto module = compile_fixture(load_fixture("while_let_pattern.kira"));
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "sum_while"), {});
  expect(result.has_value(), "expected sum_while() to succeed");
  expect(result->value.i == 10, "expected 0+1+2+3+4 == 10");

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  expect(main_result->value.i == 10, "expected main()'s sum_while() == 10");
}

auto test_let_else_diverges_on_a_failed_pattern() -> void {
  auto module = compile_fixture(load_fixture("let_else_pattern.kira"));
  const auto vm = bc::vm{module};
  auto some_result = vm.run(function_index(module, "some_case"), {});
  expect(some_result.has_value(), "expected some_case() to succeed");
  expect(some_result->value.i == 7, "expected unwrap_or(@some(7)) == 7");
  auto none_result = vm.run(function_index(module, "none_case"), {});
  expect(none_result.has_value(), "expected none_case() to succeed");
  expect(none_result->value.i == -1, "expected unwrap_or(@none) == -1");

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  expect(main_result->value.i == 6,
         "expected main()'s some_case() + none_case() == 7 + -1");
}

auto test_list_comprehension_builds_and_reads_back_a_list() -> void {
  auto module = compile_fixture(load_fixture("list_comprehension.kira"));
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "sum_squares"),
                       std::array{bc::slot_value{int64_t{4}}});
  expect(result.has_value(), "expected sum_squares(4) to succeed");
  expect(result->value.i == 14, "expected 0^2+1^2+2^2+3^2 == 14");

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  expect(main_result->value.i == 14, "expected main()'s sum_squares(4) == 14");
}

auto test_closure_captures_an_outer_parameter_and_is_called_indirectly()
    -> void {
  auto module = compile_fixture(load_fixture("closure_capture.kira"));
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "main"), {});
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 8, "expected make_adder(5)(3) == 8");
}

auto test_non_capturing_closure_is_called_indirectly() -> void {
  auto module = compile_fixture(load_fixture("closure_noncapture.kira"));
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "main"), {});
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 42, "expected (x => x * 2)(21) == 42");
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
    test_checked_div_panics_on_divide_by_zero_end_to_end();
    test_string_literal_len_reads_the_heap_header();
    test_tuple_construction_and_projection();
    test_struct_literal_and_field_access();
    test_fixed_array_construction_and_indexing();
    test_array_index_out_of_bounds_panics();
    test_array_fill_form_repeats_the_same_value();
    test_sum_type_variant_with_payload_encodes_tag_and_slot();
    test_sum_type_unit_variant_encodes_its_tag();
    test_match_dispatches_on_literal_and_wildcard_patterns();
    test_match_or_pattern_matches_any_alternative();
    test_match_range_pattern();
    test_match_guard_refines_a_pattern();
    test_match_tuple_pattern_with_literal_and_binding();
    test_match_constructor_pattern_over_a_sum_type();
    test_match_struct_pattern_destructures_named_fields();
    test_list_literal_construction_and_indexing();
    test_list_index_out_of_bounds_panics();
    test_list_fill_form_grows_to_a_runtime_count();
    test_list_for_loop_sums_every_element();
    test_while_let_loops_until_the_pattern_stops_matching();
    test_let_else_diverges_on_a_failed_pattern();
    test_list_comprehension_builds_and_reads_back_a_list();
    test_closure_captures_an_outer_parameter_and_is_called_indirectly();
    test_non_capturing_closure_is_called_indirectly();
  } catch (const std::exception &ex) {
    std::cerr << "compile_test failed: unhandled exception: " << ex.what()
              << '\n';
    std::exit(1);
  }
  return 0;
}
