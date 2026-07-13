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
#include "src/hir/link.h"
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

// Parses, checks, and lowers several files together as one session, then
// compiles the entry module (named by `entry_module_name`) plus every module
// `hir::find_reachable_modules` discovers it needs — the real
// parse -> check -> lower -> discover -> compile -> run pipeline for a
// program that calls across module boundaries.
auto compile_fixture_multi(
    const std::vector<std::pair<std::string, std::string>> &files,
    std::string_view entry_module_name) -> bc::bytecode_module {
  auto sources = kira::source_manager{};
  auto diag = kira::diagnostic_bag{};
  auto ast_files = std::vector<kira::ast::ptr<kira::ast::file>>{};
  auto parsed_modules = std::vector<kira::semantic::parsed_module>{};
  auto file_ids = std::vector<kira::file_id_type>{};

  for (const auto &[path, text] : files) {
    const auto file_id = sources.add_file(path, text);
    expect(file_id.has_value(), "expected fixture source to register");
    const auto *file = sources.get(*file_id);
    expect(file != nullptr, "expected registered fixture source");

    auto lexer = kira::lexer(file->source(), file->id(), diag);
    auto tokens = lexer.tokenize();
    auto parser = kira::parser(std::move(tokens), file->id(), diag);
    auto ast_file = parser.parse_file();
    expect(diag.error_count() == 0, "expected fixture to parse cleanly");

    file_ids.push_back(*file_id);
    ast_files.push_back(std::move(ast_file));
  }
  for (size_t i = 0; i < ast_files.size(); ++i) {
    parsed_modules.push_back(kira::semantic::parsed_module{
        .file_id = file_ids[i], .ast_file = ast_files[i].get()});
  }

  auto file_has_errors = std::vector<bool>(file_ids.size(), false);
  const auto checked =
      kira::semantic::check_program(parsed_modules, diag, file_has_errors);
  expect(diag.error_count() == 0, "expected fixture to check cleanly");

  auto modules = hir::ptr_vec<hir::hir_module>{};
  for (size_t i = 0; i < ast_files.size(); ++i) {
    auto lowered = hir::lower_module(*ast_files[i], files[i].first, checked);
    expect(lowered.has_value(), "expected every fixture module to lower");
    modules.push_back(std::move(*lowered));
  }

  const hir::hir_module *entry = nullptr;
  for (const auto &module : modules) {
    if (module->module_name == entry_module_name) {
      entry = module.get();
    }
  }
  expect(entry != nullptr, "expected to find the entry module by name");

  const auto reachable = hir::find_reachable_modules(*entry, modules);
  auto compiled = bcc::compile_module(reachable, checked.types);
  expect(compiled.has_value(),
         "expected the cross-module fixture to compile to bytecode");
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

auto test_intrinsic_call_compiles_to_op_call_intrinsic() -> void {
  // `intrinsic def rt_stdout() -> raw_fd` has no body — hir::lower_module
  // skips it, and this compiler recognizes the call by name (src/
  // intrinsics.h) and emits `op_call_intrinsic` instead of `op_call`. This
  // fixture proves that end to end: real Kira source, through name
  // resolution/typechecking, HIR lowering, bytecode compilation, and the
  // VM's native `rt_stdout` implementation (vm.cpp), which returns fd 1.
  auto module = compile_fixture(load_fixture("intrinsic_call.kira"));

  // `intrinsic def` never gets a `bytecode_function` entry of its own.
  for (const auto &fn : module.functions) {
    expect(fn.name != "rt_stdout",
           "expected `intrinsic def rt_stdout` not to compile to a "
           "bytecode_function");
  }

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  expect(main_result->value.i == 1,
         "expected main()'s rt_stdout().value to be 1");
}

auto test_intrinsic_result_constructs_and_matches_through_real_syntax()
    -> void {
  // Exercises the fix to `runtime::layout.cpp`'s `sum_variants_of`: before
  // it, `result[T, io_errno]` (a builtin generic, not a user-declared sum
  // type) had no variant/tag metadata, so `rt_open`'s `result[raw_fd,
  // io_errno]` return value could be typechecked but never actually
  // constructed or matched by this backend. This fixture opens a
  // guaranteed-missing file through the real `intrinsic def rt_open` and
  // real `match @ok(fd)/@err(e)` syntax, proving `@ok`/`@err` construction
  // and dispatch work end to end for a builtin generic, not just that the
  // metadata lookup returns something.
  //
  // Deliberately does not read a field off the bound payload (`e.code`):
  // that hits a separate, pre-existing gap — field access on a variant
  // payload binding fails to lower for *any* sum type (confirmed with a
  // plain user-declared sum type too), unrelated to this fix. Out of scope
  // here; `src/bytecode/vm_test.cpp`'s hand-assembled intrinsic tests
  // already exercise the `io_errno.code` payload directly without going
  // through that lowering path.
  auto module = compile_fixture(load_fixture("intrinsic_result.kira"));

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() not to panic");
  expect(main_result->value.i == 2,
         "expected main() to match the `@err(e)` arm for a missing file");
}

auto test_field_access_on_a_local_lowers_for_plain_lets_and_payload_bindings()
    -> void {
  // `x.field` parses as `ast::module_path_expr` whenever `x` is a bare
  // leading identifier (parser.cpp can't yet tell "value" from "module
  // path" without symbol info) — `hir::lower_expr` previously had no case
  // for that node kind at all, so it always failed with "expression kind
  // ... is not lowered by the first milestone", for *any* local, not just
  // a variant-payload binding. Fixed by `lowerer::lower_module_path`
  // (src/hir/lower.cpp), which resolves the ambiguity the same way
  // `semantic::check.cpp`'s `infer_module_path` already does for typing.
  auto module = compile_fixture(load_fixture("field_access_on_local.kira"));

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() not to panic");
  expect(main_result->value.i == 42,
         "expected field access on both a plain `let` and a variant "
         "payload binding to read the right value");
}

auto test_implicit_tail_match_and_if_are_the_return_value() -> void {
  // See src/llvm_codegen/codegen_test.cpp's identically-named test for the
  // full explanation — this is the same fixture run through the bytecode
  // backend instead, confirming the `hir::lower_block`/`check.cpp` fix
  // isn't LLVM-specific.
  auto module =
      compile_fixture(load_fixture("implicit_tail_match_and_if.kira"));

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  expect(main_result->value.i == 111222402,
         "expected classify(true)*1e6 + classify(false)*1e3 + grade(95)*1e2 "
         "+ grade_returns(72) == 111222402");
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

auto test_calls_a_function_in_another_module() -> void {
  auto module = compile_fixture_multi(
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
  auto main_result = run_main(module);
  expect(main_result.has_value(),
         "expected a call across module boundaries to run");
  expect(main_result->value.i == 42,
         "expected main() to return tools.double(21) == 42");
}

auto test_calls_an_associated_function_via_a_type_qualified_path() -> void {
  auto module = compile_fixture_multi(
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
  auto main_result = run_main(module);
  expect(main_result.has_value(),
         "expected a type-qualified associated-function call to run");
  expect(main_result->value.i == 21,
         "expected main() to return wrapped.from(7).value == 21");
}

// Exercises trait-default-method monomorphization (`semantic::check.cpp`'s
// `monomorphize_trait_default`) and its cross-module lowering
// (`hir::lower_module`'s `synthesized_trait_defaults` pass, `hir/lower.cpp`)
// end to end through the bytecode compiler and VM — `bump`'s body is never
// written in `impl bumpable for counter`, only inherited from the trait's
// default, and `bump` itself dispatches a further `self`-receiver call
// (`self.poke(1)`) to the real impl method. Nothing in the existing test
// suite compiles or runs a `self`-receiver method call of any kind (only
// type-checks it), so this is the first thing to prove the mechanism that
// `std.console.println` -> `std.io.file::write_all` -> `file::write` relies
// on actually produces runnable bytecode, in isolation from that much
// larger call graph.
auto test_calls_a_self_receiver_trait_default_method_across_modules() -> void {
  auto module = compile_fixture_multi(
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
  auto main_result = run_main(module);
  expect(main_result.has_value(),
         "expected a cross-module trait-default method call to run");
  expect(main_result->value.i == 42,
         "expected main() to return lib.make(41).bump() == 42");
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

auto test_packed_struct_field_access() -> void {
  auto module = compile_fixture(load_fixture("packed_struct_layout.kira"));
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "combine"),
                       std::array{bc::slot_value{uint64_t{7}},
                                  bc::slot_value{uint64_t{3}},
                                  bc::slot_value{uint64_t{42}}});
  expect(result.has_value(),
         "expected combine() on a packed struct to succeed");
  expect(result->value.i == 7'003'042,
         "expected each packed field to read back at its own value despite "
         "no padding between them");

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  expect(main_result->value.i == 7'003'042,
         "expected main()'s combine(7, 3, 42) == 7003042");
}

auto test_padded_struct_field_access() -> void {
  auto module = compile_fixture(load_fixture("padded_struct_layout.kira"));
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "combine"),
                       std::array{bc::slot_value{uint64_t{7}},
                                  bc::slot_value{uint64_t{3}},
                                  bc::slot_value{uint64_t{42}}});
  expect(result.has_value(),
         "expected combine() on a default-padded struct to succeed");
  expect(result->value.i == 7'003'042,
         "expected each padded field to still read back at its own value");
}

auto test_packed_struct_has_no_padding_in_memory() -> void {
  // `combine()` above only proves field *values* round-trip correctly,
  // which a padded layout would do identically — this test instead reads
  // the constructed struct's raw heap bytes directly to prove the
  // `packed` modifier actually removes alignment padding, not just that
  // reads/writes stay internally consistent.
  auto module = compile_fixture(load_fixture("packed_struct_layout.kira"));
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "make"),
                       std::array{bc::slot_value{uint64_t{0x1234}},
                                  bc::slot_value{uint64_t{0xAB}},
                                  bc::slot_value{uint64_t{0xDEADBEEF}}});
  expect(result.has_value(), "expected make() on a packed struct to succeed");
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
  auto module = compile_fixture(load_fixture("padded_struct_layout.kira"));
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "make"),
                       std::array{bc::slot_value{uint64_t{0x1234}},
                                  bc::slot_value{uint64_t{0xAB}},
                                  bc::slot_value{uint64_t{0xDEADBEEF}}});
  expect(result.has_value(), "expected make() on a padded struct to succeed");
  const auto *bytes = reinterpret_cast<const uint8_t *>(
      static_cast<uintptr_t>(result->value.u));
  // default (padded) layout: magic@0 (2 bytes), flags@2 (1 byte), one
  // padding byte at offset 3 so len lands on its own 4-byte alignment
  // boundary at offset 4 — 8 bytes total, unlike the packed layout's 7.
  expect(bytes[0] == 0x34 && bytes[1] == 0x12, "expected magic at offset 0");
  expect(bytes[2] == 0xAB, "expected flags at offset 2");
  expect(bytes[3] == 0x00,
         "expected a zero padding byte at offset 3 ahead of len's own "
         "4-byte alignment — a fresh arena allocation is zeroed, so an "
         "unused padding byte reads back as 0");
  expect(bytes[4] == 0xEF && bytes[5] == 0xBE && bytes[6] == 0xAD &&
             bytes[7] == 0xDE,
         "expected len at offset 4, after the alignment padding");
}

