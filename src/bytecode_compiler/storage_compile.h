#pragma once

#include <expected>

#include "src/bytecode/chunk.h"
#include "src/bytecode_compiler/compile.h"
#include "src/hir/storage.h"

namespace cinder::bytecode_compiler {

// ==========================================================================
//  Bytecode from the shared storage form (representation v2).
//
//  `compile_function` and `compile_module` in `compile.h` build bytecode from
//  typed HIR, where every aggregate is a pointer to a heap block. This
//  compiler builds it from `hir::storage_function`s instead: an aggregate or
//  descriptor lives as inline bytes in its owner, and a call builds its result
//  directly in storage the caller supplies.
//
//  Where values live:
//
//  - A scalar, address, or handle that nothing takes the address of lives in a
//    register.
//  - Everything else a function owns (aggregates, descriptors, and scalars
//    whose address is taken) lives in the frame's byte storage
//    (`bytecode_function::stack_byte_size`), at an offset fixed at compile
//    time. That storage is aligned, zeroed when the frame is created, and its
//    address does not change while the frame lives, including when the frame
//    vector grows.
//  - A parameter that travels by address, and the hidden result, live in the
//    caller's bytes; the register holds the address.
//
//  Registers `[0, param_count)` carry the internal signature's operands in
//  order: the hidden result address (when there is one), then each
//  non-zero-sized source parameter. `bytecode_function::abi` records the
//  sizes and classes so a host can call a function with the right storage.
//
//  The two representations must not meet in one program, so a module compiled
//  here is called only by modules compiled here. Nothing in the driver selects
//  this compiler yet.
// ==========================================================================

/// Compiles every function of `module` (in order, so a call resolves against
/// the module's own function table) from its storage form.
///
/// Each function should already satisfy `hir::verify`: this compiler relies on
/// it for initialization, ownership and place validity and does not re-check
/// them. It fails closed with a named reason on anything it cannot place:
/// an indirect call through a `fn` value, a call into another module, a
/// `drop_in_place`, a slot that does not live in the frame or the caller, a
/// 128-bit scalar, or an offset or frame size beyond what an operand encodes.
[[nodiscard]] auto compile_storage_module(const hir::storage_module &module,
                                          hir::storage_env &env)
    -> std::expected<bytecode::bytecode_module, compile_error>;

} // namespace cinder::bytecode_compiler
