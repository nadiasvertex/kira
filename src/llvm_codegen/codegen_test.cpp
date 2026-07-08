#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "src/bytecode/panic.h"
#include "src/bytecode/value.h"
#include "src/hir/lower.h"
#include "src/hir/nodes.h"
#include "src/llvm_codegen/codegen.h"
#include "src/llvm_codegen/jit_support.h"
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
namespace lc = kira::llvm_codegen;

auto test_data_dir = kira::testing::find_test_data_dir("codegen_test");

auto load_fixture(std::string_view filename) -> std::string {
  return kira::testing::load_test_data_file(test_data_dir.string(), filename);
}

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
  auto jf = jit_fixture_for(load_fixture("add.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 42, "expected 10 + 32 == 42");
}

auto test_implicit_tail_expression_is_the_return_value() -> void {
  auto jf = jit_fixture_for(load_fixture("implicit_tail_expression.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 42, "expected 21 * 2 == 42");
}

auto test_if_expression_selects_branch_value() -> void {
  auto jf = jit_fixture_for(load_fixture("if_expression.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 14, "expected abs(-7) + abs(7) == 14");
}

auto test_while_loop_sums_one_to_n() -> void {
  auto jf = jit_fixture_for(load_fixture("while_loop.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 55, "expected sum(1..=10) == 55");
}

auto test_recursive_call_computes_factorial() -> void {
  auto jf = jit_fixture_for(load_fixture("recursive_factorial.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 120, "expected factorial(5) == 120");
}

auto test_and_or_short_circuit_to_correct_value() -> void {
  auto jf = jit_fixture_for(load_fixture("and_or_short_circuit.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::boolean);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 1,
         "expected the combined classification to be true");
}

auto test_cast_widens_int_to_float() -> void {
  auto jf = jit_fixture_for(load_fixture("cast_int_to_float.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::f64);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.f == 21.0, "expected int32(21) as float64 == 21.0");
}

auto test_checked_add_panics_on_overflow_end_to_end() -> void {
  auto jf = jit_fixture_for(load_fixture("checked_add_overflow.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i8);
  expect(!result.has_value(), "expected int8 100+100 to overflow and panic");
  expect(result.error() == bc::panic_reason::integer_overflow,
         "expected the panic reason to be integer_overflow");
}

auto test_checked_div_panics_on_divide_by_zero() -> void {
  auto jf = jit_fixture_for(load_fixture("checked_div_by_zero.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(!result.has_value(), "expected division by zero to panic");
  expect(result.error() == bc::panic_reason::integer_divide_by_zero,
         "expected the panic reason to be integer_divide_by_zero");
}

auto test_string_literal_round_trips_through_the_heap() -> void {
  // Mirrors src/bytecode_compiler/compile_test.cpp's own str test exactly
  // (same fixture, same "hello" content, same {len; data} slot layout) —
  // `run_ptr_result` gives a real in-process pointer to the JIT's heap
  // value, just like the bytecode VM's own return value does, so both
  // tiers' str construction can be checked against the same expected bytes.
  auto jf = jit_fixture_for(load_fixture("string_literal.kira"));
  auto result = jf.jit.run_ptr_result("main");
  expect(result.has_value(), "expected main() to succeed");
  const auto *slots = reinterpret_cast<const bc::slot_value *>(
      static_cast<uintptr_t>(result->value.u));
  expect(slots[0].u == 5, "expected \"hello\" to report length 5");
  const auto *data =
      reinterpret_cast<const char *>(static_cast<uintptr_t>(slots[1].u));
  expect(std::string_view(data, 5) == "hello",
         "expected the str's data slot to point at \"hello\"'s bytes");
}

auto test_sum_type_variant_with_payload_encodes_tag_and_slot() -> void {
  // Mirrors src/bytecode_compiler/compile_test.cpp's own sum type test:
  // now that `match` is implemented, both tiers can decode the same
  // {tag; payload} heap layout for the same `make(3.5)` call and agree on
  // the tag/payload encoding, not just on "it compiles".
  auto jf = jit_fixture_for(load_fixture("sum_type_variant.kira"));
  auto result = jf.jit.run_ptr_result("main");
  expect(result.has_value(), "expected main() to succeed");
  const auto *slots = reinterpret_cast<const bc::slot_value *>(
      static_cast<uintptr_t>(result->value.u));
  expect(slots[0].i == 0, "expected @circle to encode as tag 0");
  expect(slots[1].f == 3.5, "expected the payload slot to hold 3.5");
}

auto test_sum_type_unit_variant_encodes_its_tag() -> void {
  auto jf = jit_fixture_for(load_fixture("sum_type_unit_variant.kira"));
  auto result = jf.jit.run_ptr_result("main");
  expect(result.has_value(), "expected main() to succeed");
  const auto *slots = reinterpret_cast<const bc::slot_value *>(
      static_cast<uintptr_t>(result->value.u));
  expect(slots[0].i == 1, "expected @empty to encode as tag 1");
}

auto test_calls_another_function_in_the_same_module() -> void {
  auto jf = jit_fixture_for(load_fixture("function_calls.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 25, "expected sum_of_squares(3, 4) == 25");
}

auto test_tuple_construction_and_projection() -> void {
  auto jf = jit_fixture_for(load_fixture("tuple_construction.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 42,
         "expected the tuple's three elements to sum to 42");
}

auto test_struct_literal_and_field_access() -> void {
  auto jf = jit_fixture_for(load_fixture("struct_field_access.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 42, "expected 18 + 24 == 42 via field access");
}

auto test_fixed_array_construction_and_indexing() -> void {
  auto jf = jit_fixture_for(load_fixture("fixed_array_indexing.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 30, "expected a[2] == 30");
}

auto test_array_index_out_of_bounds_panics() -> void {
  auto jf = jit_fixture_for(load_fixture("array_out_of_bounds.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(!result.has_value(), "expected a[5] on a 3-element array to panic");
  expect(result.error() == bc::panic_reason::index_out_of_bounds,
         "expected the panic reason to be index_out_of_bounds");
}

auto test_array_fill_form_repeats_the_same_value() -> void {
  auto jf = jit_fixture_for(load_fixture("array_fill.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 20, "expected four 5s to sum to 20");
}

auto test_match_dispatches_on_literal_and_wildcard_patterns() -> void {
  auto jf = jit_fixture_for(load_fixture("match_literal_wildcard.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 200, "expected classify(1) == 200");
}

auto test_match_or_pattern_matches_any_alternative() -> void {
  auto jf = jit_fixture_for(load_fixture("match_or_pattern.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::boolean);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 1, "expected is_small(2) && !is_small(5)");
}

auto test_match_range_pattern() -> void {
  auto jf = jit_fixture_for(load_fixture("match_range_pattern.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 2, "expected bucket(20) == 2");
}

auto test_match_guard_refines_a_pattern() -> void {
  // See bytecode_compiler/compile_test.cpp's own version of this test for
  // why the guard reads the enclosing parameter `x` rather than a
  // pattern-bound name (a known pre-existing lowering gap, out of scope
  // here).
  auto jf = jit_fixture_for(load_fixture("match_guard.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == -1, "expected sign(-7) == -1");
}

auto test_match_tuple_pattern_with_literal_and_binding() -> void {
  auto jf = jit_fixture_for(load_fixture("match_tuple_pattern.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 84,
         "expected describe(0, 42) + describe(18, 24) == 84");
}

auto test_match_constructor_pattern_over_a_sum_type() -> void {
  auto jf = jit_fixture_for(load_fixture("match_constructor_pattern.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::f64);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.f == 16.0,
         "expected area(@circle(4.0)) + area(@empty) == 16.0");
}

auto test_match_struct_pattern_destructures_named_fields() -> void {
  auto jf = jit_fixture_for(load_fixture("match_struct_pattern.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 42, "expected 18 + 24 == 42 via struct pattern");
}

auto test_list_literal_construction_and_indexing() -> void {
  auto jf = jit_fixture_for(load_fixture("list_literal_indexing.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 30, "expected xs[2] == 30");
}

auto test_list_index_out_of_bounds_panics() -> void {
  auto jf = jit_fixture_for(load_fixture("list_out_of_bounds.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(!result.has_value(), "expected xs[5] on a 3-element list to panic");
  expect(result.error() == bc::panic_reason::index_out_of_bounds,
         "expected the panic reason to be index_out_of_bounds");
}

auto test_list_fill_form_grows_to_a_runtime_count() -> void {
  auto jf = jit_fixture_for(load_fixture("list_fill.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 20,
         "expected four 5s (a runtime fill count) to sum to 20");
}

auto test_list_for_loop_sums_every_element() -> void {
  auto jf = jit_fixture_for(load_fixture("list_for_loop.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 15, "expected 1+2+3+4+5 == 15");
}

auto test_while_let_loops_until_the_pattern_stops_matching() -> void {
  auto jf = jit_fixture_for(load_fixture("while_let_pattern.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 10, "expected 0+1+2+3+4 == 10");
}

auto test_let_else_diverges_on_a_failed_pattern() -> void {
  auto jf = jit_fixture_for(load_fixture("let_else_pattern.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 6,
         "expected unwrap_or(@present(7)) + unwrap_or(@absent) == 7 + -1");
}

auto test_list_comprehension_builds_and_reads_back_a_list() -> void {
  auto jf = jit_fixture_for(load_fixture("list_comprehension.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 14, "expected 0^2+1^2+2^2+3^2 == 14");
}

auto test_closure_captures_an_outer_parameter_and_is_called_indirectly()
    -> void {
  auto jf = jit_fixture_for(load_fixture("closure_capture.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 8, "expected make_adder(5)(3) == 8");
}

auto test_non_capturing_closure_is_called_indirectly() -> void {
  auto jf = jit_fixture_for(load_fixture("closure_noncapture.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
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
    test_and_or_short_circuit_to_correct_value();
    test_cast_widens_int_to_float();
    test_checked_add_panics_on_overflow_end_to_end();
    test_checked_div_panics_on_divide_by_zero();
    test_string_literal_round_trips_through_the_heap();
    test_sum_type_variant_with_payload_encodes_tag_and_slot();
    test_sum_type_unit_variant_encodes_its_tag();
    test_calls_another_function_in_the_same_module();
    test_tuple_construction_and_projection();
    test_struct_literal_and_field_access();
    test_fixed_array_construction_and_indexing();
    test_array_index_out_of_bounds_panics();
    test_array_fill_form_repeats_the_same_value();
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
    std::cerr << "codegen_test failed: unhandled exception: " << ex.what()
              << '\n';
    std::exit(1);
  }
  return 0;
}
