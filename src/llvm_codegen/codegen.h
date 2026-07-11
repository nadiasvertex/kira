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
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#pragma clang diagnostic pop

#include "src/hir/nodes.h"
#include "src/parser/source_location.h"
#include "src/semantic/types.h"

namespace kira::llvm_codegen {

/// Why `compile_module` refused to produce an `llvm::Module` for some HIR
/// construct. Mirrors `bytecode_compiler::compile_error_kind` deliberately
/// (`src/bytecode_compiler/compile.h`) — this pass targets the exact same
/// `spec/codegen-design.md` increment-1 scalar/control-flow subset, so a
/// construct outside that scope fails closed here for the same reason it
/// does there, not because the two compilers happen to duplicate a design
/// choice independently.
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
/// `llvm::TargetMachine::addPassesToEmitFile` for the (not-yet-built) AOT
/// path (`spec/codegen-design.md` increment 4).
struct compiled_module {
  std::unique_ptr<llvm::LLVMContext> context;
  std::unique_ptr<llvm::Module> module;
};

/// The exact symbol name generated IR calls to report a panic
/// (`spec/codegen-design.md` Decision 3's runtime-ABI sharing in miniature:
/// both tiers raise the same closed `bytecode::panic_reason` set). Defined
/// as an ordinary C++ function (`runtime.cpp`) the JIT resolves via process
/// symbols — see `jit_support.h` for the execution side of this contract.
inline constexpr const char *kPanicSymbolName = "kira_codegen_panic";

/// The exact symbol name generated IR calls to allocate a heap value
/// (`spec/codegen-design.md` Decision 3/increment 6) — every non-scalar
/// value (`str`, `list[T]`, tuple/struct/sum-type/closure) is allocated
/// through this one entry point, defined in `src/runtime/arena.h` and
/// resolved the same way `kPanicSymbolName` already is (process-symbol
/// lookup for the JIT, ordinary static linking for `kira build`).
inline constexpr const char *kAllocSymbolName = "kira_rt_alloc";

/// The exact symbol name generated IR calls to grow a `list[T]` value and
/// reserve its next element slot (`spec/codegen-design.md` increment 5) —
/// defined in `src/runtime/layout.h`, resolved the same way
/// `kAllocSymbolName` already is. Returns the address to store the pushed
/// value at directly, rather than taking the value itself as a parameter,
/// so the runtime side never needs to know the pushed value's concrete
/// LLVM type (see `list_reserve_slot`'s own doc comment).
inline constexpr const char *kListReserveSlotSymbolName =
    "kira_rt_list_reserve_slot";

/// Lowers every function in `module` into one `llvm::Module` named after
/// `module.module_name`, resolving direct calls to other functions in the
/// same module by declaring every function's signature up front (a first
/// pass over `module.functions`) before compiling any body — unlike
/// `bytecode_compiler` (which resolves calls through a name -> index table
/// at its own pace), real `llvm::Function` values must exist before a call
/// instruction can reference them, including for forward references and
/// mutual recursion. `types` must be the same `type_table` every `type_id`
/// on `module`'s nodes indexes into.
[[nodiscard]] auto compile_module(const hir::hir_module &module,
                                  const semantic::type_table &types)
    -> std::expected<compiled_module, codegen_error>;

/// Multi-module variant of `compile_module`, mirroring
/// `bytecode_compiler::compile_module`'s span overload exactly (same
/// entry-module-is-bare, everyone-else-is-`module::name`-mangled scheme —
/// see its doc comment in `src/bytecode_compiler/compile.h`). `modules.
/// front()` is the entry module; the produced `llvm::Module` is named after
/// it. The single-module overload above is exactly
/// `compile_module({&module}, types)`.
[[nodiscard]] auto
compile_module(std::span<const hir::hir_module *const> modules,
               const semantic::type_table &types)
    -> std::expected<compiled_module, codegen_error>;

// ==========================================================================
//  Optimization — an opt-in `llvm::PassBuilder` pipeline run over a
//  `compiled_module`'s IR before it's JIT-executed or emitted as a native
//  object file. `compile_module` above never runs any optimization itself
//  (matches `spec/codegen-design.md`'s "compiler is a teacher" philosophy,
//  CLAUDE.md — the generated IR should stay a direct, readable reflection
//  of the source); `optimize_module` is a separate, explicit step a caller
//  opts into via `-O1`/`-O2`/`-O3` (`driver::cli_config::opt_level`).
// ==========================================================================

/// Mirrors clang/gcc's `-O0`/`-O1`/`-O2`/`-O3` convention and maps 1:1 onto
/// `llvm::OptimizationLevel` (see `optimize_module`'s definition in
/// `codegen.cpp` for the actual `llvm::PassBuilder` pipeline — kept out of
/// this header so callers that only need `compiled_module`/`compile_module`
/// don't have to pull in `<llvm/Passes/PassBuilder.h>`). `o0` — no
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

} // namespace kira::llvm_codegen
