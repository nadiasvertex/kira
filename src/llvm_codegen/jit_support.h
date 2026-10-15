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

namespace cinder::llvm_codegen {

/// JIT helper: enough `llvm::orc::LLJIT` plumbing to run a `compile_module`
/// result's zero-argument entry function in-process. The tests use it to
/// compare the LLVM tier's result against the bytecode VM's
/// (`bytecode::vm::run`). It does not tier up hot VM code, and no command
/// runs programs through it. See
/// `spec/specification/05-implementation/68-jit-execution.md`.
struct jit_result {
  bool has_value = false;
  bytecode::slot_value value{};
};

class jit_module {
public:
  /// Takes ownership of `module` (its `llvm::LLVMContext`/`llvm::Module`
  /// pair), runs `optimize_module(*module.module, level)` over it (a no-op
  /// at the default `o0`), and adds it to a fresh `LLJIT` instance, with
  /// process symbols (this executable's own linked-in `cinder_codegen_panic`,
  /// see runtime.h) visible to it.
  [[nodiscard]] static auto
  create(compiled_module module,
         optimization_level level = optimization_level::o0)
      -> std::expected<jit_module, std::string>;

  /// Runs the zero-argument function `name`. `return_kind` must be the
  /// `numeric_kind` the function's checked return type maps to (`nullopt`
  /// for a `unit`-returning function) — a JIT-compiled function has a real
  /// native ABI signature, unlike the bytecode VM's uniform `slot_value`
  /// calling convention, so the caller must say which typed function
  /// pointer to call through. Converts a propagating `panic_error`
  /// (`cinder_codegen_panic`, thrown across the JIT'd frame and caught here)
  /// into `bytecode::panic_reason`, mirroring `bytecode::vm::run`'s own
  /// `std::expected` shape so both tiers' results are directly comparable.
  [[nodiscard]] auto
  run(std::string_view name,
      std::optional<bytecode::numeric_kind> return_kind) const
      -> std::expected<jit_result, bytecode::panic_reason>;

  /// Like `run`, but for a zero-argument function whose checked return type
  /// is heap-backed (`str`, `list[T]`, tuple, fixed array, struct, sum type,
  /// or a closure) rather than a scalar `numeric_kind` — its native ABI
  /// return type is a plain `ptr` (every heap value is one pointer,
  /// `src/runtime/layout.h`), so there's no per-kind marshaling to dispatch
  /// on the way `run` needs. The returned pointer is a real, dereferenceable
  /// address in this process (the JIT runs in-process), suitable for the
  /// stress harness's deep-equality walk against the bytecode VM's own
  /// heap value.
  [[nodiscard]] auto run_ptr_result(std::string_view name) const
      -> std::expected<jit_result, bytecode::panic_reason>;

private:
  explicit jit_module(std::unique_ptr<llvm::orc::LLJIT> jit)
      : jit_(std::move(jit)) {}

  std::unique_ptr<llvm::orc::LLJIT> jit_;
};

} // namespace cinder::llvm_codegen
