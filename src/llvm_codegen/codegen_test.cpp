#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "src/bytecode/panic.h"
#include "src/bytecode/value.h"
#include "src/hir/link.h"
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

// Mirrors src/bytecode_compiler/compile_test.cpp's own
// `compile_fixture_multi`: parses/checks/lowers several files as one
// session, then compiles the entry module (named `entry_module_name`) plus
// every module `hir::find_reachable_modules` discovers it needs.
auto jit_fixture_for_multi(
    const std::vector<std::pair<std::string, std::string>> &files,
    std::string_view entry_module_name) -> jit_fixture {
  auto fixture = checked_fixture{};
  auto ast_files = std::vector<kira::ast::ptr<kira::ast::file>>{};
  auto parsed_modules = std::vector<kira::semantic::parsed_module>{};
  auto file_ids = std::vector<kira::file_id_type>{};

  for (const auto &[path, text] : files) {
    const auto file_id = fixture.sources.add_file(path, text);
    expect(file_id.has_value(), "expected fixture source to register");
    const auto *file = fixture.sources.get(*file_id);
    expect(file != nullptr, "expected registered fixture source");

    auto lexer = kira::lexer(file->source(), file->id(), fixture.diag);
    auto tokens = lexer.tokenize();
    auto parser = kira::parser(std::move(tokens), file->id(), fixture.diag);
    auto ast_file = parser.parse_file();
    expect(fixture.diag.error_count() == 0,
           "expected fixture to parse cleanly");

    file_ids.push_back(*file_id);
    ast_files.push_back(std::move(ast_file));
  }
  for (size_t i = 0; i < ast_files.size(); ++i) {
    parsed_modules.push_back(kira::semantic::parsed_module{
        .file_id = file_ids[i], .ast_file = ast_files[i].get()});
  }

  auto file_has_errors = std::vector<bool>(file_ids.size(), false);
  fixture.checked = kira::semantic::check_program(parsed_modules, fixture.diag,
                                                  file_has_errors);
  expect(fixture.diag.error_count() == 0, "expected fixture to check cleanly");

  auto modules = hir::ptr_vec<hir::hir_module>{};
  for (size_t i = 0; i < files.size(); ++i) {
    auto lowered =
        hir::lower_module(*ast_files[i], files[i].first, fixture.checked);
    expect(lowered.has_value(), "expected every fixture module to lower");
    modules.push_back(std::move(*lowered));
  }
  fixture.ast_file = std::move(ast_files.front());

  const hir::hir_module *entry = nullptr;
  for (const auto &module : modules) {
    if (module->module_name == entry_module_name) {
      entry = module.get();
    }
  }
  expect(entry != nullptr, "expected to find the entry module by name");

  const auto reachable = hir::find_reachable_modules(*entry, modules);
  auto compiled = lc::compile_module(reachable, fixture.checked.types);
  expect(compiled.has_value(),
         "expected the cross-module fixture to compile to an llvm::Module");
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

auto test_intrinsic_call_resolves_to_native_symbol() -> void {
  // `intrinsic def rt_stdout() -> raw_fd` has no body — `compile_call`
  // recognizes the call by name (src/intrinsics.h) and emits a call to the
  // `kira_rt_stdout` C-ABI symbol (declared in `compile_module`,
  // implemented in `src/runtime/io.cpp`) instead of failing with
  // `unknown_callee`. JIT-resolved here via `//src/runtime:runtime`
  // (`:jit_support`'s real `deps`, not just `data`) linking `io.cpp`'s
  // `alwayslink`'d object into this test binary's own process.
  auto jf = jit_fixture_for(load_fixture("intrinsic_call.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i64);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 1, "expected main()'s rt_stdout().value to be 1");
}

auto test_intrinsic_result_constructs_and_matches_through_real_syntax()
    -> void {
  // The LLVM-tier counterpart of
  // src/bytecode_compiler/compile_test.cpp's identically-named test: opens
  // a guaranteed-missing file through the real `intrinsic def rt_open` and
  // real `match @ok(fd)/@err(e)` syntax, proving `kira_rt_open`'s
  // `result[raw_fd, io_errno]` heap-pointer return value round-trips
  // through this backend's own sum-type construction/matching codegen.
  auto jf = jit_fixture_for(load_fixture("intrinsic_result.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() not to panic");
  expect(result->value.i == 2,
         "expected main() to match the `@err(e)` arm for a missing file");
}

auto test_implicit_tail_match_and_if_are_the_return_value() -> void {
  // `match`/`if` used as a bare *statement*-shaped tail (no explicit
  // `return`, no assignment — as opposed to `if_expression.kira`'s `return
  // if ...`) previously lowered with `k_unknown_type` and was never
  // wrapped as a value-producing tail, so it silently returned 0/unit
  // instead of the matched/branch value (src/hir/lower.cpp's
  // `lower_block`/`lower_tail_control_flow_stmt`, and — for `if` only —
  // src/semantic/check.cpp's `if_stmt` case, which previously always typed
  // `unit` unlike `match_stmt`). Also exercises the case where every
  // branch diverges via `return` (`grade_returns`), which separately
  // exposed a `llvm_codegen` bug: it unconditionally loaded/stored/
  // returned a value after `compile_if`/`compile_match` already closed the
  // block with `unreachable`, tripping LLVM's verifier
  // ("Terminator found in the middle of a basic block").
  auto jf = jit_fixture_for(load_fixture("implicit_tail_match_and_if.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 111222402,
         "expected classify(true)*1e6 + classify(false)*1e3 + grade(95)*1e2 "
         "+ grade_returns(72) == 111222402");
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

auto test_calls_a_function_in_another_module() -> void {
  auto jf = jit_fixture_for_multi(
      {
          {"tools", "module tools\n"
                    "pub def double(x: int32) -> int32:\n"
                    "    return x * 2\n"},
          {"app", "module app\n"
                  "use tools\n"
                  "pub def main() -> int32:\n"
                  "    return tools.double(21)\n"},
      },
      "app");
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected a call across module boundaries to run");
  expect(result->value.i == 42,
         "expected main() to return tools.double(21) == 42");
}

auto test_calls_an_associated_function_via_a_type_qualified_path() -> void {
  auto jf = jit_fixture_for_multi(
      {
          {"app", "module app\n"
                  "pub trait from[T]:\n"
                  "    def from(value: T) -> self\n"
                  "pub type wrapped = { pub value: int32 }\n"
                  "impl from[int32] for wrapped:\n"
                  "    def from(x: int32) -> wrapped:\n"
                  "        return wrapped{ value: x * 3 }\n"
                  "pub def main() -> int32:\n"
                  "    return wrapped.from(7).value\n"},
      },
      "app");
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(),
         "expected a type-qualified associated-function call to run");
  expect(result->value.i == 21,
         "expected main() to return wrapped.from(7).value == 21");
}

// Mirrors src/bytecode_compiler/compile_test.cpp's
// `test_calls_a_self_receiver_trait_default_method_across_modules` exactly,
// on the LLVM/AOT side: proves trait-default-method monomorphization
// (`semantic::check.cpp`'s `monomorphize_trait_default`) and its
// cross-module lowering (`synthesized_trait_defaults`, `hir/lower.cpp`)
// produce IR the JIT can actually run, not just bytecode the VM can — no
// existing test compiled a `self`-receiver method call through this
// backend before.
auto test_calls_a_self_receiver_trait_default_method_across_modules() -> void {
  auto jf = jit_fixture_for_multi(
      {
          {"lib", "module lib\n"
                  "pub type counter = { n: int32 }\n"
                  "pub trait bumpable:\n"
                  "    def peek(mut self) -> int32\n"
                  "\n"
                  "    def bump(mut self) -> int32:\n"
                  "        return self.peek() + 1\n"
                  "impl bumpable for counter:\n"
                  "    def peek(mut self) -> int32:\n"
                  "        return self.n\n"
                  "pub def make(start: int32) -> counter:\n"
                  "    return counter { n: start }\n"},
          {"app", "module app\n"
                  "use lib\n"
                  "pub def main() -> int32:\n"
                  "    var c = lib.make(41)\n"
                  "    return c.bump()\n"},
      },
      "app");
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(),
         "expected a cross-module trait-default method call to run");
  expect(result->value.i == 42,
         "expected main() to return lib.make(41).bump() == 42");
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

auto test_packed_struct_field_access() -> void {
  auto jf = jit_fixture_for(load_fixture("packed_struct_layout.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i64);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 7'003'042,
         "expected combine(7, 3, 42) == 7003042 via a packed struct");
}

auto test_padded_struct_field_access() -> void {
  auto jf = jit_fixture_for(load_fixture("padded_struct_layout.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i64);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 7'003'042,
         "expected combine(7, 3, 42) == 7003042 via a default-padded struct");
}

auto test_packed_struct_has_no_padding_in_memory() -> void {
  // Mirrors src/bytecode_compiler/compile_test.cpp's own test exactly —
  // `combine()` above only proves field *values* round-trip correctly,
  // which a padded layout would do identically. This reads the JIT's own
  // constructed struct's raw heap bytes directly, proving `packed`
  // actually removes alignment padding in the LLVM-compiled tier too, not
  // just the bytecode VM.
  auto jf = jit_fixture_for(load_fixture("packed_struct_layout.kira"));
  auto result = jf.jit.run_ptr_result("make_fixed");
  expect(result.has_value(), "expected make_fixed() to succeed");
  const auto *bytes = reinterpret_cast<const uint8_t *>(
      static_cast<uintptr_t>(result->value.u));
  // packed: magic@0 (2 bytes, LE), flags@2 (1 byte), len@3 (4 bytes, LE) —
  // back to back, no padding, 7 bytes total.
  expect(bytes[0] == 0x34 && bytes[1] == 0x12,
         "expected magic at offset 0, little-endian");
  expect(bytes[2] == 0xAB,
         "expected flags immediately after magic at offset 2");
  expect(bytes[3] == 0xEF && bytes[4] == 0xBE && bytes[5] == 0xAD &&
             bytes[6] == 0xDE,
         "expected len packed immediately after flags at offset 3, with no "
         "padding inserted for its own 4-byte alignment");
}

auto test_padded_struct_has_alignment_padding_in_memory() -> void {
  auto jf = jit_fixture_for(load_fixture("padded_struct_layout.kira"));
  auto result = jf.jit.run_ptr_result("make_fixed");
  expect(result.has_value(), "expected make_fixed() to succeed");
  const auto *bytes = reinterpret_cast<const uint8_t *>(
      static_cast<uintptr_t>(result->value.u));
  // default (padded) layout: magic@0 (2 bytes), flags@2 (1 byte), one
  // padding byte at offset 3 so len lands on its own 4-byte alignment
  // boundary at offset 4 — 8 bytes total, unlike the packed layout's 7.
  expect(bytes[0] == 0x34 && bytes[1] == 0x12, "expected magic at offset 0");
  expect(bytes[2] == 0xAB, "expected flags at offset 2");
  expect(bytes[3] == 0x00,
         "expected a zero padding byte at offset 3 ahead of len's own "
         "4-byte alignment — a fresh allocation is zeroed, so an unused "
         "padding byte reads back as 0");
  expect(bytes[4] == 0xEF && bytes[5] == 0xBE && bytes[6] == 0xAD &&
             bytes[7] == 0xDE,
         "expected len at offset 4, after the alignment padding");
}

auto test_narrow_element_array_construction_and_indexing() -> void {
  auto jf = jit_fixture_for(load_fixture("narrow_element_array.kira"));
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 60,
         "expected the first and last array[int16,5] elements (10 + 50) to "
         "round-trip");
}

auto test_narrow_element_array_has_no_padding_in_memory() -> void {
  // Value-only assertions (like `test_narrow_element_array_construction_and_
  // indexing` above) can't tell a natural-stride `array[int16,5]` apart from
  // one that wastes 8 bytes/element the way `slot_address`-based storage
  // used to — this reads the JIT's own constructed array's raw heap bytes
  // directly to prove each `int16` element is packed at exactly a 2-byte
  // stride with no gap, mirroring the packed-struct memory-layout tests
  // above.
  auto jf = jit_fixture_for(load_fixture("narrow_element_array.kira"));
  auto result = jf.jit.run_ptr_result("make_fixed");
  expect(result.has_value(), "expected make_fixed() to succeed");
  const auto *bytes = reinterpret_cast<const uint8_t *>(
      static_cast<uintptr_t>(result->value.u));
  expect(bytes[0] == 0x11 && bytes[1] == 0x11, "expected element 0 (0x1111) "
                                               "at byte offset 0, little-"
                                               "endian");
  expect(bytes[2] == 0x22 && bytes[3] == 0x22,
         "expected element 1 (0x2222) immediately after at offset 2, no "
         "padding to an 8-byte slot");
  expect(bytes[4] == 0x33 && bytes[5] == 0x33,
         "expected element 2 (0x3333) at offset 4");
  expect(bytes[6] == 0x44 && bytes[7] == 0x44,
         "expected element 3 (0x4444) at offset 6");
  expect(bytes[8] == 0x55 && bytes[9] == 0x55,
         "expected element 4 (0x5555) at offset 8 — 10 bytes total for 5 "
         "`int16` elements, not 40");
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

// ==========================================================================
//  Generators — `generator def`/`yield`/`.next()` end to end. Mirrors
//  src/bytecode_compiler/compile_test.cpp's identically-named generator
//  tests, driving the exact same source through this backend instead — a
//  generator's declared name compiles to a constructor `llvm::Function`
//  under that name (calling it is an ordinary `run(...)`), with the actual
//  body compiled separately into an internal-linkage step function reached
//  only through the generator object's own slot.
// ==========================================================================

auto test_generator_loop_with_yield_sums_values() -> void {
  auto jf = jit_fixture_for(
      "module sample\n"
      "generator def counter(limit: int32) -> some iterator[int32]:\n"
      "  var n = 0\n"
      "  while n < limit:\n"
      "    yield n\n"
      "    n = n + 1\n"
      "def main() -> int32:\n"
      "  let g = counter(5)\n"
      "  var total = 0\n"
      "  while let @some(x) = g.next():\n"
      "    total = total + x\n"
      "  return total\n");
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 10, "expected 0+1+2+3+4 == 10, the loop-"
                                "condition local surviving every yield");
}

auto test_generator_branch_yields_only_matching_values() -> void {
  auto jf = jit_fixture_for(
      "module sample\n"
      "generator def evens(limit: int32) -> some iterator[int32]:\n"
      "  var n = 0\n"
      "  while n < limit:\n"
      "    if n % 2 == 0:\n"
      "      yield n\n"
      "    n = n + 1\n"
      "def main() -> int32:\n"
      "  let g = evens(10)\n"
      "  var total = 0\n"
      "  while let @some(x) = g.next():\n"
      "    total = total + x\n"
      "  return total\n");
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 20, "expected 0+2+4+6+8 == 20");
}

auto test_generator_bare_return_exhausts_early() -> void {
  auto jf = jit_fixture_for("module sample\n"
                            "generator def early() -> some "
                            "iterator[int32]:\n"
                            "  yield 1\n"
                            "  yield 2\n"
                            "  return\n"
                            "  yield 3\n"
                            "def main() -> int32:\n"
                            "  let g = early()\n"
                            "  var count = 0\n"
                            "  while let @some(x) = g.next():\n"
                            "    count = count + 1\n"
                            "  return count\n");
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 2,
         "expected a bare `return` to exhaust the generator before the "
         "unreachable third yield");
}

auto test_generator_next_after_exhaustion_stays_none() -> void {
  auto jf = jit_fixture_for("module sample\n"
                            "generator def counter() -> some iterator[int32]:\n"
                            "  yield 1\n"
                            "def main() -> int32:\n"
                            "  let g = counter()\n"
                            "  var total = 0\n"
                            "  while let @some(x) = g.next():\n"
                            "    total = total + x\n"
                            "  var extra = 0\n"
                            "  while let @some(x) = g.next():\n"
                            "    extra = extra + 1000\n"
                            "  return total + extra\n");
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 1,
         "expected the generator to stay exhausted on further next() calls "
         "after the first loop drained it");
}

auto test_generator_nonzero_initial_locals_survive_resume() -> void {
  // LLVM-tier counterpart of
  // src/bytecode_compiler/compile_test.cpp's identically-named regression
  // test — see its doc comment for the bug this guards against.
  auto jf = jit_fixture_for(
      "module sample\n"
      "generator def pairs(count: int32) -> some iterator[int32]:\n"
      "  var a = 1\n"
      "  var b = 100\n"
      "  var i = 0\n"
      "  while i < count:\n"
      "    yield a\n"
      "    yield b\n"
      "    a = a + 1\n"
      "    b = b + 1\n"
      "    i = i + 1\n"
      "def main() -> int32:\n"
      "  let g = pairs(3)\n"
      "  var total = 0\n"
      "  while let @some(x) = g.next():\n"
      "    total = total + x\n"
      "  return total\n");
  auto result = jf.jit.run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected main() to succeed");
  expect(result->value.i == 309,
         "expected both generator-state locals to keep their real, "
         "nonzero initial values across every resume");
}

// ==========================================================================
//  Optimization (`llvm_codegen::optimization_level`/`optimize_module`) —
//  these build the same fixture at `-O2` (via `jit_module::create`'s
//  optional `level` parameter) as an already-passing `-O0` test above, and
//  assert the exact same result, proving `llvm::PassBuilder`'s default
//  pipeline doesn't change program semantics for constructs this backend
//  actually emits: recursion/control flow, and a real runtime panic still
//  firing (not folded away) through checked-arithmetic overflow.
// ==========================================================================

auto test_recursive_call_computes_factorial_at_o2() -> void {
  auto fixture = check_fixture(load_fixture("recursive_factorial.kira"));
  auto module = hir::lower_module(*fixture.ast_file, "sample", fixture.checked);
  expect(module.has_value(), "expected fixture to lower to HIR");
  auto compiled = lc::compile_module(**module, fixture.checked.types);
  expect(compiled.has_value(),
         "expected fixture to compile to an llvm::Module");
  auto jit =
      lc::jit_module::create(std::move(*compiled), lc::optimization_level::o2);
  expect(jit.has_value(),
         "expected the -O2 compiled module to JIT successfully");

  auto result = jit->run("main", bc::numeric_kind::i32);
  expect(result.has_value(), "expected -O2's main() to succeed");
  expect(result->value.i == 120, "expected -O2's factorial(5) == 120, matching "
                                 "the -O0 result exactly");
}

auto test_checked_add_still_panics_on_overflow_at_o2() -> void {
  auto fixture = check_fixture(load_fixture("checked_add_overflow.kira"));
  auto module = hir::lower_module(*fixture.ast_file, "sample", fixture.checked);
  expect(module.has_value(), "expected fixture to lower to HIR");
  auto compiled = lc::compile_module(**module, fixture.checked.types);
  expect(compiled.has_value(),
         "expected fixture to compile to an llvm::Module");
  auto jit =
      lc::jit_module::create(std::move(*compiled), lc::optimization_level::o2);
  expect(jit.has_value(),
         "expected the -O2 compiled module to JIT successfully");

  auto result = jit->run("main", bc::numeric_kind::i32);
  expect(!result.has_value(),
         "expected -O2's overflow panic to still fire, not be optimized away");
  expect(result.error() == bc::panic_reason::integer_overflow,
         "expected -O2's panic reason to still be integer_overflow");
}

auto test_parse_optimization_level_accepts_0_through_3_only() -> void {
  expect(lc::parse_optimization_level("0") == lc::optimization_level::o0,
         "expected \"0\" to parse as o0");
  expect(lc::parse_optimization_level("1") == lc::optimization_level::o1,
         "expected \"1\" to parse as o1");
  expect(lc::parse_optimization_level("2") == lc::optimization_level::o2,
         "expected \"2\" to parse as o2");
  expect(lc::parse_optimization_level("3") == lc::optimization_level::o3,
         "expected \"3\" to parse as o3");
  expect(!lc::parse_optimization_level("4").has_value(),
         "expected an out-of-range level to fail to parse");
  expect(!lc::parse_optimization_level("s").has_value(),
         "expected a non-numeric level to fail to parse");
}

} // namespace

auto main() -> int {
  try {
    test_add_compiles_and_runs();
    test_implicit_tail_expression_is_the_return_value();
    test_if_expression_selects_branch_value();
    test_intrinsic_call_resolves_to_native_symbol();
    test_intrinsic_result_constructs_and_matches_through_real_syntax();
    test_implicit_tail_match_and_if_are_the_return_value();
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
    test_calls_a_function_in_another_module();
    test_calls_an_associated_function_via_a_type_qualified_path();
    test_calls_a_self_receiver_trait_default_method_across_modules();
    test_tuple_construction_and_projection();
    test_struct_literal_and_field_access();
    test_fixed_array_construction_and_indexing();
    test_packed_struct_field_access();
    test_padded_struct_field_access();
    test_packed_struct_has_no_padding_in_memory();
    test_padded_struct_has_alignment_padding_in_memory();
    test_narrow_element_array_construction_and_indexing();
    test_narrow_element_array_has_no_padding_in_memory();
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
    test_generator_loop_with_yield_sums_values();
    test_generator_branch_yields_only_matching_values();
    test_generator_bare_return_exhausts_early();
    test_generator_next_after_exhaustion_stays_none();
    test_generator_nonzero_initial_locals_survive_resume();
    test_recursive_call_computes_factorial_at_o2();
    test_checked_add_still_panics_on_overflow_at_o2();
    test_parse_optimization_level_accepts_0_through_3_only();
  } catch (const std::exception &ex) {
    std::cerr << "codegen_test failed: unhandled exception: " << ex.what()
              << '\n';
    std::exit(1);
  }
  return 0;
}
