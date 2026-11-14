// Bytecode compiled from the shared storage form: results are checked against
// values worked out by hand (`src/testing/storage_cases.h`, shared with the
// LLVM backend), not against the pointer-per-aggregate compiler.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
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
#include "src/testing/storage_cases.h"
#include "src/testing/storage_closure_cases.h"
#include "src/testing/storage_fixture.h"
#include "src/testing/storage_generator_cases.h"
#include "src/testing/storage_intrinsic_cases.h"
#include "src/testing/storage_memory_cases.h"
#include "src/testing/storage_module_cases.h"
#include "src/testing/storage_pattern_cases.h"
#include "src/testing/storage_rvo_cases.h"
#include "src/testing/storage_task_cases.h"
#include "src/testing/test_assert.h"

namespace {

using cinder::testing::expect;
using cinder::testing::fail;
namespace bytecode = cinder::bytecode;
namespace cases = cinder::testing::storage_cases;
using bytecode::slot_value;
using cases::expect_equal;

/// A fixture compiled to bytecode, ready to run on the VM.
class bytecode_program final : public cases::case_runner {
public:
  explicit bytecode_program(std::unique_ptr<cinder::testing::storage_program> p)
      : program_(std::move(p)) {
    const auto modules = program_->modules();
    auto compiled = cinder::bytecode_compiler::compile_storage_module(
        std::span<const cinder::hir::storage_module *const>(modules),
        *program_->env);
    if (!compiled.has_value()) {
      std::cerr << compiled.error().message << "\n";
      fail("expected the storage form to compile to bytecode");
    }
    code = std::move(*compiled);
    for (size_t i = 0; i < code.functions.size(); ++i) {
      index.emplace(code.functions[i].name, static_cast<uint16_t>(i));
    }
    machine = std::make_unique<bytecode::vm>(code);
    // Each module's statics are built once, before anything reads them.
    for (const auto init : code.static_initializers) {
      const auto ran = machine->run(init, std::vector<slot_value>{});
      if (!ran.has_value()) {
        std::cerr << bytecode::panic_reason_message(ran.error()) << "\n";
        fail("expected the statics to build");
      }
    }
  }

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

  [[nodiscard]] auto scalar(std::string_view name,
                            std::span<const int64_t> args) -> int64_t override {
    const auto result = machine->run(find(name), registers(args));
    if (!result.has_value()) {
      std::cerr << name << " panicked: "
                << bytecode::panic_reason_message(result.error()) << "\n";
      fail("expected the function to return");
    }
    expect(result->has_value, "expected a returned value");
    return result->value.i;
  }

  [[nodiscard]] auto panics(std::string_view name,
                            std::span<const int64_t> args)
      -> std::optional<bytecode::panic_reason> override {
    const auto &fn = function(name);
    if (fn.abi.has_value() && fn.abi->returns_through_address) {
      // A function that writes its result through an address needs somewhere
      // to write it even when it is expected to panic first.
      auto out = cases::result_bytes{};
      const auto result = machine->run_hosted_into(
          find(name),
          std::span<std::byte>(out.bytes.data(), fn.abi->result.size),
          registers(args), {});
      if (result.has_value()) {
        return std::nullopt;
      }
      return result.error().reason;
    }
    const auto result = machine->run_hosted(find(name), registers(args), {});
    if (result.has_value()) {
      return std::nullopt;
    }
    return result.error().reason;
  }

  [[nodiscard]] auto into(std::string_view name, std::span<const int64_t> args)
      -> cases::result_bytes override {
    const auto &fn = function(name);
    expect(fn.abi.has_value() && fn.abi->returns_through_address,
           "expected a result built through an address");
    auto out = cases::result_bytes{};
    const auto result = machine->run_into(
        find(name), std::span<std::byte>(out.bytes.data(), fn.abi->result.size),
        registers(args));
    if (!result.has_value()) {
      std::cerr << name << " failed: " << result.error().message << "\n";
      fail("expected the function to return");
    }
    return out;
  }

