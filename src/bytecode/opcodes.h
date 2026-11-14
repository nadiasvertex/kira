#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace cinder::bytecode {

// ==========================================================================
//  Instruction set. See
//  `spec/specification/05-implementation/66-hir-and-bytecode-vm.md`.
//
//  A register machine, not a stack machine: every operand is an explicit
//  register index into the current call frame's register file, not an
//  implicit stack top. This saves instructions (one `add dst, lhs, rhs`
//  where a stack machine needs push-push-add-pop-push) and avoids the
//  redundant push/pop pairs a stack machine's compiler must remove later.
//
//  A function's registers are one flat array per call frame — there is no
//  separate "locals" storage distinct from "temporaries": parameters
//  occupy registers `[0, param_count)` at call entry, and every `let`/
//  `var` binding *and* every intermediate expression result lives in one of
//  the registers above them. `bytecode_function::register_count` (chunk.h)
//  is the size of that array.
//
//  Registers are assigned by linear scan (`bytecode_compiler/
//  register_alloc.h`), so one is reused as soon as the value occupying it
//  is dead. That replaces the original "next unused register,
//  monotonically, revisit if it matters" rule, which turned out to matter
//  sooner than expected: without reuse the `u8` operand width below capped
//  a function at 256 registers *ever allocated*, not 256 live at once, and
//  ordinary code reached that.
//
//  Every arithmetic/comparison/bitwise opcode is still parameterized by a
//  trailing `numeric_kind` immediate byte (see value.h) rather than one
//  opcode per scalar width, for the reason recorded when that decision was
//  made: `CLAUDE.md`'s explicit priority ("clarity... over performance or
//  cleverness") plus Decision 4 already routing hot code to the LLVM tier,
//  so this tier doesn't need to win a dispatch-speed contest down to the
//  width-specialized-opcode level. Switching to a register machine doesn't
//  reopen that — it's an orthogonal axis (how operands are addressed, not
//  how many opcodes exist per width).
//
//  Operand encoding is fixed-width little-endian, appended directly after
//  the one-byte opcode: `u16` for a register index, `u16` for a
//  constant-pool or function-table index, `u8` for a small tag or count
//  (`numeric_kind`, `field_size`, `argc`, ...), `i32` for a relative jump
//  offset. `operands_of` at the bottom of this file is the single
//  description of which is which, and every instruction size and offset is
//  derived from it.
//
//  The register index is a `u16`. A `u8` index (256 registers per frame)
//  was too small for real programs. See the operand-layout section below
//  for why widening beat spilling.
//
//  Two hardware-inspired encoding ideas were considered for this
//  instruction set and rejected — recorded here so they aren't
//  re-litigated later without the reasoning that closed them off:
//
//  - **VLIW-style instruction bundling** (packing multiple operations into
//    one wide "word" for a compiler to statically schedule across parallel
//    functional units, the way Itanium/TI DSPs do): doesn't transfer to a
//    software interpreter. The whole point of VLIW is to let hardware skip
//    building a dynamic (out-of-order) scheduler by pushing scheduling to
//    the compiler; here, the interpreter is *itself* just sequential code
//    running on a host CPU that already has its own superscalar/OoO engine
//    extracting whatever parallelism exists in the interpreter's compiled
//    dispatch loop — there's no analog of "N functional units the
//    interpreter controls in parallel" for bytecode ops to be bundled
//    onto. It also fails for the same structural reason VLIW struggled on
//    general-purpose hardware (Itanium being the canonical case): static
//    scheduling needs predictable branches and latencies, and a bytecode
//    stream — dispatch-heavy, full of `if`/`while`/match branches and
//    (once heap types land) unpredictable-latency loads — is exactly the
//    shape VLIW handles worst. The one related idea worth keeping on the
//    roadmap is *superinstructions* (hand- or profile-fused opcodes for
//    common adjacent sequences, the way CPython 3.11+ specialization
//    works) — a real, separate technique, worth adding once a working
//    interpreter's profile shows dispatch count actually dominates, not
//    designed in up front. The register machine above already captures
//    most of the same win structurally: fewer total instructions than the
//    equivalent stack-machine encoding, with no separate push/pop pairs to
//    fuse away in the first place.
//  - **Per-instruction predicate bits** (execute-if-true operands, the way
//    ARM's classic condition codes or Itanium's predicate registers work,
//    to convert an unpredictable branch into branchless data flow):
//    rejected for a different, sharper reason than VLIW. A predicated
//    instruction whose predicate is false must still be *dispatched* —
//    the interpreter still fetches, decodes, and executes its handler,
//    just suppressing the write — so predication trades "skip these N
//    instructions with one cheap `pc` adjustment" (what `op_jump`/
//    `op_jump_if_false` already do) for "dispatch all N instructions from
//    *both* arms unconditionally." That's strictly more total dispatch
//    overhead, not less, for a software interpreter — the CPU-level
//    branch-misprediction cost predication exists to avoid on real
//    hardware isn't actually attached to the bytecode's own conditional
//    jumps here; the interpreter already eats one hard-to-predict
//    indirect dispatch per instruction *regardless* of whether that
//    instruction happens to be a jump, since the next opcode's handler
//    address varies instruction to instruction either way. Worse,
//    predication doesn't compose with this language's checked-arithmetic
//    semantics (`spec/cinder-reference.md`'s "Integer Overflow" — plain
//    `+`/`-`/`*` panic on overflow): suppressing a panic for a false
//    predicate requires the exact same conditional check predication was
//    supposed to eliminate, so there's no branchless win available even
//    in principle for this language's arithmetic. Where predication
//    genuinely earns its keep — real hardware executing compiled native
//    code with a real branch-predictor pipeline — is already handled
//    downstream: LLVM performs its own branch-to-`select` if-conversion
//    in the Decision-4 JIT/AOT tier when it's profitable, so there's
//    nothing to duplicate in the bytecode format itself. The one
//    genuinely good idea adjacent to this — a dedicated branchless
//    `op_select dst, cond, a, b` for pure value-producing ternaries with
//    no side effects on either arm (unlike full predication, this doesn't
//    need to suppress anything, so it doesn't hit the checked-arithmetic
//    problem above, and it's fewer total dispatches than jump+move+move,
//    not more) — is plausible future scope, not yet added to the enum
//    below pending a decision on whether it's worth the encoding space now
//    versus when the compiler's `hir_if`-as-expression lowering exists to
//    use it.
// ==========================================================================
enum class opcode : uint8_t {
  // --- Constants and register-to-register moves --------------------------
  op_load_const, ///< u16 dst, u32 const_index — reg[dst] = constants[idx].
  op_move,       ///< u16 dst, u16 src — reg[dst] = reg[src].