auto test_narrow_element_array_construction_and_indexing() -> void {
  auto module = compile_fixture(load_fixture("narrow_element_array.kira"));
  const auto vm = bc::vm{module};
  auto result = vm.run(
      function_index(module, "sum_all"),
      std::array{bc::slot_value{uint64_t{0}}, bc::slot_value{uint64_t{4}}});
  expect(result.has_value(), "expected sum_all(0, 4) to succeed");
  expect(result->value.i == 60,
         "expected the first and last 2-byte-wide array[int16,5] elements "
         "(10 + 50) to round-trip contiguously at their own natural width");

  auto main_result = run_main(module);
  expect(main_result.has_value(), "expected main() to succeed");
  expect(main_result->value.i == 60, "expected main()'s sum_all(0, 4) == 60");
}

auto test_narrow_element_array_has_no_padding_in_memory() -> void {
  // Mirrors `test_packed_struct_has_no_padding_in_memory` above: value-only
  // assertions can't distinguish a natural-stride `array[int16,5]` from one
  // that still wastes 8 bytes/element the old uniform-slot way — this reads
  // the constructed array's raw heap bytes directly.
  auto module = compile_fixture(load_fixture("narrow_element_array.kira"));
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "make_fixed"), {});
  expect(result.has_value(), "expected make_fixed() to succeed");
  const auto *bytes = reinterpret_cast<const uint8_t *>(
      static_cast<uintptr_t>(result->value.u));
  expect(bytes[0] == 0x11 && bytes[1] == 0x11,
         "expected element 0 (0x1111) at byte offset 0, little-endian");
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

