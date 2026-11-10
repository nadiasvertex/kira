// Bytecode compiled from the shared storage form: results are checked against
// values worked out by hand, not against the pointer-per-aggregate compiler.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "src/bytecode/chunk.h"
#include "src/bytecode/opcodes.h"
#include "src/bytecode/panic.h"
#include "src/bytecode/vm.h"
#include "src/bytecode_compiler/storage_compile.h"
#include "src/comptime_vm/backend.h"
#include "src/hir/lower.h"
#include "src/hir/storage.h"
#include "src/hir/storage_lower.h"
#include "src/hir/storage_verify.h"
#include "src/parser/diagnostic.h"
#include "src/parser/lexer.h"
#include "src/parser/parser.h"
#include "src/parser/source_location.h"
#include "src/runtime/allocator.h"
#include "src/runtime/value_layout.h"
#include "src/semantic/analysis.h"
#include "src/semantic/check.h"
#include "src/testing/stdlib_fixtures.h"
#include "src/testing/test_assert.h"
#include "src/testing/test_data.h"

namespace {

using cinder::testing::expect;
using cinder::testing::fail;
namespace bytecode = cinder::bytecode;
namespace hir = cinder::hir;
namespace runtime = cinder::runtime;
namespace semantic = cinder::semantic;
using bytecode::slot_value;

auto read_program(std::string_view dir, std::string_view name) -> std::string {
  const auto path = cinder::testing::find_test_data_dir(dir);
  return cinder::testing::load_test_data_file(path.string(), name);
}

/// A checked program, lowered to storage form, compiled, and ready to run.
struct program {
  cinder::source_manager sources;
  cinder::diagnostic_bag diag{};
  cinder::testing::parsed_stdlib stdlib;
  cinder::ast::ptr<cinder::ast::file> ast_file;
  semantic::checked_types checked;
  hir::ptr<hir::hir_module> module;
  std::unique_ptr<runtime::value_layouts> layouts;
  std::unique_ptr<hir::storage_env> env;
  hir::storage_lowering lowering;
  bytecode::bytecode_module code;
  std::unique_ptr<bytecode::vm> machine;
  std::unordered_map<std::string, uint16_t> index;

  [[nodiscard]] auto find(std::string_view name) const -> uint16_t {
    const auto found = index.find(std::string(name));
    if (found == index.end()) {
      std::cerr << "missing function: " << name << "\n";
      fail("expected the function to be compiled");
    }
    return found->second;
  }

