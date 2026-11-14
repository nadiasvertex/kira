#pragma once

#include <cstdint>
#include <string>

#include "src/parser/source_location.h"

namespace cinder::bytecode_compiler {

/// Why `compile_storage_module` refused to produce bytecode for a function of
/// the storage form (see
/// `spec/specification/05-implementation/66-hir-and-bytecode-vm.md`). A
/// construct it cannot yet compile fails closed with a specific reason here,
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

} // namespace cinder::bytecode_compiler