auto test_violated_precondition_panics() -> void {
  auto module = compile_fixture(load_fixture("contract_violation.kira"));

  // `half(-4)` violates `pre x >= 0`, and the caller is opaque to the
  // checker, so the check is the callee's to make — and it fails.
  auto main_result = run_main(module);
  expect(!main_result.has_value(),
         "expected main()'s half(-8) to violate half's precondition");
  expect(main_result.error() == bc::panic_reason::precondition_violated,
         "expected the panic reason to name the broken precondition");

  // The same function, called with an argument that honours the contract,
  // runs to completion — the checks are guards, not a toll on every call.
  const auto vm = bc::vm{module};
  auto ok = vm.run(function_index(module, "half"),
                   std::array{bc::slot_value{int64_t{8}}});
  expect(ok.has_value(), "expected half(8) to satisfy its own contract");
  expect(ok->value.i == 4, "expected half(8) to return 4");
}

// Every exit is an explicit `return`, so each one carries the postcondition
// check itself — and the one that breaks the promise panics.
auto test_violated_postcondition_in_diverging_tail_panics() -> void {
  auto module = compile_fixture(load_fixture("contract_diverging_tail.kira"));
  auto main_result = run_main(module);
  expect(!main_result.has_value(),
         "expected broken_abs(-7) to violate its postcondition");
  expect(main_result.error() == bc::panic_reason::postcondition_violated,
         "expected the panic reason to name the broken postcondition");
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

// ==========================================================================
//  Generators — `generator def`/`yield`/`.next()` end to end. `counter`
//  compiles to two bytecode_functions (a constructor under its own name,
//  plus an internal step function reachable only via the generator object's
//  `step_function_index` slot); calling `counter(...)` and then `.next()`
//  repeatedly should produce `some(0), some(1), ..., none, none, ...`.
// ==========================================================================

auto option_tag_and_payload(bc::slot_value option_value)
    -> std::pair<int64_t, bc::slot_value> {
  const auto *slots = reinterpret_cast<const bc::slot_value *>(
      static_cast<uintptr_t>(option_value.u));
  return {slots[0].i, slots[1]};
}

auto test_generator_sequential_next_calls_yield_then_exhaust() -> void {
  auto module = compile_fixture("module sample\n"
                                "generator def counter() -> some "
                                "iterator[int32]:\n"
                                "  yield 1\n"
                                "  yield 2\n"
                                "def make() -> generator[int32]:\n"
                                "  return counter()\n"
                                "def next_of(g: generator[int32]) -> "
                                "option[int32]:\n"
                                "  return g.next()\n");
  const auto vm = bc::vm{module};
  auto gen_result = vm.run(function_index(module, "make"), {});
  expect(gen_result.has_value(), "expected make() to succeed");
  const auto gen = gen_result->value;

  auto first = vm.run(function_index(module, "next_of"), std::array{gen});
  expect(first.has_value(), "expected the first next() to succeed");
  auto [first_tag, first_payload] = option_tag_and_payload(first->value);
  expect(first_tag == 0, "expected the first next() to be some(...)");
  expect(first_payload.i == 1, "expected the first yielded value to be 1");

  auto second = vm.run(function_index(module, "next_of"), std::array{gen});
  expect(second.has_value(), "expected the second next() to succeed");
  auto [second_tag, second_payload] = option_tag_and_payload(second->value);
  expect(second_tag == 0, "expected the second next() to be some(...)");
  expect(second_payload.i == 2, "expected the second yielded value to be 2");

  auto third = vm.run(function_index(module, "next_of"), std::array{gen});
  expect(third.has_value(), "expected the third next() to succeed");
  auto [third_tag, third_payload] = option_tag_and_payload(third->value);
  (void)third_payload;
  expect(third_tag == 1, "expected the generator to be exhausted (none)");

  auto fourth = vm.run(function_index(module, "next_of"), std::array{gen});
  expect(fourth.has_value(), "expected a next() call after exhaustion to "
                             "still succeed");
  auto [fourth_tag, fourth_payload] = option_tag_and_payload(fourth->value);
  (void)fourth_payload;
  expect(fourth_tag == 1,
         "expected the generator to stay exhausted on further calls");
}

auto test_generator_loop_with_yield_sums_values() -> void {
  auto module = compile_fixture(
      "module sample\n"
      "generator def counter(limit: int32) -> some iterator[int32]:\n"
      "  var n = 0\n"
      "  while n < limit:\n"
      "    yield n\n"
      "    n = n + 1\n"
      "def sum_via_generator(limit: int32) -> int32:\n"
      "  let g = counter(limit)\n"
      "  var total = 0\n"
      "  while let @some(x) = g.next():\n"
      "    total = total + x\n"
      "  return total\n");
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "sum_via_generator"),
                       std::array{bc::slot_value{int64_t{5}}});
  expect(result.has_value(), "expected sum_via_generator(5) to succeed");
  expect(result->value.i == 10, "expected 0+1+2+3+4 == 10, the loop-"
                                "condition local surviving every yield");
}