  [[nodiscard]] auto function(std::string_view name) const
      -> const bytecode::bytecode_function & {
    return code.functions[find(name)];
  }
};

auto build(const std::string &text) -> std::unique_ptr<program> {
  auto out = std::make_unique<program>();
  out->stdlib = cinder::testing::parse_stdlib(out->sources, out->diag);
  const auto file_id = out->sources.add_file("sample.cn", text);
  expect(file_id.has_value(), "expected fixture source to register");
  const auto *file = out->sources.get(*file_id);
  auto lexer = cinder::lexer(file->source(), file->id(), out->diag);
  auto tokens = lexer.tokenize();
  auto parser = cinder::parser(std::move(tokens), file->id(), out->diag);
  out->ast_file = parser.parse_file();
  if (out->diag.error_count() != 0) {
    std::cerr << cinder::diagnostic_renderer(out->sources, false)
                     .render_all(out->diag);
    fail("expected fixture to parse cleanly");
  }

  auto file_has_errors =
      std::vector<bool>(static_cast<size_t>(*file_id) + 1, false);
  auto parsed_modules = out->stdlib.modules;
  parsed_modules.push_back(semantic::parsed_module{
      .file_id = *file_id, .ast_file = out->ast_file.get()});
  auto comptime = cinder::comptime_vm::vm_backend{};
  out->checked = semantic::check_program(parsed_modules, out->diag,
                                         file_has_errors, &comptime);
  if (out->diag.error_count() != 0) {
    std::cerr << cinder::diagnostic_renderer(out->sources, false)
                     .render_all(out->diag);
    fail("expected fixture to check cleanly");
  }
  auto lowered = hir::lower_module(*out->ast_file, "sample", out->checked);
  expect(lowered.has_value(), "expected the fixture to lower to HIR");
  out->module = std::move(*lowered);
  out->layouts = std::make_unique<runtime::value_layouts>(out->checked.types,
                                                          out->checked.shapes);
  out->env = std::make_unique<hir::storage_env>(
      hir::storage_env{out->checked, *out->layouts});
  out->lowering = hir::lower_to_storage(*out->module, *out->env);
  for (const auto &refused : out->lowering.refused) {
    std::cerr << "refused " << refused.name << ": " << refused.error.message
              << "\n";
  }
  expect(out->lowering.refused.empty(),
         "expected every fixture function to lower to storage form");
  const auto verdict = hir::verify(out->lowering.module, *out->env);
  if (!verdict.has_value()) {
    std::cerr << verdict.error().function << ": " << verdict.error().message
              << "\n";
    fail("expected the storage form to verify");
  }
  auto compiled =
      cinder::bytecode_compiler::compile_storage_module(out->lowering.module,
                                                        *out->env);
  if (!compiled.has_value()) {
    std::cerr << compiled.error().message << "\n";
    fail("expected the storage form to compile to bytecode");
  }
  out->code = std::move(*compiled);
  for (size_t i = 0; i < out->code.functions.size(); ++i) {
    out->index.emplace(out->code.functions[i].name, static_cast<uint16_t>(i));
  }
  out->machine = std::make_unique<bytecode::vm>(out->code);
  return out;
}

auto integer(int64_t value) -> slot_value { return slot_value{value}; }

/// Runs a function that returns a scalar.
auto run_scalar(const program &p, std::string_view name,
                std::initializer_list<int64_t> args) -> int64_t {
  auto registers = std::vector<slot_value>{};
  for (const auto arg : args) {
    registers.push_back(integer(arg));
  }
  const auto result = p.machine->run(p.find(name), registers);
  if (!result.has_value()) {
    std::cerr << name << " panicked: "
              << bytecode::panic_reason_message(result.error()) << "\n";
    fail("expected the function to return");
  }
  expect(result->has_value, "expected a returned value");
  return result->value.i;
}

/// Runs a function that returns a scalar and reports the panic it ended in.
auto run_panics(const program &p, std::string_view name,
                std::initializer_list<int64_t> args)
    -> std::optional<bytecode::panic_reason> {
  auto registers = std::vector<slot_value>{};
  for (const auto arg : args) {
    registers.push_back(integer(arg));
  }
  const auto result = p.machine->run_hosted(p.find(name), registers, {});
  if (result.has_value()) {
    return std::nullopt;
  }
  return result.error().reason;
}

/// Caller-owned result bytes, filled with a pattern that is not a valid value
/// so an unwritten part shows up.
struct result_bytes {
  alignas(16) std::array<std::byte, 64> bytes{};

  result_bytes() { bytes.fill(std::byte{0xAA}); }

  [[nodiscard]] auto span(size_t size) -> std::span<std::byte> {
    return std::span<std::byte>(bytes.data(), size);
  }