  // --- Checked arithmetic (panics on overflow/div-by-zero for integer
  //     kinds, per spec/cinder-reference.md's "Integer Overflow" section;
  //     plain IEEE semantics — no panic, may produce inf/nan — for float
  //     kinds) -----------------------------------------------------------
  op_add, ///< u16 dst, u16 lhs, u16 rhs, u8 numeric_kind — reg[dst] = lhs+rhs.
  op_sub, ///< u16 dst, u16 lhs, u16 rhs, u8 numeric_kind.
  op_mul, ///< u16 dst, u16 lhs, u16 rhs, u8 numeric_kind.
  op_div, ///< u16 dst, u16 lhs, u16 rhs, u8 numeric_kind.
  op_mod, ///< u16 dst, u16 lhs, u16 rhs, u8 numeric_kind.
  op_neg, ///< u16 dst, u16 src, u8 numeric_kind — checked for integers.

  // --- Wrapping arithmetic (`+%`/`-%`/`*%` — integer kinds only; never
  //     called with a float numeric_kind) --------------------------------
  op_add_wrap, ///< u16 dst, u16 lhs, u16 rhs, u8 numeric_kind.
  op_sub_wrap, ///< u16 dst, u16 lhs, u16 rhs, u8 numeric_kind.
  op_mul_wrap, ///< u16 dst, u16 lhs, u16 rhs, u8 numeric_kind.