auto test_generator_branch_yields_only_matching_values() -> void {
  auto module = compile_fixture(
      "module sample\n"
      "generator def evens(limit: int32) -> some iterator[int32]:\n"
      "  var n = 0\n"
      "  while n < limit:\n"
      "    if n % 2 == 0:\n"
      "      yield n\n"
      "    n = n + 1\n"
      "def sum_evens(limit: int32) -> int32:\n"
      "  let g = evens(limit)\n"
      "  var total = 0\n"
      "  while let @some(x) = g.next():\n"
      "    total = total + x\n"
      "  return total\n");
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "sum_evens"),
                       std::array{bc::slot_value{int64_t{10}}});
  expect(result.has_value(), "expected sum_evens(10) to succeed");
  expect(result->value.i == 20, "expected 0+2+4+6+8 == 20");
}

auto test_generator_bare_return_exhausts_early() -> void {
  auto module = compile_fixture("module sample\n"
                                "generator def early() -> some "
                                "iterator[int32]:\n"
                                "  yield 1\n"
                                "  yield 2\n"
                                "  return\n"
                                "  yield 3\n"
                                "def count_items() -> int32:\n"
                                "  let g = early()\n"
                                "  var count = 0\n"
                                "  while let @some(x) = g.next():\n"
                                "    count = count + 1\n"
                                "  return count\n");
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "count_items"), {});
  expect(result.has_value(), "expected count_items() to succeed");
  expect(result->value.i == 2,
         "expected a bare `return` to exhaust the generator before the "
         "unreachable third yield");
}

