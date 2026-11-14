#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "src/bytecode/opcodes.h"
#include "src/bytecode/value.h"
#include "src/parser/source_location.h"

namespace cinder::bytecode {

/// One compiled function: its flat instruction stream plus its own constant
/// pool (see `chunk_writer`'s doc comment for why constants are kept
/// per-function rather than module-wide). `register_count` is the total
/// size of the frame's register file — there is no separate "locals"
/// storage (see opcodes.h's top comment): parameters occupy registers
/// `[0, param_count)`, and every `let`/`var` binding and every intermediate
/// expression result lives in one of the registers above them.
///
/// This count is the number of values the function holds live *at once*,
/// not the number it ever names. The compiler emits into an unbounded
/// virtual register space and `bytecode_compiler/register_alloc.h` maps
/// those onto physical registers by linear scan, recycling one as soon as
/// the value in it dies. It originally did not: each binding and
/// subexpression took the next unused register and kept it, which made the
/// register operand's width a ceiling on how many values a function could
/// *mention* rather than how many it needed simultaneously — reachable by
/// unremarkable straight-line code, and the reason several
/// `src/testdata/codegen_stress` files are split across extra `def`s.
///
/// This field was always a `uint16_t` and the VM has always sized frames
/// straight from it, so the 256-register ceiling was never a property of
/// the frame — only of the `u8` operand that named a register. That operand
/// is a `u16` now (opcodes.h), which is what makes the two agree.
/// One entry of a function's source map: the instructions from `offset` up
/// to the next entry's were compiled from `span`.
struct span_entry {
  uint32_t offset = 0;
  source_span span;
};

/// One `op_host_call` instruction's operation, as `hir_host_call` described
/// it: what to do, the syntax it is about, and its operands' and result's
/// types, so the host can read the operands and write the result.
struct host_site {
  std::string operation;
  const void *subject = nullptr;
  std::vector<semantic::type_id> arg_types;
  semantic::type_id result_type = semantic::k_unknown_type;
  source_span span;
  /// Set by the storage-form compiler, where the operands follow the call
  /// ABI (`function_abi`): the address of the result's bytes first when
  /// `result_by_address`, then each argument that is not zero-sized, as its
  /// value or (`arg_by_address[i]`) the address of its bytes. The host then
  /// writes a result built by address and returns nothing.
  bool storage_form = false;
  bool result_by_address = false;
  std::vector<bool> arg_by_address;
  std::vector<bool> arg_zero_sized;
};

/// How one value crosses a call boundary in the inline-value representation.
/// A value that travels by address is the address of caller-owned bytes of
/// `size` bytes aligned to `align`; any other value is one register.
struct value_transport {
  uint32_t size = 0;
  uint32_t align = 1;
  bool by_address = false;