  // --- Saturating arithmetic (`+|`/`-|`/`*|` — integer kinds only) ------
  op_add_sat, ///< u16 dst, u16 lhs, u16 rhs, u8 numeric_kind.
  op_sub_sat, ///< u16 dst, u16 lhs, u16 rhs, u8 numeric_kind.
  op_mul_sat, ///< u16 dst, u16 lhs, u16 rhs, u8 numeric_kind.

  // --- Bitwise / shift (integer kinds only) ------------------------------
  op_bitand, ///< u16 dst, u16 lhs, u16 rhs, u8 numeric_kind.
  op_bitor,  ///< u16 dst, u16 lhs, u16 rhs, u8 numeric_kind.
  op_bitxor, ///< u16 dst, u16 lhs, u16 rhs, u8 numeric_kind.
  op_bitnot, ///< u16 dst, u16 src, u8 numeric_kind.
  op_shl,    ///< u16 dst, u16 lhs, u16 rhs, u8 numeric_kind.
  op_shr,    ///< u16 dst, u16 lhs, u16 rhs, u8 numeric_kind — arithmetic for
             ///< signed kinds, logical for unsigned.

  // --- Logical (operates on `numeric_kind::boolean` registers only;
  //     `and`/`or` themselves never reach here — see below, they compile
  //     to jumps) ----------------------------------------------------------
  op_not_bool, ///< u16 dst, u16 src.

  // --- Comparison (result is always a `boolean` register) ----------------
  op_eq, ///< u16 dst, u16 lhs, u16 rhs, u8 numeric_kind.
  op_ne, ///< u16 dst, u16 lhs, u16 rhs, u8 numeric_kind.
  op_lt, ///< u16 dst, u16 lhs, u16 rhs, u8 numeric_kind.
  op_le, ///< u16 dst, u16 lhs, u16 rhs, u8 numeric_kind.
  op_gt, ///< u16 dst, u16 lhs, u16 rhs, u8 numeric_kind.
  op_ge, ///< u16 dst, u16 lhs, u16 rhs, u8 numeric_kind.

  // --- Cast ---------------------------------------------------------------
  op_cast, ///< u16 dst, u16 src, u8 from_kind, u8 to_kind — converts reg[src]
           ///< (as from_kind) into reg[dst] (as to_kind). Integer-to-
           ///< integer is truncate-or-extend; integer-to-float and
           ///< float-to-integer follow ordinary C++ conversion semantics;
           ///< float-to-float is widen/narrow.

  // --- Control flow --------------------------------------------------------
  //
  //  `if`/`while`/`while let` compile to these directly. `and`/`or`
  //  (`ast::binary_op::And`/`Or`) are lowered to `hir_binary` at the HIR
  //  level with no short-circuit marker at all (confirmed by reading
  //  `hir::lower_binary` — both operands are unconditionally lowered
  //  through the same generic path as every other binary op); this
  //  bytecode compiler chooses to give them short-circuit evaluation
  //  anyway via `op_jump_if_false`/`op_jump_if_true` rather than "evaluate
  //  both operands, then boolean-and/or the results" — the two are only
  //  observably different when an operand has a side effect (a call, or an
  //  assignment-expression), and short-circuit is what every mainstream
  //  language's `&&`/`||` means. This is a backend-only decision — it does
  //  not require (and did not get) any HIR-level change.
  op_jump,          ///< i32 relative offset (from the position immediately
                    ///< after this instruction's operand) — unconditional.
  op_jump_if_false, ///< u16 cond, i32 relative offset — jump if reg[cond]
                    ///< (a `boolean` register) is false.
  op_jump_if_true,  ///< u16 cond, i32 relative offset — jump if reg[cond]
                    ///< is true.