  template <typename T> [[nodiscard]] auto at(size_t offset) const -> T {
    auto value = T{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
  }
};

/// Runs a function that builds its result through an address.
auto run_into(const program &p, std::string_view name, result_bytes &out,
              std::initializer_list<int64_t> args) -> void {
  const auto &function = p.function(name);
  expect(function.abi.has_value() && function.abi->returns_through_address,
         "expected a result built through an address");
  auto registers = std::vector<slot_value>{};
  for (const auto arg : args) {
    registers.push_back(integer(arg));
  }
  const auto result =
      p.machine->run_into(p.find(name), out.span(function.abi->result.size),
                          registers);
  if (!result.has_value()) {
    std::cerr << name << " failed: " << result.error().message << "\n";
    fail("expected the function to return");
  }
}

auto expect_equal(int64_t actual, int64_t wanted, std::string_view what)
    -> void {
  if (actual != wanted) {
    std::cerr << what << ": expected " << wanted << ", got " << actual << "\n";
    fail("a computed value differs");
  }
}

// ==========================================================================
//  Scalars and control flow
// ==========================================================================

auto test_scalars_and_control_flow() -> void {
  auto p = build(read_program("hir_storage_test", "flow.cn"));
  struct row {
    std::string_view name;
    std::initializer_list<int64_t> args;
    int64_t wanted;
  };
  const auto rows = std::vector<row>{
      {"early", {1}, 1},
      {"early", {-1}, 2},
      {"both_return", {0}, 2},
      {"pick", {7}, 7},
      {"pick", {-7}, 7},
      {"chain", {11}, 3},
      {"chain", {7}, 2},
      {"chain", {1}, 1},
      {"assign_in_branch", {4}, 1},
      {"assign_in_branch", {0}, 2},
      {"count", {5}, 5},
      {"count_with_locals", {4}, 10},
      {"stops", {3}, 3},
      {"skips", {5}, 12},
      {"returns_from_loop", {4}, 4},
      {"nested", {4}, 5},
      {"stepped", {5}, 8},
      // `and`/`or` evaluate their right side only when the left does not
      // decide the result: dividing by zero here must never run.
      {"guarded", {0}, 0},
      {"guarded", {5}, 1},
      {"guarded", {20}, 0},
      {"either", {0, 0}, 0},
      {"either", {0, 1}, 1},
      {"either", {1, 0}, 1},
  };
  for (const auto &entry : rows) {
    expect_equal(run_scalar(*p, entry.name, entry.args), entry.wanted,
                 entry.name);
  }
}

// ==========================================================================
//  Aggregate results built in caller storage
// ==========================================================================

auto test_aggregate_results_are_built_in_the_caller_storage() -> void {
  auto p = build(read_program("bytecode_storage_test", "values.cn"));

  auto pair = result_bytes{};
  run_into(*p, "make_pair", pair, {5});
  expect_equal(pair.at<int32_t>(0), 5, "make_pair.a");
  expect_equal(pair.at<int32_t>(4), 6, "make_pair.b");

  // The nested pairs are built where they sit inside the line.
  auto line = result_bytes{};
  run_into(*p, "make_line", line, {5});
  expect_equal(line.at<int32_t>(0), 5, "line.from.a");
  expect_equal(line.at<int32_t>(4), 6, "line.from.b");
  expect_equal(line.at<int32_t>(8), 0, "line.to.a");
  expect_equal(line.at<int32_t>(12), 5, "line.to.b");

  auto forwarded = result_bytes{};
  run_into(*p, "forward", forwarded, {7});
  expect_equal(forwarded.at<int32_t>(0), 7, "forward.a");
  expect_equal(forwarded.at<int32_t>(4), 8, "forward.b");

  auto tuple = result_bytes{};
  run_into(*p, "make_tuple", tuple, {3});
  expect_equal(tuple.at<int32_t>(0), 3, "tuple.0");
  expect_equal(tuple.at<int32_t>(4), 4, "tuple.1");

  auto array = result_bytes{};
  run_into(*p, "make_array", array, {9});
  expect_equal(array.at<int32_t>(0), 9, "array[0]");
  expect_equal(array.at<int32_t>(4), 9, "array[1]");
  expect_equal(array.at<int32_t>(8), 0, "array[2]");

  // A sum: the tag, then the active payload at the payload offset.
  auto rect = result_bytes{};
  run_into(*p, "make_rect", rect, {4});
  expect_equal(rect.at<int64_t>(0), 1, "rect tag");
  expect_equal(rect.at<int32_t>(8), 4, "rect.0");
  expect_equal(rect.at<int32_t>(12), 5, "rect.1");
  auto empty = result_bytes{};
  run_into(*p, "make_empty", empty, {});
  expect_equal(empty.at<int64_t>(0), 2, "empty tag");

  // A sum nested in a struct, with trailing fields after its padding.
  auto framed = result_bytes{};
  run_into(*p, "make_framed", framed, {3});
  expect_equal(framed.at<int64_t>(0), 0, "framed.kind tag");
  expect_equal(framed.at<int32_t>(8), 3, "framed.kind payload");
  expect_equal(framed.at<int32_t>(16), 3, "framed.id");

  // Larger than a register image, built and forwarded through two calls.
  auto wide = result_bytes{};
  run_into(*p, "forward_wide", wide, {100});
  for (int64_t part = 0; part < 4; ++part) {
    expect_equal(wide.at<int64_t>(static_cast<size_t>(part) * 8), 100 + part,
                 "wide member");
  }

  // Members narrower than a word are stored at their own widths.
  auto flags = result_bytes{};
  run_into(*p, "make_flags", flags, {42});
  expect_equal(flags.at<uint8_t>(0), 1, "flags.lit");
  expect_equal(flags.at<int8_t>(1), -5, "flags.small");
  expect_equal(flags.at<int16_t>(2), -300, "flags.mid");
  expect_equal(flags.at<int32_t>(4), 42, "flags.id");
}

auto test_locals_assignment_and_borrows() -> void {
  auto p = build(read_program("bytecode_storage_test", "values.cn"));
  expect_equal(run_scalar(*p, "bind", {3}), 4, "bind");
  expect_equal(run_scalar(*p, "borrow", {9}), 9, "borrow");
  expect_equal(run_scalar(*p, "build_line_sum", {5}), 5 + 6 + 0 + 5,
               "build_line_sum");
  // `p.a = 40` through `&mut p`, then reading both fields: 40 + (n + 1).
  expect_equal(run_scalar(*p, "mutate", {2}), 43, "mutate");

  // The replacement is built first, then moved over the old value.
  auto reassigned = result_bytes{};
  run_into(*p, "reassign", reassigned, {5});
  expect_equal(reassigned.at<int32_t>(0), 6, "reassign.a");
  expect_equal(reassigned.at<int32_t>(4), 7, "reassign.b");

  auto returned = result_bytes{};
  run_into(*p, "return_local", returned, {8});
  expect_equal(returned.at<int32_t>(0), 8, "return_local.a");
  expect_equal(returned.at<int32_t>(4), 9, "return_local.b");

  // `p = bump(&p)`: the new value is built apart from the borrowed one.
  auto bumped = result_bytes{};
  run_into(*p, "self_bump", bumped, {2});
  expect_equal(bumped.at<int32_t>(0), 3, "self_bump.a");
  expect_equal(bumped.at<int32_t>(4), 3, "self_bump.b");
}

// ==========================================================================
//  Arguments that travel by address
// ==========================================================================

auto test_aggregate_arguments_travel_by_address() -> void {
  auto p = build(read_program("bytecode_storage_test", "values.cn"));
  expect_equal(run_scalar(*p, "pass_fresh", {10}), 20, "pass_fresh");
  expect_equal(run_scalar(*p, "pass_wide", {50}), 50 + 53, "pass_wide");

  // A host passes a parameter as the address of storage it owns.
  struct alignas(8) pair_bytes {
    int32_t a;
    int32_t b;
  };
  auto argument = pair_bytes{.a = 3, .b = 4};
  const auto registers = std::vector<slot_value>{
      slot_value{static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&argument))}};
  const auto result = p->machine->run(p->find("pass_local"), registers);
  expect(result.has_value() && result->has_value, "expected a returned value");
  expect_equal(result->value.i, 4, "pass_local");
}

