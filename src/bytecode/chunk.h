#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "src/bytecode/opcodes.h"
#include "src/bytecode/value.h"

namespace kira::bytecode {

/// One compiled function: its flat instruction stream plus its own constant
/// pool (see `chunk_writer`'s doc comment for why constants are kept
/// per-function rather than module-wide). `register_count` is the total
/// size of the frame's register file — there is no separate "locals"
/// storage (see opcodes.h's top comment): parameters occupy registers
/// `[0, param_count)`, and every `let`/`var` binding *and* every
/// intermediate expression result gets the next unused register in
/// evaluation order (Decision, recorded in `spec/codegen-design.md`: no
/// register reuse across sibling scopes/subexpressions for this first cut
/// — simplest thing that works, at the cost of some wasted frame space,
/// and doesn't constrain the instruction encoding if reuse is added later).
struct bytecode_function {
  std::string name;
  uint16_t param_count = 0;
  uint16_t register_count = 0;
  std::vector<uint8_t> code;
  std::vector<slot_value> constants;
};

/// One compiled module: every function lowered from it, in an order that
/// assigns each a stable index — `op_call`'s `function_index` operand
/// indexes directly into `functions`, resolved at compile time (this
/// project's functions aren't first-class values yet, so there's no need
/// for anything more dynamic than a fixed table).
struct bytecode_module {
  std::string module_name;
  std::vector<bytecode_function> functions;
};

/// Reads a little-endian-encoded operand back out of a `bytecode_function`'s
/// `code` buffer — used by the VM (increment 2) and by tests that verify
/// `chunk_writer`'s encoding directly, without needing a VM yet. Byte order
/// is chosen explicitly here (not `std::memcpy` over the host's native
/// order) so the encoding is portable regardless of host endianness.
[[nodiscard]] auto read_u16(const std::vector<uint8_t> &code, size_t offset)
    -> uint16_t;
[[nodiscard]] auto read_u32(const std::vector<uint8_t> &code, size_t offset)
    -> uint32_t;
[[nodiscard]] auto read_i32(const std::vector<uint8_t> &code, size_t offset)
    -> int32_t;

/// Incrementally builds one `bytecode_function`'s code buffer and constant
/// pool. Constants are per-function rather than shared module-wide: it
/// keeps a function's compiled form self-contained (no cross-function
/// constant-pool bookkeeping to get wrong), at the cost of some
/// duplication when the same literal appears in two functions — an
/// acceptable trade for this first design, consistent with "simplest thing
/// that works" elsewhere in this increment (see `bytecode_function`'s doc
/// comment on register reuse).
///
/// Jump targets are patched in a second pass: emit the opcode, call
/// `emit_jump_placeholder` to reserve (and remember the offset of) the i32
/// operand, keep compiling, then call `patch_jump_to_here` once the target
/// location is known. Offsets are relative to the byte immediately
/// following the 4-byte operand (i.e. where execution would resume if the
/// jump were not taken) — the same convention `op_jump`/`op_jump_if_false`/
/// `op_jump_if_true` document in opcodes.h.
class chunk_writer {
public:
  auto emit_opcode(opcode op) -> void;
  auto emit_u8(uint8_t value) -> void;
  auto emit_u16(uint16_t value) -> void;
  auto emit_i32(int32_t value) -> void;
  auto emit_numeric_kind(numeric_kind kind) -> void;

  /// Reserves a 4-byte jump-offset operand, returning its byte offset for a
  /// later `patch_jump_to_here` call.
  [[nodiscard]] auto emit_jump_placeholder() -> size_t;
  /// Patches the placeholder at `placeholder_offset` so the jump lands at
  /// the current end of the code buffer.
  auto patch_jump_to_here(size_t placeholder_offset) -> void;

  /// Appends `value` to this function's constant pool, returning its index.
  /// Never deduplicates — a compiler that wants pooling can do so itself
  /// before calling this; this class only owns encoding, not policy.
  [[nodiscard]] auto add_constant(slot_value value) -> uint16_t;

  [[nodiscard]] auto current_offset() const noexcept -> size_t {
    return code_.size();
  }

  /// Finalizes this writer into a `bytecode_function`. Consumes the writer
  /// (rvalue-qualified) since there's nothing left to build after this.
  [[nodiscard]] auto finish(std::string name, uint16_t param_count,
                            uint16_t register_count) && -> bytecode_function;

private:
  std::vector<uint8_t> code_;
  std::vector<slot_value> constants_;
};

} // namespace kira::bytecode