  // --- Calls and returns ---------------------------------------------------
  op_call, ///< u16 dst, u16 function_index, u16 first_arg_reg, u8 argc —
           ///< copies `argc` consecutive registers starting at
           ///< `first_arg_reg` (in the *caller's* frame) into the new
           ///< callee frame's registers `[0, argc)`, transfers control to
           ///< `function_index`'s entry, and on return copies the callee's
           ///< result into the caller's `dst` register (unused/untouched
           ///< for a callee compiled with `op_return_unit`).
  op_return_value, ///< u16 src — pop the current frame, hand reg[src] back
                   ///< to the caller as the call's result.
  op_return_unit,  ///< (no operands) — pop the current frame with no value
                   ///< to hand back (the callee's return type is `unit`).

  op_tail_call, ///< u16 function_index, u16 first_arg_reg, u8 argc — same
                ///< operand layout as `op_call` minus the destination
                ///< register, since a tail call's result flows to the
                ///< *current* frame's caller, not to a register in the
                ///< current frame. Reuses the top frame in place instead of
                ///< pushing a new one (see `vm::run`): call depth
                ///< (`frames.size()`) does not grow, which is the whole of
                ///< the VM's tail-call guarantee (spec/specification/
                ///< 03-advanced/39-tail-call-optimization.md). Emitted, with
                ///< no following return opcode, exactly at HIR-marked tail
                ///< positions in place of an ordinary call plus return.

  op_call_intrinsic, ///< u16 dst, u8 intrinsic_id, u16 first_arg_reg, u8 argc
                     ///< — calls the native implementation of the intrinsic
                     ///< at index `intrinsic_id` into
                     ///< `cinder::known_intrinsic_names` (src/intrinsics.h)
                     ///< with `argc` consecutive registers starting at
                     ///< `first_arg_reg`, writing its result into `dst`. No
                     ///< frame is pushed — an intrinsic is a direct C++
                     ///< call inside the VM's own dispatch loop, not
                     ///< another `bytecode_function`. Parameterized by
                     ///< intrinsic id rather than one opcode per intrinsic,
                     ///< the same "parameterize, don't enumerate" choice
                     ///< `numeric_kind` already makes for arithmetic.

  // --- Memory -------------------------------------------------------------
  //
  //  An aggregate or descriptor lives as inline bytes in its owner: the
  //  frame's byte storage, the caller's result storage, a static, or a heap
  //  block a container owns (`src/runtime/value_layout.h`). A register that
  //  refers to one holds its address in `slot_value::u`. Rather than one
  //  opcode per aggregate kind, loads and stores take a byte offset and a
  //  width, the same "parameterize, don't combinatorially enumerate" choice
  //  `numeric_kind` makes for arithmetic.
  op_stack_alloc,    ///< u16 dst, u32 byte_offset — reg[dst] = the address
                     ///< of `byte_offset` within this frame's own
                     ///< `stack_byte_size` scratch range. The frame-local
                     ///< counterpart of `op_alloc`: storage for a
                     ///< `uninit[T, N]` buffer, which is the one aggregate
                     ///< that must not be heap-allocated. The offset is
                     ///< fixed at compile time and the range is reserved
                     ///< whole on frame entry, so the address is stable for
                     ///< the life of the frame and the opcode itself
                     ///< allocates nothing.
  op_alloc,          ///< u16 dst, u64 byte_size — reg[dst] = a fresh, zeroed
                     ///< `byte_size`-byte heap block (a coroutine object or
                     ///< its state block).
  op_load_slot,      ///< u16 dst, u16 ptr, u16 byte_offset, u8 field_size —
                     ///< reg[dst] = a `field_size`-byte (1/2/4/8), zero-
                     ///< extended-to-64-bit read at
                     ///< *(reg[ptr] as uint8_t* + byte_offset). The offset
                     ///< and width come from the value's layout.
  op_store_slot,     ///< u16 ptr, u16 byte_offset, u16 src, u8 field_size —
                     ///< *(reg[ptr] as uint8_t* + byte_offset) = the low
                     ///< `field_size` bytes (1/2/4/8) of reg[src]. Write-side
                     ///< counterpart to `op_load_slot`.
  op_str_const_data, ///< u16 dst, u32 string_const_index — reg[dst].u = the
                     ///< address of `string_constants[idx]`'s bytes, which
                     ///< outlive the run. The storage form builds a `str`
                     ///< descriptor from it in the frame, allocating nothing.
  op_global_addr,    ///< u16 dst, u32 offset — reg[dst].u = the address of
                     ///< byte `offset` of the program's static storage
                     ///< (`bytecode_module::global_byte_size`), which lives
                     ///< as long as the `vm` and never moves.