auto test_generator_nonzero_initial_locals_survive_resume() -> void {
  // Regression test: a generator-state local's own `var`/`let` binding
  // must actually initialize the *same* register the state-preload step
  // already reserved for it, not a second, orphaned one — `locals_.
  // emplace`'s silent no-op on an already-present key previously let a
  // nonzero initializer (like `var a = 1` here) get computed and then
  // discarded, leaving every read of `a`/`b` see the state block's
  // zero-initialized default forever. `n`-style `var n = 0` locals never
  // exposed this, since the correct and the (bugged) discarded-initializer
  // values were coincidentally identical.
  auto module = compile_fixture(
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
      "def sum_pairs(count: int32) -> int32:\n"
      "  let g = pairs(count)\n"
      "  var total = 0\n"
      "  while let @some(x) = g.next():\n"
      "    total = total + x\n"
      "  return total\n");
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "sum_pairs"),
                       std::array{bc::slot_value{int64_t{3}}});
  expect(result.has_value(), "expected sum_pairs(3) to succeed");
  // a: 1,2,3  b: 100,101,102 -> (1+2+3)+(100+101+102) = 6+303 = 309
  expect(result->value.i == 309,
         "expected both generator-state locals to keep their real, "
         "nonzero initial values across every resume");
}

auto test_for_loop_over_generator_sums_values() -> void {
  auto module = compile_fixture(
      "module sample\n"
      "generator def counter(limit: int32) -> some iterator[int32]:\n"
      "  var n = 0\n"
      "  while n < limit:\n"
      "    yield n\n"
      "    n = n + 1\n"
      "def sum_via_for(limit: int32) -> int32:\n"
      "  var total = 0\n"
      "  for x in counter(limit):\n"
      "    total = total + x\n"
      "  return total\n");
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "sum_via_for"),
                       std::array{bc::slot_value{int64_t{5}}});
  expect(result.has_value(), "expected sum_via_for(5) to succeed");
  expect(result->value.i == 10, "expected 0+1+2+3+4 == 10");
}

