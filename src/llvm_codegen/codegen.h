#pragma once

#include <cstdint>
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
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#pragma clang diagnostic pop

#include "src/parser/source_location.h"

namespace cinder::llvm_codegen {

/// Why `compile_storage_module` (`storage_codegen.h`) refused to produce an
/// `llvm::Module` for a function of the storage form. It compiles the same
/// storage form as the bytecode compiler (`src/bytecode_compiler/
/// storage_compile.h`) and fails closed on the same kinds of construct, so a
/// gap in one tier is reported as a gap in the other.
enum class codegen_error_kind : uint8_t {
  unsupported_construct,
  unsupported_type,
  unknown_callee,
};

/// One reason lowering stopped, plus where in the source it happened.
struct codegen_error {
  codegen_error_kind kind;
  source_span span;
  std::string message;
};

/// An `llvm::Module` together with the `llvm::LLVMContext` that owns all of
/// its types/constants — an `llvm::Module` is not usable once its context is
/// destroyed, so the two are kept paired rather than letting a caller
/// accidentally separate them. Ready to hand to `llvm::orc::LLJIT` via
/// `llvm::orc::ThreadSafeModule` (which takes exactly this pair), or to
/// the AOT path in `aot.h`, which emits an object file from it.
struct compiled_module {
  std::unique_ptr<llvm::LLVMContext> context;
  std::unique_ptr<llvm::Module> module;
};

/// The exact symbol name generated IR calls to report a panic. Both tiers
/// raise the same closed `bytecode::panic_reason` set. Defined as an ordinary
/// C++ function (`runtime.h`) that the JIT resolves via process symbols — see
/// `jit_support.h` for the execution side of this contract.
inline constexpr const char *kPanicSymbolName = "cinder_codegen_panic";

// ==========================================================================
//  Optimization — an opt-in `llvm::PassBuilder` pipeline run over a
//  `compiled_module`'s IR before it's JIT-executed or emitted as a native
//  object file. `compile_storage_module` never runs any optimization itself
//  (the generated IR should stay a direct, readable reflection of the
//  source); `optimize_module` is a separate, explicit step a caller
//  opts into via `-O1`/`-O2`/`-O3` (`driver::cli_config::opt_level`).
// ==========================================================================

/// Mirrors clang/gcc's `-O0`/`-O1`/`-O2`/`-O3` convention and maps 1:1 onto
/// `llvm::OptimizationLevel` (see `optimize_module`'s definition in
/// `optimize.cpp` for the actual `llvm::PassBuilder` pipeline — kept out of
/// this header so callers that only need `compiled_module` don't have to
/// pull in `<llvm/Passes/PassBuilder.h>`). `o0` — no
/// optimization passes run at all — is every caller's default unless a
/// `-O` flag says otherwise.
enum class optimization_level : uint8_t { o0 = 0, o1 = 1, o2 = 2, o3 = 3 };

/// Parses a CLI-style `-O` value's text (`"0"`/`"1"`/`"2"`/`"3"`, i.e. the
/// part after `-O`) into an `optimization_level` — `nullopt` for anything
/// else, so the caller (`driver::parse_args`) can report a clear diagnostic
/// naming exactly what failed to parse.
[[nodiscard]] auto parse_optimization_level(std::string_view text)
    -> std::optional<optimization_level>;

/// Runs `llvm::PassBuilder`'s standard per-module pipeline for `level` over
/// `module` in place — a no-op when `level` is `o0`. Safe to call on any
/// verified module, including one `aot.cpp`'s `synthesize_c_main` has
/// already added a synthetic `main` to, or one about to be handed to
/// `llvm::orc::LLJIT`.
auto optimize_module(llvm::Module &module, optimization_level level) -> void;

} // namespace cinder::llvm_codegen