  // ------------------------------------------------------------------
  //  Address arithmetic: the address of a field or element, from the
  //  address of the value that holds it. A load or store through the
  //  result is an `op_load_slot`/`op_store_slot` with `byte_offset = 0`.
  op_addr_slot,    ///< u16 dst, u16 ptr, u16 byte_offset — reg[dst].u =
                   ///< reg[ptr].u + byte_offset. The address counterpart of
                   ///< `op_load_slot`: `&mut s.field` on a scalar field.
  op_addr_indexed, ///< u16 dst, u16 ptr, u16 index, u8 elem_size — reg[dst].u
                   ///< = reg[ptr].u + reg[index].u * elem_size: the address
                   ///< of an element (`xs[i]`, `&mut xs[i]`, and `iter_mut`
                   ///< over a container).
  op_panic_if,     ///< u16 cond, u8 panic_reason — panics with
                   ///< `static_cast<panic_reason>(panic_reason)` if
                   ///< reg[cond] (a `boolean` register) is true; otherwise
                   ///< falls through. The bytecode-level building block a
                   ///< compiler-emitted bounds check (or any other
                   ///< source-triggered panic condition that isn't one of
                   ///< the checked-arithmetic opcodes' own built-in panics)
                   ///< composes from — mirrors `llvm_codegen`'s
                   ///< `guard_panic` helper, just reified as one opcode
                   ///< instead of a conditional branch to a call.

  // --- Diagnostics ---------------------------------------------------------
  op_panic, ///< (no operands) — panics with `panic_reason::explicit_panic`
            ///< (see `panic.h`). Every panic, from this opcode or from the
            ///< checked-arithmetic opcodes above, reports one of a small
            ///< fixed `panic_reason` set rather than an arbitrary message.
            ///< `op_panic` is for panics with a source-level trigger, such
            ///< as an unreachable match arm.

