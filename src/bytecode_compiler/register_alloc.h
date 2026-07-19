#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace kira::bytecode_compiler {

// ==========================================================================
//  Linear scan register allocation (Poletto & Sarkar, TOPLAS 1999).
//
//  `function_compiler` emits into an *unbounded* virtual register space and
//  never reuses a virtual, which is the simplest thing that works and was
//  what `chunk.h`'s `bytecode_function` doc comment originally described as
//  the whole story ("every `let`/`var` binding *and* every intermediate
//  expression result gets the next unused register"). The cost of that was
//  not wasted frame space — it was a hard ceiling: `opcodes.h` encodes a
//  register index as a `u8`, so a function that *allocated* its 257th
//  temporary failed to compile even when only a handful were ever live at
//  once. Ordinary straight-line arithmetic burns a virtual per
//  subexpression, so the ceiling was reachable by unremarkable code, and
//  several `src/testdata/codegen_stress` corpus files are split across
//  extra `def`s purely to stay under it.
//
//  This pass closes that gap by mapping virtuals onto physical registers,
//  reusing a physical as soon as the virtual holding it is dead. Linear
//  scan is the right algorithm for this compiler specifically: it is
//  O(n log n), it needs no interference graph, and — the reason that
//  matters here — its whole premise is that live ranges are approximated as
//  *intervals over a linear ordering of the code*, which is exactly what a
//  single flat instruction stream with no basic-block structure already
//  gives us. Graph colouring would need a CFG this compiler never builds.
//
//  Four things about this instruction set constrain the classic algorithm,
//  and all four are handled below rather than papered over:
//
//  - **Contiguity.** `op_call`/`op_call_intrinsic`/`op_call_indirect` read
//    their arguments from `argc` *consecutive* registers. So some virtuals
//    cannot be allocated independently; see `register_group`.
//  - **Pinning.** The calling convention fixes the first `param_count`
//    physicals at entry (a closure's env pointer at 0, a generator step's
//    three reserved registers), so those virtuals must be identity-mapped.
//  - **Address-taken.** `op_addr_local` yields the address of a *frame
//    register*, which can be stored and read back through arbitrarily much
//    later. Last-mention is not a bound on such a register's lifetime; see
//    `address_taken`.
//  - **Loops.** Interval endpoints come from first/last mention in the
//    linear order, which is unsound across a back edge: a virtual defined
//    before a loop and last read early inside it looks dead for the rest of
//    the loop body, so its physical gets reused — and the next iteration
//    reads the clobbered value. See `loop_range`.
//
//  There is deliberately **no spilling**, and unlike most linear-scan
//  implementations this one needs none: register operands are `u16`
//  (opcodes.h), so a frame can address 65536 registers while the compiler
//  hands out at most 65535 virtuals. The active set cannot exceed the
//  register file, so there is never anything to evict.
//
//  Spilling was the original plan, and widening the operand replaced it on
//  purpose. Spilling is the smaller change, but it only ever executes above
//  256 simultaneously-live values — a path no ordinary program takes, that
//  cross-backend agreement cannot check (only this tier would spill), and
//  whose bugs would therefore sit undiscovered. Widening is the larger
//  change with the louder failure: get an operand width wrong and the
//  instruction stream desynchronizes immediately, on every program.
//
//  What that leaves this pass responsible for is frame *size* only. It is
//  an optimization now, not a correctness requirement — but the constraints
//  below are still load-bearing, because they are about choosing the right
//  register, not about running out of them.
// ==========================================================================

/// A virtual register id, unbounded during compilation.
///
/// A distinct type rather than a bare `uint32_t` on purpose: before this
/// pass existed, a register operand was a `uint8_t` written straight to the
/// code buffer with `chunk_writer::emit_u8`, and virtuals now outrun that
/// width. Had this stayed an integer alias, every one of those ~90 emit
/// sites would have kept compiling and silently truncated. Being a struct
/// makes each one a compile error until it is converted to
/// `function_compiler::emit_register`, which is how they were all found.
struct virtual_reg {
  uint32_t id = 0;

  friend auto operator==(virtual_reg, virtual_reg) -> bool = default;
};