auto test_for_loop_over_generator_evaluates_iterable_once() -> void {
  // The generator handle (`counter()`, a fresh generator each call) must be
  // evaluated exactly once by the `for` loop, not re-invoked every
  // iteration — otherwise the loop would restart from scratch each pass
  // and never terminate (or, since a fresh generator's `n` always starts
  // at 0 < limit, spin forever). A finite, correct sum proves the loop
  // used one single generator instance throughout.
  auto module = compile_fixture(
      "module sample\n"
      "generator def counter(limit: int32) -> some iterator[int32]:\n"
      "  var n = 0\n"
      "  while n < limit:\n"
      "    yield n\n"
      "    n = n + 1\n"
      "def sum_via_for() -> int32:\n"
      "  var total = 0\n"
      "  for x in counter(4):\n"
      "    total = total + x\n"
      "  return total\n");
  const auto vm = bc::vm{module};
  auto result = vm.run(function_index(module, "sum_via_for"), {});
  expect(result.has_value(), "expected sum_via_for() to succeed");
  expect(result->value.i == 6, "expected 0+1+2+3 == 6");
}

} // namespace

auto main() -> int {
  try {
    test_add_compiles_and_runs();
    test_intrinsic_call_compiles_to_op_call_intrinsic();
    test_intrinsic_result_constructs_and_matches_through_real_syntax();
    test_field_access_on_a_local_lowers_for_plain_lets_and_payload_bindings();
    test_implicit_tail_match_and_if_are_the_return_value();
    test_implicit_tail_expression_is_the_return_value();
    test_if_expression_selects_branch_value();
    test_while_loop_sums_one_to_n();
    test_recursive_call_computes_factorial();
    test_calls_another_function_in_the_same_module();
    test_calls_a_function_in_another_module();
    test_calls_an_associated_function_via_a_type_qualified_path();
    test_calls_a_self_receiver_trait_default_method_across_modules();
    test_and_or_short_circuit_to_correct_value();
    test_cast_widens_int_to_float();
    test_checked_add_panics_on_overflow_end_to_end();
    test_checked_div_panics_on_divide_by_zero_end_to_end();
    test_string_literal_len_reads_the_heap_header();
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
    test_violated_precondition_panics();
    test_violated_postcondition_in_diverging_tail_panics();
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
    test_generator_sequential_next_calls_yield_then_exhaust();
    test_generator_loop_with_yield_sums_values();
    test_generator_branch_yields_only_matching_values();
    test_generator_bare_return_exhausts_early();
    test_generator_nonzero_initial_locals_survive_resume();
    test_for_loop_over_generator_sums_values();
    test_for_loop_over_generator_evaluates_iterable_once();
  } catch (const std::exception &ex) {
    std::cerr << "compile_test failed: unhandled exception: " << ex.what()
              << '\n';
    std::exit(1);
  }
  return 0;
}
