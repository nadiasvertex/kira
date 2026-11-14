#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>
#include <string_view>

#include "interpret.h"
#include "src/bytecode/panic.h"
#include "src/bytecode/value.h"
#include "src/bytecode/vm.h"
#include "src/bytecode_compiler/storage_compile.h"
#include "src/hir/storage_program.h"
#include "src/hir/lower.h"
#include "src/runtime/aligned_buffer.h"
#include "src/runtime/scheduler.h"
#include "src/util/integer_literal.h"

namespace cinder::driver {

[[nodiscard]] auto render_run_value(const semantic::type_table &types,
                                    semantic::type_id return_type,
                                    const bytecode::slot_value &value)
    -> std::string {
  const auto kind = bytecode::numeric_kind_of(types, return_type);
  if (!kind) {
    return "<unsupported return type>";
  }

  switch (*kind) {
  case bytecode::numeric_kind::i8:
  case bytecode::numeric_kind::i16:
  case bytecode::numeric_kind::i32:
  case bytecode::numeric_kind::i64:
    return std::format("{}", value.i);
  case bytecode::numeric_kind::u8:
  case bytecode::numeric_kind::u16:
  case bytecode::numeric_kind::u32:
  case bytecode::numeric_kind::u64:
    return std::format("{}", value.u);
  case bytecode::numeric_kind::i128:
    return util::format_wide_signed_decimal(static_cast<__int128>(value.w));
  case bytecode::numeric_kind::u128:
    return util::format_wide_decimal(value.w);
  case bytecode::numeric_kind::f32:
    return std::format("{}",
                       std::bit_cast<float>(static_cast<uint32_t>(value.u)));
  case bytecode::numeric_kind::f64:
    return std::format("{}", value.f);
  case bytecode::numeric_kind::boolean:
    return value.i != 0 ? "true" : "false";
  case bytecode::numeric_kind::character:
    return std::format("U+{:04X}", value.u);
  }
  return {};
}

/// Reads a VM return value back as a process exit code, using the same
/// `numeric_kind_of` dispatch `render_run_value` uses to pick apart
/// `slot_value`'s untagged union. Non-numeric/unsupported return types (and
/// `()`) contribute no meaningful exit status, so they fall back to `0`.
///
/// @param types Checked type table `return_type` indexes into.
/// @param return_type Checked return type of the executed function.
/// @param value Raw VM result to read.
[[nodiscard]] static auto exit_code_of(const semantic::type_table &types,
                                       semantic::type_id return_type,
                                       const bytecode::slot_value &value)
    -> int32_t {
  const auto kind = bytecode::numeric_kind_of(types, return_type);
  if (!kind) {
    return 0;
  }

  switch (*kind) {
  case bytecode::numeric_kind::i8:
  case bytecode::numeric_kind::i16:
  case bytecode::numeric_kind::i32:
  case bytecode::numeric_kind::i64:
    return static_cast<int32_t>(value.i);
  case bytecode::numeric_kind::u8:
  case bytecode::numeric_kind::u16:
  case bytecode::numeric_kind::u32:
  case bytecode::numeric_kind::u64:
  case bytecode::numeric_kind::i128:
  case bytecode::numeric_kind::u128:
    return static_cast<int32_t>(value.u);
  case bytecode::numeric_kind::boolean:
    return value.i != 0 ? 1 : 0;
  case bytecode::numeric_kind::character:
    return static_cast<int32_t>(value.u);
  case bytecode::numeric_kind::f32:
  case bytecode::numeric_kind::f64:
    return static_cast<int32_t>(value.f);
  }
  return 0;
}

/// The inline result bytes at `at` as the `slot_value` `render_run_value`
/// and `exit_code_of` read: a signed integer sign-extended into `i`, an
/// unsigned one zero-extended into `u`, a 128-bit one in `w`, and a float's
/// bits where its width reads them.
[[nodiscard]] static auto
slot_of_inline(const semantic::type_table &types, semantic::type_id type,
               const std::byte *at, size_t size) -> bytecode::slot_value {
  auto bits = uint64_t{0};
  std::memcpy(&bits, at, std::min(size, sizeof(bits)));
  auto slot = bytecode::slot_value{};
  slot.u = bits;
  const auto kind = bytecode::numeric_kind_of(types, type);
  if (!kind.has_value()) {
    return slot;
  }
  const auto sign = [bits](unsigned width) -> int64_t {
    const auto shift = 64U - width;
    return static_cast<int64_t>(bits << shift) >> shift;
  };
  switch (*kind) {
  case bytecode::numeric_kind::i8:
    slot.i = sign(8);
    break;
  case bytecode::numeric_kind::i16:
    slot.i = sign(16);
    break;
  case bytecode::numeric_kind::i32:
    slot.i = sign(32);
    break;
  case bytecode::numeric_kind::u8:
    slot.u = bits & 0xffU;
    break;
  case bytecode::numeric_kind::u16:
    slot.u = bits & 0xffffU;
    break;
  case bytecode::numeric_kind::u32:
  case bytecode::numeric_kind::character:
  case bytecode::numeric_kind::f32:
    slot.u = bits & 0xffffffffU;
    break;
  case bytecode::numeric_kind::boolean:
    slot.u = bits & 1U;
    break;
  case bytecode::numeric_kind::i128:
  case bytecode::numeric_kind::u128: {
    auto wide = static_cast<unsigned __int128>(0);
    std::memcpy(&wide, at, std::min(size, sizeof(wide)));
    slot.w = wide;
    break;
  }
  default:
    break;
  }
  return slot;
}

[[nodiscard]] auto
run_hir_module(std::span<const hir::hir_module *const> modules,
               semantic::checked_types &checked,
               std::string_view function_name) -> run_outcome {
  const auto &types = checked.types;
  const auto &entry_module = *modules.front();
  const auto *target = static_cast<const hir::hir_function *>(nullptr);
  for (const auto &fn : entry_module.functions) {
    if (fn != nullptr && fn->name == function_name) {
      target = fn.get();
      break;
    }
  }

  if (target == nullptr) {
    return run_outcome{
        .succeeded = false,
        .message = std::format("module `{}` has no function named `{}`",
                               entry_module.module_name, function_name)};
  }

  if (!target->params.empty()) {
    return run_outcome{
        .succeeded = false,
        .message = std::format(
            "cannot run `{}`: --run only supports zero-parameter functions",
            function_name)};
  }

  auto program = hir::prepare_storage_program(modules, checked);
  if (!program.has_value()) {
    return run_outcome{.succeeded = false,
                       .message = std::format("failed to compile `{}`: {}",
                                              function_name, program.error())};
  }
  auto compiled = bytecode_compiler::compile_storage_module(
      std::span<const hir::storage_module *const>((*program)->modules),
      *(*program)->env);
  if (!compiled) {
    return run_outcome{.succeeded = false,
                       .message = std::format("failed to compile `{}` to "
                                              "bytecode: {}",
                                              function_name,
                                              compiled.error().message)};
  }

  auto index = uint16_t{0};
  const auto function_count = static_cast<uint16_t>(compiled->functions.size());
  for (; index < function_count; ++index) {
    if (compiled->functions[index].name == function_name) {
      break;
    }
  }

  const auto vm = bytecode::vm{*compiled};
  // Builds every static's value before the entry point can read any of
  // them: one initializer per module, writing the module's globals.
  for (const auto init : compiled->static_initializers) {
    auto init_result = vm.run(init, std::span<const bytecode::slot_value>{});
    if (!init_result) {
      return run_outcome{
          .succeeded = false,
          .message =
              std::format("static initialization panicked: {}",
                          bytecode::panic_reason_message(init_result.error()))};
    }
  }
  // The result's inline bytes: built through an address for an aggregate,
  // returned in a register otherwise.
  const auto &abi = compiled->functions[index].abi;
  const auto result_size = abi.has_value() ? abi->result.size : size_t{8};
  auto bytes = runtime::aligned_buffer(
      std::max<size_t>(result_size, 16),
      std::max<size_t>(abi.has_value() ? abi->result.align : 8, 16));
  auto result = std::expected<bytecode::vm_result, bytecode::panic_reason>{};
  if (abi.has_value() && abi->returns_through_address) {
    auto into = vm.run_into(index, std::span<std::byte>(bytes.data(), bytes.size()),
                            std::span<const bytecode::slot_value>{});
    if (into.has_value()) {
      result = bytecode::vm_result{.has_value = true};
    } else {
      result = std::unexpected(into.error().reason);
    }
  } else {
    result = vm.run(index, std::span<const bytecode::slot_value>{});
    if (result.has_value()) {
      std::memcpy(bytes.data(), &result->value, sizeof(result->value));
      result->value =
          slot_of_inline(types, target->return_type, bytes.data(), result_size);
    }
  }
  // `vm` and `compiled` end with this function, and a task still running
  // reads both, so one that will not stop has to end with the process.
  const auto stopped = runtime::scheduler::shutdown();
  if (!result) {
    if (!stopped) {
      std::_Exit(bytecode::k_panic_exit_code);
    }
    return run_outcome{
        .panicked = true,
        .succeeded = false,
        .message = std::format("`{}` panicked: {}", function_name,
                               bytecode::panic_reason_message(result.error()))};
  }

  if (!stopped) {
    std::_Exit(result->has_value
                   ? exit_code_of(types, target->return_type, result->value)
                   : 0);
  }

  if (!result->has_value) {
    return run_outcome{.succeeded = true,
                       .message = std::format("{}() -> ()", function_name)};
  }

  return run_outcome{
      .succeeded = true,
      .exit_code = exit_code_of(types, target->return_type, result->value),
      .message = std::format(
          "{}() -> {}", function_name,
          render_run_value(types, target->return_type, result->value))};
}

} // namespace cinder::driver