/// One register operand in the emitted byte stream: the byte at
/// `code_offset` is a placeholder to be overwritten with `reg`'s assigned
/// physical register once allocation has run.
struct register_site {
  size_t code_offset = 0;
  virtual_reg reg;
};

/// A run of `count` virtuals starting at `first` that must be allocated to
/// *consecutive* physical registers, because a call opcode will read them as
/// one contiguous argument block.
///
/// Members are allocated and freed as a unit, over the union of their
/// individual intervals. That is more conservative than allocating each
/// separately — the whole run stays live until the last member dies — but a
/// contiguity constraint is not expressible in scalar linear scan at all,
/// and argument blocks are short-lived enough that the imprecision costs
/// little.
struct register_group {
  virtual_reg first;
  uint32_t count = 0;
};

/// A loop, as a range of *byte offsets*: `header` is where the back edge
/// jumps to, `back_edge` the offset of the jump itself. Both are mapped onto
/// instruction indices internally, the same way a `register_site`'s offset
/// is — the compiler records offsets because that is what it has on hand
/// while emitting, and one conversion rule serves both.
///
/// Any interval overlapping this range is extended to `back_edge`, which is
/// what makes first/last-mention intervals sound in the presence of
/// backward control flow. Recorded explicitly by the compiler at the three
/// sites that emit a backward jump rather than recovered by decoding the
/// byte stream — the compiler already knows where its loops are, and
/// re-deriving that from jump offsets would mean teaching this pass the
/// operand layout of every opcode just to find instruction boundaries.
struct loop_range {
  size_t header = 0;
  size_t back_edge = 0;
};

/// Everything the allocator needs about one compiled function.
struct allocation_input {
  /// Total virtuals handed out; ids are dense in `[0, virtual_count)`.
  uint32_t virtual_count = 0;
  /// Virtuals `[0, pinned_prefix)` are identity-mapped to physicals, as the
  /// calling convention requires at entry. Always the function's
  /// `param_count` (which for a closure body counts the env pointer, and for
  /// a generator step the three reserved registers).
  uint32_t pinned_prefix = 0;
  /// Byte offset of each instruction's opcode, ascending. Used only to map
  /// a `register_site`'s byte offset onto an instruction index.
  std::vector<size_t> instruction_offsets;
  std::vector<register_site> sites;
  std::vector<loop_range> loops;
  std::vector<register_group> groups;
  /// Virtuals whose address `op_addr_local` took. Live to the end of the
  /// function regardless of last mention.
  std::vector<virtual_reg> address_taken;
};

struct allocation_result {
  /// `assignment[v]` is virtual `v`'s physical register.
  std::vector<uint16_t> assignment;
  /// Frame size: one past the highest physical actually used.
  uint16_t register_count = 0;
};

/// Assigns every virtual in `input` a physical register, reusing physicals
/// across virtuals whose live intervals do not overlap.
///
/// This cannot fail, and deliberately returns no `std::expected` to say so.
/// It used to: a `u8` register operand gave a frame 256 addressable
/// registers, and a function needing more was a hard compile error the user
/// could do nothing about except split the function by hand. Widening the
/// operand to `u16` (see `opcodes.h`'s `k_register_operand_bytes`) makes
/// exhaustion unreachable by construction rather than merely unlikely —
/// `function_compiler` caps virtuals at `k_max_virtual_registers` (65535),
/// which is strictly below the physicals available here, so the worst case
/// where no virtual shares with any other still fits.
///
/// That demotes this pass from a correctness requirement to an
/// optimization: it now only shrinks frames. Every constraint it honors
/// (call-argument contiguity, the pinned parameter prefix, address-taken
/// values, loop-carried liveness) remains load-bearing, because those are
/// about assigning the *right* register, not about running out of them.
[[nodiscard]] auto allocate_registers(const allocation_input &input)
    -> allocation_result;

/// The number of physical registers a frame can address — `opcodes.h`
/// encodes every register operand as a `u16`.
inline constexpr uint32_t k_physical_register_count = 65536;

} // namespace kira::bytecode_compiler
