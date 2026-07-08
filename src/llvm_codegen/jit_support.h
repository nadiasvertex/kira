#pragma once

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#pragma clang diagnostic ignored "-Wshadow"
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#pragma clang diagnostic pop

#include "src/bytecode/panic.h"
#include "src/bytecode/value.h"
#include "src/llvm_codegen/codegen.h"

namespace kira::llvm_codegen {

/// Test/JIT-execution-only helper: enough `llvm::orc::LLJIT` plumbing to
/// actually run a `compile_module` result's zero-argument entry function, so
/// `codegen_test.cpp` and the codegen stress suite can compare its result
/// directly against the bytecode VM's (`bytecode::vm::run`). This is
/// explicitly *not* `spec/codegen-design.md` increment 5's tier-up JIT
/// wiring (no call counters, no dispatch-slot swap, no interpreter
/// integration) — just the minimum needed to prove increment 3's `HIR ->
/// llvm::Module` lowering is correct by actually running its output, the
/// same spirit as increment 3's own "prove the lowering is correct" scope,
/// just via execution instead of by-hand `.ll` inspection.
struct jit_result {
  bool has_value = false;
  bytecode::slot_value value{};
};

class jit_module {
public:
  /// Takes ownership of `module` (its `llvm::LLVMContext`/`llvm::Module`
  /// pair) and adds it to a fresh `LLJIT` instance, with process symbols
  /// (this executable's own linked-in `kira_codegen_panic`, see runtime.h)
  /// visible to it.
  [[nodiscard]] static auto create(compiled_module module)
      -> std::expected<jit_module, std::string>;

  /// Runs the zero-argument function `name`. `return_kind` must be the
  /// `numeric_kind` the function's checked return type maps to (`nullopt`
  /// for a `unit`-returning function) — a JIT-compiled function has a real
  /// native ABI signature, unlike the bytecode VM's uniform `slot_value`
  /// calling convention, so the caller must say which typed function
  /// pointer to call through. Converts a propagating `panic_error`
  /// (`kira_codegen_panic`, thrown across the JIT'd frame and caught here)
  /// into `bytecode::panic_reason`, mirroring `bytecode::vm::run`'s own
  /// `std::expected` shape so both tiers' results are directly comparable.
  [[nodiscard]] auto
  run(std::string_view name,
      std::optional<bytecode::numeric_kind> return_kind) const
      -> std::expected<jit_result, bytecode::panic_reason>;

private:
  explicit jit_module(std::unique_ptr<llvm::orc::LLJIT> jit)
      : jit_(std::move(jit)) {}

  std::unique_ptr<llvm::orc::LLJIT> jit_;
};

} // namespace kira::llvm_codegen