  [[nodiscard]] auto scalar_at(std::string_view name, void *argument)
      -> int64_t override {
    const auto regs = std::vector<slot_value>{slot_value{
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(argument))}};
    const auto result = machine->run(find(name), regs);
    expect(result.has_value() && result->has_value,
           "expected a returned value");
    return result->value.i;
  }

  [[nodiscard]] auto reports_explicit_panic() const -> bool override {
    return true;
  }

  bytecode::bytecode_module code;
  std::unique_ptr<bytecode::vm> machine;
  std::unordered_map<std::string, uint16_t> index;

private:
  [[nodiscard]] static auto registers(std::span<const int64_t> args)
      -> std::vector<slot_value> {
    auto out = std::vector<slot_value>{};
    for (const auto arg : args) {
      out.push_back(slot_value{arg});
    }
    return out;
  }

  std::unique_ptr<cinder::testing::storage_program> program_;
};

class bytecode_backend final : public cases::case_backend {
public:
  [[nodiscard]] auto open(std::string_view dir, std::string_view name)
      -> std::unique_ptr<cases::case_runner> override {
    return build(dir, name);
  }

  [[nodiscard]] static auto build(std::string_view dir, std::string_view name)
      -> std::unique_ptr<bytecode_program> {
    return std::make_unique<bytecode_program>(
        cinder::testing::open_storage_program(dir, name));
  }
};

// ==========================================================================
//  Checks only the bytecode tier can make
// ==========================================================================