  [[nodiscard]] auto operator==(const value_transport &) const
      -> bool = default;
};

/// A function's calling convention in the inline-value representation, kept
/// beside the code so a host can call it without guessing. The hidden result
/// address, when there is one, is register 0 and is not listed in `params`;
/// `params` holds the source parameters in order, zero-sized ones included
/// (they occupy no register). Absent from a function that still uses the
/// pointer-per-aggregate representation.
struct function_abi {
  /// The callee builds its result in storage the caller supplies.
  bool returns_through_address = false;
  /// The result value: its size and alignment, and whether it is built through
  /// `returns_through_address` storage.
  value_transport result;
  std::vector<value_transport> params;
};

struct bytecode_function {
  std::string name;
  uint16_t param_count = 0;
  uint32_t register_count = 0;
  std::vector<uint8_t> code;
  std::vector<slot_value> constants;
  /// String-literal bytes referenced by `op_str_const_data` (opcodes.h).
  /// Kept separate from `constants` (which only holds scalar `slot_value`s)
  /// since a string's bytes have no fixed width — this function outlives
  /// any VM run built from it, so a `str` descriptor's data can point
  /// directly at one of these entries without copying its bytes.
  std::vector<std::string> string_constants;
  /// Indexed by `op_host_call`'s `site` operand.
  std::vector<host_site> host_sites;
  /// Total bytes of frame-local storage this function's `uninit[T, N]`
  /// buffers need (`op_stack_alloc`). Reserved once, on frame entry, rather
  /// than grown per allocation: a buffer's address must stay valid for the
  /// whole frame, and a growing `std::vector` would move it out from under
  /// any pointer already taken into it. Every `op_stack_alloc` therefore
  /// carries a *fixed offset* into this range, assigned at compile time.
  uint32_t stack_byte_size = 0;
  /// The calling convention of an inline-value function; see `function_abi`.
  /// `param_count` counts the hidden result address when there is one.
  std::optional<function_abi> abi;
  /// Where the code came from, for reporting a failure at a source line: the
  /// module and top-level HIR function it was compiled from (a lambda's or a
  /// generator step's enclosing function, for one of those), and, in code
  /// order, the source span each run of instructions was compiled from.
  /// Spans are byte offsets into that function's file.
  std::string source_module;
  std::string source_function;
  std::vector<span_entry> spans;
};

/// The span the instruction at `offset` in `fn` was compiled from, or an
/// empty span when `fn` has no source map.
[[nodiscard]] auto span_at(const bytecode_function &fn, size_t offset)
    -> source_span;

/// One compiled module: every function lowered from it, in an order that
/// assigns each a stable index — `op_call`'s `function_index` operand
/// indexes directly into `functions`, resolved at compile time (this
/// project's functions aren't first-class values yet, so there's no need
/// for anything more dynamic than a fixed table).
struct bytecode_module {
  std::string module_name;
  std::vector<bytecode_function> functions;
  /// The bytes of every module's statics, laid out
  /// at offsets the compiler chose (`op_global_addr`), each at its type's
  /// alignment. The `vm` allocates them zeroed and 16-byte aligned.
  size_t global_byte_size = 0;
  /// The function that builds each module's statics
  /// (`hir::k_static_init_name`), in module order. A host runs each once,
  /// on the `vm` it will use, before anything else.
  std::vector<uint16_t> static_initializers;
};

/// Reads a little-endian-encoded operand back out of a `bytecode_function`'s
/// `code` buffer — used by the VM and by tests that verify
/// `chunk_writer`'s encoding directly, without needing a VM yet. Byte order
/// is chosen explicitly here (not `std::memcpy` over the host's native
/// order) so the encoding is portable regardless of host endianness.
[[nodiscard]] auto read_u16(const std::vector<uint8_t> &code, size_t offset)
    -> uint16_t;
[[nodiscard]] auto read_u32(const std::vector<uint8_t> &code, size_t offset)
    -> uint32_t;
[[nodiscard]] auto read_u64(const std::vector<uint8_t> &code, size_t offset)
    -> uint64_t;
[[nodiscard]] auto read_i32(const std::vector<uint8_t> &code, size_t offset)
    -> int32_t;

/// Incrementally builds one `bytecode_function`'s code buffer and constant
/// pool. Constants are per-function rather than shared module-wide: it
/// keeps a function's compiled form self-contained (no cross-function
/// constant-pool bookkeeping to get wrong), at the cost of some
/// duplication when the same literal appears in two functions — an
/// acceptable trade, consistent with "simplest thing that works" elsewhere
/// (see `bytecode_function`'s doc comment on register reuse).
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
  auto emit_u32(uint32_t value) -> void;
  auto emit_u64(uint64_t value) -> void;

  /// Emits a register operand at whatever width `k_register_operand_bytes`
  /// declares. Distinct from `emit_u8`/`emit_u16` so that "this operand is a
  /// register" is stated at the call site rather than inferred from the
  /// width that happens to be correct today — the bytecode compiler learned
  /// this the expensive way, and hand-assembled bytecode needs it just as
  /// much.
  auto emit_register(uint16_t reg) -> void;
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
  [[nodiscard]] auto add_constant(slot_value value) -> uint32_t;

  /// Appends `text` to this function's string-constant table, returning its
  /// index for `op_str_const_data`. Never deduplicates, for the same reason
  /// `add_constant` doesn't.
  [[nodiscard]] auto add_string_constant(std::string text) -> uint32_t;

  /// Appends `site` to this function's host-call table, returning its
  /// index for `op_host_call`.
  [[nodiscard]] auto add_host_site(host_site site) -> uint16_t;

  [[nodiscard]] auto current_offset() const noexcept -> size_t {
    return code_.size();
  }

  /// Finalizes this writer into a `bytecode_function`. Consumes the writer
  /// (rvalue-qualified) since there's nothing left to build after this.
  [[nodiscard]] auto finish(std::string name, uint16_t param_count,
                            uint32_t register_count) && -> bytecode_function;

private:
  std::vector<uint8_t> code_;
  std::vector<slot_value> constants_;
  std::vector<std::string> string_constants_;
  std::vector<host_site> host_sites_;
};

} // namespace cinder::bytecode