  // --- Coroutines -----------------------------------------------------
  //
  //  A generator or task is a coroutine object (`src/runtime/coroutine.h`)
  //  whose step function the storage compiler builds; its state block holds
  //  what lives across a suspension. These run, cancel and finish one.
  op_coroutine_resume, ///< u16 dst, u16 coroutine_reg — runs the coroutine
                       ///< one step. If reg[coroutine_reg]'s `status` slot is
                       ///< not `suspended`, writes that status into dst
                       ///< directly (no frame push). Otherwise pushes a frame
                       ///< for the coroutine's `step_function_index`, with
                       ///< register 0 = `state_ptr`, register 1 =
                       ///< `resume_index`, register 2 = the coroutine itself;
                       ///< the step function's return value, the new status,
                       ///< lands in dst. A suspension leaves the yielded item
                       ///< in the coroutine's `value` slot. A generator's
                       ///< `.next()` compiles to this plus code that reads
                       ///< `value` into an `option`.
  op_coroutine_cancel, ///< u16 dst, u16 generator_reg — the compiled form
                       ///< of `hir_coroutine_cancel`. If reg[generator_reg]'s
                       ///< `finished` slot is set, does nothing. Otherwise
                       ///< sets it to `2` (cancelled) and resumes the
                       ///< generator exactly as `op_coroutine_resume` does; its
                       ///< body sees the cancellation and returns, dropping
                       ///< what it owns, which sets `finished` to `1`. The
                       ///< status it returns lands in dst, unread.
  op_coroutine_cancelled, ///< u16 dst, u16 generator_reg — reg[dst] = whether
                          ///< reg[generator_reg]'s `finished` slot is `2`:
                          ///< the generator was resumed only to be cancelled.
                          ///< An acquire load: the scheduler may write the
                          ///< slot from another thread.
  op_coroutine_finish,    ///< u16 generator_reg — sets reg[generator_reg]'s
                          ///< `finished` slot to `1` (done) with a release
                          ///< store, as a body that has returned does. The
                          ///< storage form's `coroutine_finish`.
  op_atomic_fetch_add,    ///< u16 dst, u16 ptr, u16 n — atomically adds
                          ///< reg[n] to the 8-byte word at reg[ptr], with
                          ///< acquire-release ordering; dst = the word's
                          ///< previous value. Compiled form of
                          ///< `rt_atomic_fetch_add` (`src/intrinsics.h`).
  op_atomic_fetch_sub,    ///< u16 dst, u16 ptr, u16 n — the subtracting
                          ///< counterpart of `op_atomic_fetch_add`.
  op_host_call,           ///< u16 dst, u16 site, u16 first_arg_reg, u8 argc —
                          ///< asks the host to perform the operation
                          ///< `bytecode_function::host_sites[site]` describes,
                          ///< on reg[first_arg_reg .. +argc]; dst = its
                          ///< result. Compiled form of `hir_host_call`; only
                          ///< a hosted run (`run_limits::host`) can do one.
  op_copy_bytes,          ///< u16 dst_ptr, u16 src_ptr, u32 byte_count — copies
  ///< `byte_count` bytes from the address in reg[src_ptr]
  ///< to the address in reg[dst_ptr]. The transfer of an
  ///< inline aggregate or descriptor (representation
  ///< v2); both ranges are owned storage that the
  ///< compiler has shown do not overlap, but the copy is
  ///< a `memmove` so a mistake there cannot corrupt
  ///< bytes silently. Does nothing for a zero count.
  op_zero_bytes,          ///< u16 dst_ptr, u32 byte_count — writes `byte_count`
                          ///< zero bytes at the address in reg[dst_ptr]: an
                          ///< `uninit[T, N]` buffer each time it is made.
  op_call_intrinsic_into, ///< u8 intrinsic_id, u16 first_arg_reg, u8 argc —
                          ///< calls an intrinsic marked `into` in
                          ///< `src/intrinsics.h`, whose result is built in
                          ///< caller storage. reg[first_arg_reg] holds the
                          ///< destination address and the following registers
                          ///< the parameters: a scalar in its register, a
                          ///< descriptor or aggregate as the address of the
                          ///< caller's bytes. Nothing comes back. The
                          ///< counterpart of `op_call_intrinsic`, whose result
                          ///< comes back in a register.
  op_call_function,       ///< u16 dst, u16 entry_reg, u16 first_arg, u8 argc —
                          ///< calls function `reg[entry_reg]` with the `argc`
                          ///< registers from `first_arg` as its arguments. The
                          ///< storage compiler's call through a `fn` value
                          ///< (`{entry, env, glue}`, 24 bytes of the caller's
                          ///< storage): the caller loads `entry`, and the
                          ///< environment address is the first argument.
  op_drop_function_value, ///< u16 dst, u16 value_reg — runs the drop glue of
                          ///< the `fn` value whose bytes are at the address in
                          ///< reg[value_reg], when it has any: calls function
  ///< `glue - 1` with the environment address as its only
  ///< argument, its `unit` landing in dst. Does nothing
  ///< for a value with no glue (`glue == 0`).
};

