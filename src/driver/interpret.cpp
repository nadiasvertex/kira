#include <bit>
#include <format>
#include <string>
#include <string_view>

#include "interpret.h"
#include "src/bytecode/panic.h"
#include "src/bytecode/value.h"
#include "src/bytecode/vm.h"
#include "src/bytecode_compiler/compile.h"
#include "src/hir/lower.h"

namespace kira::driver {

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

[[nodiscard]] auto
run_hir_module(std::span<const hir::hir_module *const> modules,
               const semantic::type_table &types,
               std::string_view function_name) -> run_outcome {
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

  auto compiled = bytecode_compiler::compile_module(modules, types);
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
  auto result = vm.run(index, std::span<const bytecode::slot_value>{});
  if (!result) {
    return run_outcome{
        .succeeded = false,
        .message = std::format("`{}` panicked: {}", function_name,
                               bytecode::panic_reason_message(result.error()))};
  }

  if (!result->has_value) {
    return run_outcome{.succeeded = true,
                       .message = std::format("{}() -> ()", function_name)};
  }

  return run_outcome{
      .succeeded = true,
      .message = std::format(
          "{}() -> {}", function_name,
          render_run_value(types, target->return_type, result->value))};
}

} // namespace kira::driver
