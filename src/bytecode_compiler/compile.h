#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <unordered_map>

#include "src/bytecode/chunk.h"
#include "src/hir/nodes.h"
#include "src/parser/source_location.h"
#include "src/semantic/types.h"

namespace kira::bytecode_compiler {

/// Why `compile_module`/`compile_function` refused to produce bytecode for
/// some HIR construct. This compiler targets exactly
/// `spec/codegen-design.md` increment 1's scope — the non-generic,
/// non-concurrent, scalar/control-flow subset (arithmetic, comparisons,
/// casts, `if`, `while`, direct calls) — even though HIR itself has since
/// grown match, tuples, structs, arrays, lambdas, and sum-type variants for
/// later increments. Anything outside that scope fails closed with a
/// specific reason here, the same discipline `hir::lower` already applies
/// (`spec/typed-ir-design.md` Decision 1), rather than silently miscompiling
/// or guessing a representation for a construct increment 6 (heap types)
/// hasn't designed yet.
enum class compile_error_kind : uint8_t {
  unsupported_construct,   ///< Outside increment 1's scope — match, tuples,
                           ///< structs, arrays, lambdas, variants, field/index
                           ///< access, references, destructuring, string
                           ///< literals, and every construct this increment
                           ///< doesn't lower yet.
  unsupported_type,        ///< A reachable expression's checked type has no
                           ///< `bytecode::numeric_kind` (`numeric_kind_of`
                           ///< returned `nullopt`) — a non-scalar type
                           ///< (`list[T]`, `str`, a struct/sum type, ...) or a
                           ///< 128-bit scalar, neither representable by this
                           ///< tier's scalar-only `slot_value` yet.
  unknown_callee,          ///< A call's callee could not be resolved to a
                           ///< function declared in the same compiled module.
  register_limit_exceeded, ///< This function needed more than 256 registers
                           ///< (`opcodes.h`'s `u8` register-index encoding)
                           ///< — revisit the encoding width if a real
                           ///< program ever gets close.
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
/// the result (no first-class function values in this increment, so a
/// fixed compile-time table is all calls ever need — mirrors
/// `bytecode_module`'s own doc comment in chunk.h). Fails closed on the
/// first function `compile_function` rejects.
[[nodiscard]] auto compile_module(const hir::hir_module &module,
                                  const semantic::type_table &types)
    -> std::expected<bytecode::bytecode_module, compile_error>;

} // namespace kira::bytecode_compiler
