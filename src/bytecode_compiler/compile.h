#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <unordered_map>

#include "src/bytecode/chunk.h"
#include "src/hir/nodes.h"
#include "src/parser/source_location.h"
#include "src/semantic/checker_results.h"

namespace cinder::bytecode_compiler {

/// Why `compile_module`/`compile_function` refused to produce bytecode for
/// some HIR construct. The compiler covers the full HIR language, including
/// generics (already instantiated in HIR), match, aggregates, heap-backed
/// values, closures, and generators (see
/// `spec/specification/05-implementation/66-hir-and-bytecode-vm.md`). A
/// construct it cannot yet lower fails closed with a specific reason here,
/// the same discipline `hir::lower` applies, rather than silently
/// miscompiling.
enum class compile_error_kind : uint8_t {
  unsupported_construct,   ///< A HIR construct this compiler does not lower
                           ///< yet.
  unsupported_type,        ///< A reachable scalar operation's checked type has
                           ///< no `bytecode::numeric_kind` (`numeric_kind_of`
                           ///< returned `nullopt`). Today this is a 128-bit
                           ///< integer or float, which needs two register
                           ///< slots and is not yet supported.
  unknown_callee,          ///< A call's callee could not be resolved to a
                           ///< function declared in the compiled module set.
  encoding_limit_exceeded, ///< Some count in this function exceeds what the
                           ///< corresponding operand can encode: more than 255
                           ///< `yield` points (a `u8` resume index), or an
                           ///< aggregate whose element count or byte size
                           ///< exceeds a `u16`. Notably *not* the register
                           ///< file — register operands are `u16` and the
                           ///< allocator cannot exhaust them
                           ///< (`register_alloc.h`).
  internal_error,          ///< A self-check inside the compiler failed — not
                           ///< anything the user's program did wrong. Reported
                           ///< rather than asserted so it surfaces identically
                           ///< in release builds, since the invariant it
                           ///< guards (opcodes.h's operand-layout table
                           ///< agreeing with what the emitter emits) fails
                           ///< silently and destructively if it fails at all.
};

/// One reason compilation stopped, plus where in the source it happened.
struct compile_error {
  compile_error_kind kind;
  source_span span;
  std::string message;
};

/// Compiles one lowered `hir_function` into a `bytecode_function`, resolving
/// direct calls to other functions in the same module against
/// `function_index` (function name -> `op_call`'s `function_index` operand,
/// built once per module by `compile_module` from `module.functions`'s
/// order). `types` must be the same `type_table` every `type_id` on `fn`'s
/// nodes indexes into (i.e. the `checked_types::types` the HIR was lowered
/// from) — this compiler never re-infers a type, only maps an already
/// -concrete `type_id` to a `numeric_kind`.
[[nodiscard]] auto compile_function(
    const hir::hir_function &fn, const semantic::type_table &types,
    const std::unordered_map<std::string, uint16_t> &function_index)
    -> std::expected<bytecode::bytecode_function, compile_error>;

/// Compiles every function in `module` into a `bytecode_module`, in the same
/// order, so `op_call`'s `function_index` operand can index directly into
/// the result. Direct calls resolve through this fixed compile-time table
/// (mirrors `bytecode_module`'s own doc comment in chunk.h). Fails closed on
/// the first function `compile_function` rejects.
[[nodiscard]] auto compile_module(const hir::hir_module &module,
                                  const semantic::type_table &types)
    -> std::expected<bytecode::bytecode_module, compile_error>;

/// Multi-module variant of `compile_module`, for a program whose entry
/// function calls into another module (a `hir_local_ref` with
/// `owner_module` set — see `hir::lower_call`'s `resolved_callees`
/// handling). `modules.front()` is the entry module: its own functions keep
/// their bare declared names (so `--run`/`--build`'s function-name lookup is
/// unaffected), exactly like the single-module overload. Every other
/// module's functions are keyed `module_name::name` in the combined function
/// table, and any call whose callee names a non-entry module (directly via
/// `owner_module`, or implicitly — a same-module sibling call inside a
/// non-entry module's own body) resolves against that same mangled key. The
/// single-module overload above is exactly `compile_module({&module}, types)`.
[[nodiscard]] auto
compile_module(std::span<const hir::hir_module *const> modules,
               const semantic::type_table &types)
    -> std::expected<bytecode::bytecode_module, compile_error>;

} // namespace cinder::bytecode_compiler