// ==========================================================================
//  Frames, recursion, and the heap
// ==========================================================================

auto test_deep_recursion_keeps_frame_storage_stable() -> void {
  auto p = build(read_program("bytecode_storage_test", "values.cn"));
  // Each level reads the value its callee built in its own frame. The frame
  // vector grows many times on the way down; the bytes must not move.
  auto deep = result_bytes{};
  run_into(*p, "deep", deep, {3000});
  expect_equal(deep.at<int32_t>(0), 3000, "deep.a");
  expect_equal(deep.at<int32_t>(4), 1 + 2 * 3000, "deep.b");
}

auto test_values_are_not_allocated_on_the_heap() -> void {
  auto p = build(read_program("bytecode_storage_test", "values.cn"));
  const auto before = runtime::heap_allocation_count();
  auto line = result_bytes{};
  run_into(*p, "make_line", line, {5});
  auto framed = result_bytes{};
  run_into(*p, "make_framed", framed, {5});
  auto deep = result_bytes{};
  run_into(*p, "deep", deep, {200});
  expect_equal(run_scalar(*p, "pass_fresh", {2}), 4, "pass_fresh");
  expect_equal(static_cast<int64_t>(runtime::heap_allocation_count() - before),
               0, "heap allocations");

  // The compiled code has no allocation instruction at all.
  for (const auto &function : p->code.functions) {
    auto offset = size_t{0};
    while (offset < function.code.size()) {
      const auto op = static_cast<bytecode::opcode>(function.code[offset]);
      expect(op != bytecode::opcode::op_alloc,
             "expected no heap allocation instruction");
      offset += bytecode::instruction_size(op);
    }
  }
}

auto test_narrow_signed_values_are_sign_extended_when_read() -> void {
  auto p = build(read_program("bytecode_storage_test", "values.cn"));
  expect_equal(run_scalar(*p, "small_flags", {1}), -305, "small_flags");
  expect_equal(run_scalar(*p, "neg_literal", {}), -128, "neg_literal");
  expect_equal(run_scalar(*p, "narrow_compare", {7}), 1, "narrow_compare");
  // Returned whole, without a cast that would widen it first.
  expect_equal(run_scalar(*p, "mid_of", {7}), -300, "mid_of");
}