// ==========================================================================
//  Operand layout
//
//  The width of a register operand is stated once, here, rather than being
//  implied by ~130 hand-written emit sites in the bytecode compiler and ~52
//  hand-written decode sites in the VM. It is `2` (not `1`) because a `u8`
//  register operand capped a function at 256 registers, and the frame
//  itself never had that limit: `bytecode_function::register_count` is a
//  `uint16_t` and the VM sizes frames straight from it, so the ceiling was
//  purely an encoding artifact.
//
//  Widening was chosen over spilling deliberately. Spilling is the smaller
//  change but has the worse failure mode: a spill bug only manifests above
//  256 simultaneously-live values, which no ordinary program reaches and
//  which both backends agreeing cannot detect (only this tier would spill).
//  A widening mistake, by contrast, desynchronizes the instruction stream
//  and breaks essentially every program on the first test run. Given a
//  choice between a narrow quiet bug and a wide loud one, this ISA takes
//  the loud one.
//
//  Note that only *register* operands are 2 bytes. The other `u8` operands
//  — `numeric_kind`, `field_size`, `elem_size`, `argc`, `intrinsic_id`,
//  `panic_reason`, `next_resume_index` — index small fixed sets and stay
//  1 byte, so an instruction grows by exactly one byte per register it
//  names rather than doubling.
// ==========================================================================

/// What one operand of an instruction encodes. `reg` is called out
/// separately from `imm8`/`imm16` precisely because it is the one whose
/// width is a property of the ISA rather than of the individual opcode —
/// telling the two apart is what makes the width changeable in one place.
enum class operand_kind : uint8_t {
  reg,   ///< A register index, `k_register_operand_bytes` wide.
  imm8,  ///< A 1-byte immediate (kind tag, size, count, id).
  imm16, ///< A 2-byte immediate (constant index, byte offset, function index).
  imm32, ///< A 4-byte unsigned immediate (a frame's `uninit` byte offset,
         ///< a constant-pool index).
  imm64, ///< An 8-byte unsigned immediate (a heap allocation's byte size).
  rel32, ///< A 4-byte signed relative jump offset.
};

/// The number of bytes a register operand occupies in the instruction
/// stream. Changing this and `function_compiler::emit_register` is, by
/// construction, the whole of a width change: every other size and offset
/// in the encoding is derived from `operands_of` below.
inline constexpr size_t k_register_operand_bytes = 2;

/// The ordered operand sequence of one opcode. No instruction in this ISA
/// takes more than four operands, so this is a fixed array rather than a
/// heap sequence — it keeps `operands_of` usable in a constant expression.
struct operand_signature {
  std::array<operand_kind, 4> kinds{};
  uint8_t count = 0;
};

