#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
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

/// The sanitizer this process was built with, which JIT-compiled code is
/// instrumented for so that it is checked like the C++ around it.
enum class host_sanitizer : std::uint8_t { none, address, thread };

[[nodiscard]] auto current_host_sanitizer() -> host_sanitizer;

/// Instruments every function `module` defines for `current_host_sanitizer()`
/// (AddressSanitizer or ThreadSanitizer), as `-fsanitize=` would for C++.
/// The sanitizer runtime is the host's, found through the process's symbols.
/// Globals are left uninstrumented under AddressSanitizer: registering them
/// needs the object-file sections a static link provides. Does nothing in an
/// unsanitized build. `jit_module::create` calls it after optimization.
auto instrument_for_host_sanitizer(llvm::Module &module) -> void;

/// JIT helper: enough `llvm::orc::LLJIT` plumbing to run a compiled module's
/// functions in-process. The tests use it to run the LLVM tier
/// (`run_storage` for code compiled from the storage form). It does not tier
/// up hot VM code, and no command runs programs through it. See
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

  /// Runs the zero-argument native function `name` (hand-written IR, say),
  /// whose native return type is the one `return_kind` names (`nullopt` for
  /// `void`), so the caller must say which typed function pointer to call
  /// through. Code compiled from the storage form is run with
  /// `run_storage` instead. Converts a propagating `panic_error`
  /// (`cinder_codegen_panic`, thrown across the JIT'd frame and caught here)
  /// into `bytecode::panic_reason`, mirroring `bytecode::vm::run`'s own
  /// `std::expected` shape so both tiers' results are directly comparable.
  [[nodiscard]] auto
  run(std::string_view name,
      std::optional<bytecode::numeric_kind> return_kind) const
      -> std::expected<jit_result, bytecode::panic_reason>;

  /// Calls a function compiled from the storage form through its entry thunk
  /// (`storage_codegen.h`, `storage_codegen_options::entry_thunks`). `args`
  /// holds one 8-byte slot per non-zero-sized source parameter; `result` is
  /// the caller-owned storage a by-address result is built in, or where a
  /// scalar result is written (it may be null for a zero-sized result). A
  /// propagating `panic_error` becomes a `panic_reason`, as in `run`.
  [[nodiscard]] auto run_storage(std::string_view name,
                                 std::span<const uint64_t> args,
                                 void *result) const
      -> std::expected<void, bytecode::panic_reason>;

private:
  explicit jit_module(std::unique_ptr<llvm::orc::LLJIT> jit)
      : jit_(std::move(jit)) {}

  std::unique_ptr<llvm::orc::LLJIT> jit_;
};

} // namespace cinder::llvm_codegen