auto test_compiled_code_has_no_allocation_instruction() -> void {
  auto p = bytecode_backend::build("bytecode_storage_test", "values.cn");
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

auto test_function_metadata_describes_the_calling_convention() -> void {
  auto p = bytecode_backend::build("bytecode_storage_test", "values.cn");
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

/// A program of several modules has one function table: the entry module's
/// functions under their names, every other module's as `module::name`.
auto test_program_table_names_other_modules_functions() -> void {
  auto p = bytecode_backend::build("bytecode_storage_test", "modules.cn");
  const auto &local = p->function("make");
  expect(!local.abi->returns_through_address && local.abi->params.size() == 1,
         "expected the entry module's `make` to return a scalar");
  expect(local.source_module == "sample", "expected `make` to be sample's");
  const auto &other = p->function("tools::make");
  expect(other.abi->returns_through_address && other.abi->result.size == 8,
         "expected `tools::make` to return a pair through an address");
  expect(other.source_module == "tools" && other.source_function == "make",
         "expected `tools::make` to record its own module and name");
  expect(p->code.module_name == "sample",
         "expected the program to be named after the entry module");
}

auto test_entry_points_check_the_result_storage() -> void {
  auto p = bytecode_backend::build("bytecode_storage_test", "values.cn");
  const auto n = std::vector<slot_value>{slot_value{int64_t{1}}};
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
  auto pair = cases::result_bytes{};
  std::memcpy(pair.bytes.data(), storage.data(), 8);
  expect_equal(pair.at<int32_t>(4), 2, "hosted make_pair.b");
}

} // namespace

/// The storage form's calls are ordinary calls. A tail call reuses the caller's
/// frame, which holds outgoing arguments and locals a callee may be handed the
/// address of, so none is emitted until a shared plan proves one safe.
/// A tail call with scalar operands and result reuses the frame; one that
/// passes or returns bytes in this frame keeps it (`hir::is_safe_tail_call`).
auto test_only_scalar_tail_calls_reuse_the_frame() -> void {
  auto p = bytecode_backend::build("bytecode_storage_test", "frames_tail.cn");
  auto calls = size_t{0};
  auto tail_calls = std::map<std::string, size_t>{};
  for (const auto &function : p->code.functions) {
    auto offset = size_t{0};
    while (offset < function.code.size()) {
      const auto op = static_cast<bytecode::opcode>(function.code[offset]);
      tail_calls[function.name] += op == bytecode::opcode::op_tail_call;
      calls += op == bytecode::opcode::op_call;
      offset += bytecode::instruction_size(op);
    }
  }
  expect(calls >= 3, "expected the fixture's calls to be scanned");
  const auto tail_calls_in = [&](std::string_view name) {
    auto total = size_t{0};
    for (const auto &[function, count] : tail_calls) {
      if (function.ends_with(name)) {
        total += count;
      }
    }
    return total;
  };
  expect(tail_calls_in("count_down") == 1,
         "expected count_down's self-call to reuse its frame");
  expect(tail_calls_in("tail_with_pair") == 0,
         "expected a call passing a pair built in this frame to keep it");
  expect(tail_calls_in("tail_forwarding_result") == 0,
         "expected a call building the caller's result to keep the frame");
  const auto ten = std::array<int64_t, 1>{10};
  const auto deep = std::array<int64_t, 1>{1000000};
  const auto three = std::array<int64_t, 1>{3};
  expect_equal(p->scalar("count_down", ten), 0, "count_down");
  // Deeper than the VM's call-depth limit, so only a reused frame finishes.
  expect_equal(p->scalar("count_down", deep), 0, "count_down a million deep");
  expect_equal(p->scalar("tail_with_pair", three), 3 + 4, "tail_with_pair");
}

/// Loads, stores and addresses more than 64 KiB into one slot, past the 16-bit
/// operand offset, reach the right bytes.
auto test_offsets_past_64k_reach_their_bytes() -> void {
  auto p = bytecode_backend::build("bytecode_storage_test", "far_offsets.cn");
  expect_equal(p->scalar("spread", {}), 5 + 5, "spread");
  expect_equal(p->scalar("counted", {}), 8199 + 8192 + 7 + 0, "counted");
}

/// The language's frame limit comes first; the compiler's own encoding limit is
/// a separate line of defense, reached only when the first is lifted.
auto test_frame_offsets_that_do_not_fit_are_refused() -> void {
  const auto text =
      cinder::testing::read_program("bytecode_storage_test", "huge_frame.cn");
  auto lifted = cinder::testing::build_storage_program(
      text, std::numeric_limits<uint64_t>::max() - 1);
  auto compiled = cinder::bytecode_compiler::compile_storage_module(
      lifted->lowering.module, *lifted->env);
  expect(!compiled.has_value(), "expected a 4.8 GB frame not to compile");
  expect(compiled.error().kind ==
             cinder::bytecode_compiler::compile_error_kind::
                 encoding_limit_exceeded,
         "expected the encoding limit to be named");
  expect(compiled.error().message.find("holds_huge") != std::string::npos,
         "expected the function to be named");
}

auto main() -> int {
  auto backend = bytecode_backend{};
  // Every case runs with a returned local built in the result storage and
  // again with it moved there; the expected values do not change.
  for (const auto nrvo : {true, false}) {
    cinder::testing::storage_nrvo_enabled() = nrvo;
    cases::run_all(backend);
    cases::intrinsic_cases(backend);
    cases::memory_cases(backend);
    cases::closure_cases(backend);
    cases::pattern_cases(backend);
    cases::rvo_cases(backend);
    cases::generator_cases(backend);
    cases::task_cases(backend);
    cases::module_cases(backend);
  }
  cinder::testing::storage_nrvo_enabled() = true;
  test_compiled_code_has_no_allocation_instruction();
  test_function_metadata_describes_the_calling_convention();
  test_program_table_names_other_modules_functions();
  test_entry_points_check_the_result_storage();
  test_frame_offsets_that_do_not_fit_are_refused();
  test_only_scalar_tail_calls_reuse_the_frame();
  test_offsets_past_64k_reach_their_bytes();
  std::cout << "storage_compile_test: ok\n";
  return 0;
}