auto test_function_metadata_describes_the_calling_convention() -> void {
  auto p = build(read_program("bytecode_storage_test", "values.cn"));
  const auto &line = p->function("make_line");
  expect(line.abi.has_value() && line.abi->returns_through_address,
         "expected make_line to return through an address");
  expect(line.abi->result.size == 16 && line.abi->result.align == 4,
         "expected the line result to be 16 bytes aligned to 4");
  // The hidden result address is register 0 and not a source parameter.
  expect(line.param_count == 2 && line.abi->params.size() == 1,
         "expected a result address and one source parameter");
  const auto &take = p->function("take_pair");
  expect(take.abi->params.size() == 2 && take.abi->params[0].by_address &&
             !take.abi->params[1].by_address,
         "expected the pair to travel by address and the int by register");
  expect(!take.abi->returns_through_address, "expected a scalar result");
  const auto &wide = p->function("make_wide");
  expect(wide.abi->result.size == 32 && wide.abi->result.align == 8,
         "expected the wide result to be 32 bytes aligned to 8");
}

// ==========================================================================
//  Host entry points
// ==========================================================================

auto test_entry_points_check_the_result_storage() -> void {
  auto p = build(read_program("bytecode_storage_test", "values.cn"));
  const auto n = std::vector<slot_value>{integer(1)};
  alignas(16) std::array<std::byte, 32> storage{};

  const auto too_small = p->machine->run_into(
      p->find("make_line"), std::span<std::byte>(storage.data(), 8), n);
  expect(!too_small.has_value() &&
             too_small.error().reason == bytecode::panic_reason::host_failed,
         "expected storage that is too small to be refused");

  const auto misaligned = p->machine->run_into(
      p->find("make_line"), std::span<std::byte>(storage.data() + 1, 16), n);
  expect(!misaligned.has_value() &&
             misaligned.error().reason == bytecode::panic_reason::host_failed,
         "expected misaligned storage to be refused");

  const auto no_result_address = p->machine->run_into(
      p->find("bind"), std::span<std::byte>(storage.data(), 16), n);
  expect(!no_result_address.has_value(),
         "expected a scalar function to refuse result storage");

  const auto wrong_count = p->machine->run_into(
      p->find("make_line"), std::span<std::byte>(storage.data(), 16), {});
  expect(!wrong_count.has_value(),
         "expected the wrong argument count to be refused");

  // The hosted entry point runs to the same result.
  const auto hosted = p->machine->run_hosted_into(
      p->find("make_pair"), std::span<std::byte>(storage.data(), 8), n, {});
  expect(hosted.has_value(), "expected the hosted call to succeed");
  auto pair = result_bytes{};
  std::memcpy(pair.bytes.data(), storage.data(), 8);
  expect_equal(pair.at<int32_t>(4), 2, "hosted make_pair.b");
}

// ==========================================================================
//  Drops
// ==========================================================================

auto test_drops_run_with_the_value_they_end() -> void {
  auto p = build(read_program("bytecode_storage_test", "drops.cn"));
  // `drop` divides by `self.id`, so a drop that runs with an id of zero
  // panics: that shows it ran, and ran on the right bytes.
  struct row {
    std::string_view name;
    int64_t arg;
    int64_t wanted;
  };
  const auto fine = std::vector<row>{
      {"scoped", 5, 1},    {"discarded", 5, 2}, {"replaced", 5, 3},
      {"moved_away", 5, 4}, {"receive", 5, 5},  {"in_loop", 1, 1},
  };
  for (const auto &entry : fine) {
    expect_equal(run_scalar(*p, entry.name, {entry.arg}), entry.wanted,
                 entry.name);
  }
  for (const auto name :
       {"scoped", "discarded", "replaced", "moved_away", "receive"}) {
    const auto failure = run_panics(*p, name, {0});
    expect(failure.has_value() &&
               *failure == bytecode::panic_reason::integer_divide_by_zero,
           "expected the value's drop to run and panic");
  }
  // Only the second iteration's value has an id of zero.
  const auto looped = run_panics(*p, "in_loop", {2});
  expect(looped.has_value() &&
             *looped == bytecode::panic_reason::integer_divide_by_zero,
         "expected a drop in each iteration of the loop");
}

} // namespace

auto main() -> int {
  test_scalars_and_control_flow();
  test_aggregate_results_are_built_in_the_caller_storage();
  test_locals_assignment_and_borrows();
  test_aggregate_arguments_travel_by_address();
  test_deep_recursion_keeps_frame_storage_stable();
  test_values_are_not_allocated_on_the_heap();
  test_narrow_signed_values_are_sign_extended_when_read();
  test_function_metadata_describes_the_calling_convention();
  test_entry_points_check_the_result_storage();
  test_drops_run_with_the_value_they_end();
  std::cout << "storage_compile_test: ok\n";
  return 0;
}