/// The operand sequence of `op`.
///
/// Deliberately an exhaustive `switch` rather than a table indexed by
/// opcode: under `-Wswitch -Werror`, adding an opcode without describing
/// its operands here fails the build. A table would have silently returned
/// a zero-length signature for the new opcode instead, and a wrong
/// instruction length is exactly the kind of mistake that is invisible
/// until the instruction stream desynchronizes somewhere unrelated.
[[nodiscard]] constexpr auto operands_of(opcode op) -> operand_signature {
  using enum operand_kind;
  constexpr auto sig =
      [](std::initializer_list<operand_kind> kinds) -> operand_signature {
    auto result = operand_signature{};
    for (const auto kind : kinds) {
      result.kinds.at(result.count++) = kind;
    }
    return result;
  };

  switch (op) {
  case opcode::op_load_const:
    return sig({reg, imm32});
  case opcode::op_move:
    return sig({reg, reg});

  // Arithmetic, bitwise, and comparison all share one shape: a destination,
  // one or two sources, and a trailing `numeric_kind` tag.
  case opcode::op_add:
  case opcode::op_sub:
  case opcode::op_mul:
  case opcode::op_div:
  case opcode::op_mod:
  case opcode::op_add_wrap:
  case opcode::op_sub_wrap:
  case opcode::op_mul_wrap:
  case opcode::op_add_sat:
  case opcode::op_sub_sat:
  case opcode::op_mul_sat:
  case opcode::op_bitand:
  case opcode::op_bitor:
  case opcode::op_bitxor:
  case opcode::op_shl:
  case opcode::op_shr:
  case opcode::op_eq:
  case opcode::op_ne:
  case opcode::op_lt:
  case opcode::op_le:
  case opcode::op_gt:
  case opcode::op_ge:
    return sig({reg, reg, reg, imm8});
  case opcode::op_neg:
  case opcode::op_bitnot:
    return sig({reg, reg, imm8});
  case opcode::op_not_bool:
    return sig({reg, reg});
  case opcode::op_cast:
    return sig({reg, reg, imm8, imm8});

  case opcode::op_jump:
    return sig({rel32});
  case opcode::op_jump_if_false:
  case opcode::op_jump_if_true:
    return sig({reg, rel32});

  case opcode::op_call:
    return sig({reg, imm16, reg, imm8});
  case opcode::op_return_value:
    return sig({reg});
  case opcode::op_return_unit:
    return sig({});
  case opcode::op_tail_call:
    return sig({imm16, reg, imm8});
  case opcode::op_call_intrinsic:
    return sig({reg, imm8, reg, imm8});
  case opcode::op_host_call:
    return sig({reg, imm16, reg, imm8});

  case opcode::op_stack_alloc:
    return sig({reg, imm32});
  case opcode::op_alloc:
    return sig({reg, imm64});
  case opcode::op_load_slot:
    return sig({reg, reg, imm16, imm8});
  case opcode::op_store_slot:
    return sig({reg, imm16, reg, imm8});
  case opcode::op_str_const_data:
  case opcode::op_global_addr:
    return sig({reg, imm32});

  case opcode::op_addr_slot:
    return sig({reg, reg, imm16});
  case opcode::op_addr_indexed:
    return sig({reg, reg, reg, imm8});
  case opcode::op_panic_if:
    return sig({reg, imm8});
  case opcode::op_panic:
    return sig({});

  case opcode::op_coroutine_resume:
  case opcode::op_coroutine_cancel:
  case opcode::op_coroutine_cancelled:
    return sig({reg, reg});
  case opcode::op_atomic_fetch_add:
  case opcode::op_atomic_fetch_sub:
    return sig({reg, reg, reg});
  case opcode::op_copy_bytes:
    return sig({reg, reg, imm32});
  case opcode::op_zero_bytes:
    return sig({reg, imm32});
  case opcode::op_call_intrinsic_into:
    return sig({imm8, reg, imm8});
  case opcode::op_call_function:
    return sig({reg, reg, reg, imm8});
  case opcode::op_drop_function_value:
    return sig({reg, reg});
  case opcode::op_coroutine_finish:
    return sig({reg});
  }
  return operand_signature{};
}

/// How many bytes one `op` instruction occupies, opcode byte included.
[[nodiscard]] constexpr auto instruction_size(opcode op) -> size_t {
  auto size = size_t{1};
  const auto signature = operands_of(op);
  for (auto index = uint8_t{0}; index < signature.count; ++index) {
    switch (signature.kinds.at(index)) {
    case operand_kind::reg:
      size += k_register_operand_bytes;
      break;
    case operand_kind::imm8:
      size += 1;
      break;
    case operand_kind::imm16:
      size += 2;
      break;
    case operand_kind::imm32:
    case operand_kind::rel32:
      size += 4;
      break;
    case operand_kind::imm64:
      size += 8;
      break;
    }
  }
  return size;
}

} // namespace cinder::bytecode
