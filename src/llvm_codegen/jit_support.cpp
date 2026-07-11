#include "src/llvm_codegen/jit_support.h"

#include <bit>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <utility>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#pragma clang diagnostic ignored "-Wshadow"
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>
#pragma clang diagnostic pop

namespace kira::llvm_codegen {

namespace {

using bytecode::numeric_kind;
using bytecode::panic_error;
using bytecode::slot_value;

auto ensure_native_target_initialized() -> void {
  static const auto initialized = [] -> bool {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    return true;
  }();
  (void)initialized;
}

// Packs one native return value into the same `slot_value` bit pattern
// `bytecode::vm` would produce for the equivalent `numeric_kind` (mirrors
// vm.cpp's file-local `store_signed`/`store_unsigned`/`store_f32`/
// `store_f64`, which aren't exported for one JIT-execution caller to reuse).
[[nodiscard]] auto pack(int8_t v) -> slot_value {
  return slot_value{static_cast<int64_t>(v)};
}
[[nodiscard]] auto pack(int16_t v) -> slot_value {
  return slot_value{static_cast<int64_t>(v)};
}
[[nodiscard]] auto pack(int32_t v) -> slot_value {
  return slot_value{static_cast<int64_t>(v)};
}
[[nodiscard]] auto pack(int64_t v) -> slot_value { return slot_value{v}; }
[[nodiscard]] auto pack(uint8_t v) -> slot_value {
  return slot_value{static_cast<uint64_t>(v)};
}
[[nodiscard]] auto pack(uint16_t v) -> slot_value {
  return slot_value{static_cast<uint64_t>(v)};
}
[[nodiscard]] auto pack(uint32_t v) -> slot_value {
  return slot_value{static_cast<uint64_t>(v)};
}
[[nodiscard]] auto pack(uint64_t v) -> slot_value { return slot_value{v}; }
[[nodiscard]] auto pack(float v) -> slot_value {
  return slot_value{static_cast<uint64_t>(std::bit_cast<uint32_t>(v))};
}
[[nodiscard]] auto pack(double v) -> slot_value { return slot_value{v}; }
[[nodiscard]] auto pack(bool v) -> slot_value {
  return slot_value{static_cast<uint64_t>(v ? 1 : 0)};
}

template <typename T>
[[nodiscard]] auto call_and_pack(void *addr) -> slot_value {
  return pack(reinterpret_cast<T (*)()>(addr)());
}

} // namespace

auto jit_module::create(compiled_module module, optimization_level level)
    -> std::expected<jit_module, std::string> {
  ensure_native_target_initialized();
  optimize_module(*module.module, level);

  auto builder = llvm::orc::LLJITBuilder();
  auto jit = builder.create();
  if (!jit) {
    return std::unexpected(llvm::toString(jit.takeError()));
  }

  auto generator =
      llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
          (*jit)->getDataLayout().getGlobalPrefix());
  if (!generator) {
    return std::unexpected(llvm::toString(generator.takeError()));
  }
  (*jit)->getMainJITDylib().addGenerator(std::move(*generator));

  auto add_error = (*jit)->addIRModule(llvm::orc::ThreadSafeModule(
      std::move(module.module), std::move(module.context)));
  if (add_error) {
    return std::unexpected(llvm::toString(std::move(add_error)));
  }

  return jit_module(std::move(*jit));
}

auto jit_module::run(std::string_view name,
                     std::optional<numeric_kind> return_kind) const
    -> std::expected<jit_result, bytecode::panic_reason> {
  auto symbol = jit_->lookup(name);
  if (!symbol) {
    throw std::runtime_error(
        std::format("jit_module::run: symbol `{}` not found: {}", name,
                    llvm::toString(symbol.takeError())));
  }
  auto *addr = symbol->toPtr<void *>();

  try {
    if (!return_kind.has_value()) {
      reinterpret_cast<void (*)()>(addr)();
      return jit_result{.has_value = false};
    }
    switch (*return_kind) {
    case numeric_kind::i8:
      return jit_result{.has_value = true,
                        .value = call_and_pack<int8_t>(addr)};
    case numeric_kind::i16:
      return jit_result{.has_value = true,
                        .value = call_and_pack<int16_t>(addr)};
    case numeric_kind::i32:
      return jit_result{.has_value = true,
                        .value = call_and_pack<int32_t>(addr)};
    case numeric_kind::i64:
      return jit_result{.has_value = true,
                        .value = call_and_pack<int64_t>(addr)};
    case numeric_kind::u8:
      return jit_result{.has_value = true,
                        .value = call_and_pack<uint8_t>(addr)};
    case numeric_kind::u16:
      return jit_result{.has_value = true,
                        .value = call_and_pack<uint16_t>(addr)};
    case numeric_kind::u32:
    case numeric_kind::character:
      return jit_result{.has_value = true,
                        .value = call_and_pack<uint32_t>(addr)};
    case numeric_kind::u64:
      return jit_result{.has_value = true,
                        .value = call_and_pack<uint64_t>(addr)};
    case numeric_kind::f32:
      return jit_result{.has_value = true, .value = call_and_pack<float>(addr)};
    case numeric_kind::f64:
      return jit_result{.has_value = true,
                        .value = call_and_pack<double>(addr)};
    case numeric_kind::boolean:
      return jit_result{.has_value = true, .value = call_and_pack<bool>(addr)};
    }
    throw std::runtime_error("jit_module::run: unhandled numeric_kind");
  } catch (const panic_error &ex) {
    return std::unexpected(ex.reason());
  }
}

auto jit_module::run_ptr_result(std::string_view name) const
    -> std::expected<jit_result, bytecode::panic_reason> {
  auto symbol = jit_->lookup(name);
  if (!symbol) {
    throw std::runtime_error(
        std::format("jit_module::run_ptr_result: symbol `{}` not found: {}",
                    name, llvm::toString(symbol.takeError())));
  }
  auto *addr = symbol->toPtr<void *>();

  try {
    auto *result = reinterpret_cast<void *(*)()>(addr)();
    return jit_result{.has_value = true,
                      .value = slot_value{static_cast<uint64_t>(
                          reinterpret_cast<uintptr_t>(result))}};
  } catch (const panic_error &ex) {
    return std::unexpected(ex.reason());
  }
}

} // namespace kira::llvm_codegen
