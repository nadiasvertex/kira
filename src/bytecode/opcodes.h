#pragma once

#include <cstdint>

namespace kira::bytecode {

// ==========================================================================
//  Instruction set — see `spec/codegen-design.md` Decision 1/increment 1.
//
//  A register machine, not a stack machine: every operand is an explicit
//  register index into the current call frame's register file, not an
//  implicit stack top. This reverses the stack-machine choice this same
//  increment started with — caught and corrected before any compiler code
//  was written against it, specifically because register machines
//  measurably win on instruction count (one `add dst, lhs, rhs` where a
//  stack machine needs push-push-add-implicit-pop-push) and avoid the
//  stack-shuffling (redundant push/pop pairs for values already sitting
//  where they're needed) that a stack machine's compiler has to work
//  around after the fact — Lua's VM (register-based since 5.0, replacing an
//  earlier stack design) and Dalvik/ART both made the same call for the
//  same reason. Tearing this out once the HIR-to-bytecode compiler existed
//  would have been far more expensive than fixing it now.
//
//  A function's registers are one flat array per call frame — there is no
//  separate "locals" storage distinct from "temporaries": parameters
//  occupy registers `[0, param_count)` at call entry, and every `let`/
//  `var` binding *and* every intermediate expression result gets the next
//  unused register, monotonically, for the same "simplest thing that
//  works, revisit if it matters" reason `spec/codegen-design.md` already
//  accepts for locals (no register reuse across sibling scopes yet).
//  `bytecode_function::register_count` (chunk.h) is this high-water mark.
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
//  the one-byte opcode: `u8` for a register index (up to 256 per frame —
//  ample for the scalar/control-flow subset this increment covers; revisit
//  the width only if a real program's temporary count ever gets close),
//  `u16` for a constant-pool or function-table index, `i32` for a relative
//  jump offset. `chunk_writer` (chunk.h) is the only thing that needs to
//  reason about instruction boundaries.
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
//    semantics (`spec/kira-reference.md`'s "Integer Overflow" — plain
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
  op_load_const, ///< u8 dst, u16 const_index — reg[dst] = constants[idx].
  op_move,       ///< u8 dst, u8 src — reg[dst] = reg[src].

  // --- Checked arithmetic (panics on overflow/div-by-zero for integer
  //     kinds, per spec/kira-reference.md's "Integer Overflow" section;
  //     plain IEEE semantics — no panic, may produce inf/nan — for float
  //     kinds) -----------------------------------------------------------
  op_add, ///< u8 dst, u8 lhs, u8 rhs, u8 numeric_kind — reg[dst] = lhs+rhs.
  op_sub, ///< u8 dst, u8 lhs, u8 rhs, u8 numeric_kind.
  op_mul, ///< u8 dst, u8 lhs, u8 rhs, u8 numeric_kind.
  op_div, ///< u8 dst, u8 lhs, u8 rhs, u8 numeric_kind.
  op_mod, ///< u8 dst, u8 lhs, u8 rhs, u8 numeric_kind.
  op_neg, ///< u8 dst, u8 src, u8 numeric_kind — checked for integers.

  // --- Wrapping arithmetic (`+%`/`-%`/`*%` — integer kinds only; never
  //     called with a float numeric_kind) --------------------------------
  op_add_wrap, ///< u8 dst, u8 lhs, u8 rhs, u8 numeric_kind.
  op_sub_wrap, ///< u8 dst, u8 lhs, u8 rhs, u8 numeric_kind.
  op_mul_wrap, ///< u8 dst, u8 lhs, u8 rhs, u8 numeric_kind.

  // --- Saturating arithmetic (`+|`/`-|`/`*|` — integer kinds only) ------
  op_add_sat, ///< u8 dst, u8 lhs, u8 rhs, u8 numeric_kind.
  op_sub_sat, ///< u8 dst, u8 lhs, u8 rhs, u8 numeric_kind.
  op_mul_sat, ///< u8 dst, u8 lhs, u8 rhs, u8 numeric_kind.

  // --- Bitwise / shift (integer kinds only) ------------------------------
  op_bitand, ///< u8 dst, u8 lhs, u8 rhs, u8 numeric_kind.
  op_bitor,  ///< u8 dst, u8 lhs, u8 rhs, u8 numeric_kind.
  op_bitxor, ///< u8 dst, u8 lhs, u8 rhs, u8 numeric_kind.
  op_bitnot, ///< u8 dst, u8 src, u8 numeric_kind.
  op_shl,    ///< u8 dst, u8 lhs, u8 rhs, u8 numeric_kind.
  op_shr,    ///< u8 dst, u8 lhs, u8 rhs, u8 numeric_kind — arithmetic for
             ///< signed kinds, logical for unsigned.

  // --- Logical (operates on `numeric_kind::boolean` registers only;
  //     `and`/`or` themselves never reach here — see below, they compile
  //     to jumps) ----------------------------------------------------------
  op_not_bool, ///< u8 dst, u8 src.

  // --- Comparison (result is always a `boolean` register) ----------------
  op_eq, ///< u8 dst, u8 lhs, u8 rhs, u8 numeric_kind.
  op_ne, ///< u8 dst, u8 lhs, u8 rhs, u8 numeric_kind.
  op_lt, ///< u8 dst, u8 lhs, u8 rhs, u8 numeric_kind.
  op_le, ///< u8 dst, u8 lhs, u8 rhs, u8 numeric_kind.
  op_gt, ///< u8 dst, u8 lhs, u8 rhs, u8 numeric_kind.
  op_ge, ///< u8 dst, u8 lhs, u8 rhs, u8 numeric_kind.

  // --- Cast ---------------------------------------------------------------
  op_cast, ///< u8 dst, u8 src, u8 from_kind, u8 to_kind — converts reg[src]
           ///< (as from_kind) into reg[dst] (as to_kind). Integer-to-
           ///< integer is truncate-or-extend; integer-to-float and
           ///< float-to-integer follow ordinary C++ conversion semantics;
           ///< float-to-float is widen/narrow.

  // --- Control flow --------------------------------------------------------
  //
  //  `if`/`while`/`while let` compile to these directly (see
  //  spec/codegen-design.md's `hir_if`/`hir_while` shapes). `and`/`or`
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
  op_jump_if_false, ///< u8 cond, i32 relative offset — jump if reg[cond]
                    ///< (a `boolean` register) is false.
  op_jump_if_true,  ///< u8 cond, i32 relative offset — jump if reg[cond]
                    ///< is true.

  // --- Calls and returns ---------------------------------------------------
  op_call, ///< u8 dst, u16 function_index, u8 first_arg_reg, u8 argc —
           ///< copies `argc` consecutive registers starting at
           ///< `first_arg_reg` (in the *caller's* frame) into the new
           ///< callee frame's registers `[0, argc)`, transfers control to
           ///< `function_index`'s entry, and on return copies the callee's
           ///< result into the caller's `dst` register (unused/untouched
           ///< for a callee compiled with `op_return_unit`).
  op_return_value, ///< u8 src — pop the current frame, hand reg[src] back
                   ///< to the caller as the call's result.
  op_return_unit,  ///< (no operands) — pop the current frame with no value
                   ///< to hand back (the callee's return type is `unit`).

  op_call_intrinsic, ///< u8 dst, u8 intrinsic_id, u8 first_arg_reg, u8 argc
                     ///< — calls the native implementation of the intrinsic
                     ///< at index `intrinsic_id` into
                     ///< `kira::known_intrinsic_names` (src/intrinsics.h)
                     ///< with `argc` consecutive registers starting at
                     ///< `first_arg_reg`, writing its result into `dst`. No
                     ///< frame is pushed — an intrinsic is a direct C++
                     ///< call inside the VM's own dispatch loop, not
                     ///< another `bytecode_function`. Parameterized by
                     ///< intrinsic id rather than one opcode per intrinsic,
                     ///< the same "parameterize, don't enumerate" choice
                     ///< `numeric_kind` already makes for arithmetic.

  // --- Heap values (spec/codegen-design.md increment 6) -------------------
  //
  //  Every non-scalar Kira value (`str`, `list[T]`, fixed `array[T, N]`,
  //  tuple, struct, sum-type payload, closure) is a single pointer into
  //  `src/runtime/arena.h`'s bump allocator (Decision 3); `slot_value`'s
  //  existing `u` field holds it, reinterpreted as an address — no new
  //  union member needed, the same way `numeric_kind` already lets one
  //  untagged 8-byte slot mean different things depending on which opcode
  //  (and which static type the compiler already knows) touches it. Rather
  //  than one opcode per aggregate kind (tuple-get, struct-get, variant-
  //  payload-get, closure-env-get, ...), these three are deliberately
  //  generic over "a flat block of 8-byte slots" (`src/runtime/layout.h`),
  //  matching the same "parameterize, don't combinatorially enumerate"
  //  choice `numeric_kind` already made for arithmetic.
  op_alloc,          ///< u8 dst, u16 slot_count — reg[dst] = a fresh, zeroed
                     ///< `slot_count * 8`-byte block from the arena.
  op_load_slot,      ///< u8 dst, u8 ptr, u16 slot_index — reg[dst] =
                     ///< *(reg[ptr] as slot_value* + slot_index).
  op_store_slot,     ///< u8 ptr, u16 slot_index, u8 src —
                     ///< *(reg[ptr] as slot_value* + slot_index) = reg[src].
  op_load_str_const, ///< u8 dst, u16 string_const_index — reg[dst] = a
                     ///< fresh 2-slot `str` heap value `{ len; data_ptr }`
                     ///< whose `data_ptr` points directly at
                     ///< `bytecode_function::string_constants[idx]`'s own
                     ///< bytes (no arena copy — the function outlives the
                     ///< run, so this is safe, and literal string bytes are
                     ///< never mutated through a `str` value).
  op_load_indexed,   ///< u8 dst, u8 ptr, u8 index — reg[dst] =
                     ///< *(reg[ptr] as slot_value* + reg[index].u). The
                     ///< runtime-indexed counterpart to `op_load_slot`'s
                     ///< compile-time-constant slot index — used for fixed
                     ///< `array[T, N]` element access, where the index is an
                     ///< ordinary runtime value the compiler bounds-checks
                     ///< itself (via `op_ge`/`op_panic_if`) before emitting
                     ///< this, not baked into the instruction stream.
  op_store_indexed,  ///< u8 ptr, u8 index, u8 src —
                     ///< *(reg[ptr] as slot_value* + reg[index].u) = reg[src].
  op_store_byte,     ///< u8 ptr, u16 byte_offset, u8 src —
                     ///< *(reg[ptr] as uint8_t* + byte_offset) = low byte of
                     ///< reg[src]. Byte-granular, compile-time-constant-offset
                     ///< counterpart to `op_store_slot` — used for `array
                     ///< [byte, N]`'s tightly-packed (1 byte/element, not one
                     ///< 8-byte slot/element) representation, the one element
                     ///< type that must byte-address so it can alias a
                     ///< `str`/`slice[byte]` view without a copy (see
                     ///< `spec/stdlib.md`'s `std.io` byte-buffer notes).
  op_load_byte_indexed, ///< u8 dst, u8 ptr, u8 index — reg[dst] = zero-
                        ///< extended *(reg[ptr] as uint8_t* + reg[index].u).
                        ///< Byte-granular, runtime-indexed counterpart to
                        ///< `op_load_indexed`, for reading one element out of a
                        ///< byte-packed `array[byte, N]`.
  op_store_byte_indexed, ///< u8 ptr, u8 index, u8 src —
  ///< *(reg[ptr] as uint8_t* + reg[index].u) = low byte of
  ///< reg[src]. Byte-granular, runtime-indexed counterpart
  ///< to `op_store_indexed` — the write-side sibling of
  ///< `op_load_byte_indexed`/`op_store_byte`, unused by
  ///< any construct compiled today (nothing assigns
  ///< through a runtime-indexed `array[byte, N]` element
  ///< yet) but kept alongside its load counterpart so a
  ///< future one doesn't need its own opcode-design pass.
  op_list_push, ///< u8 header_ptr, u8 value — appends reg[value] onto
                ///< the `list[T]` value at reg[header_ptr] (a 3-slot
                ///< `{ len; cap; data }` header, `src/runtime/
                ///< layout.h`), growing/copying to a larger `data`
                ///< block when already at capacity. Unlike
                ///< `op_load_slot`/`op_store_slot`, this is one
                ///< opcode rather than several composed primitives:
                ///< growth needs a real conditional allocate-and-copy
                ///< that doesn't reduce to "one flat block of 8-byte
                ///< slots," so this delegates to
                ///< `kira::runtime::list_push` (the exact same
                ///< function `llvm_codegen`'s generated IR calls via
                ///< `kira_rt_list_push`) rather than reimplementing
                ///< growth as a second copy of that logic here.
  op_panic_if,  ///< u8 cond, u8 panic_reason — panics with
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
            ///< (see `panic.h`). Originally specced with a
            ///< `u16 message_const_index` into the constant pool, but
            ///< `slot_value` (value.h) has no string representation and
            ///< heap types are deferred to increment 6 — rather than add a
            ///< string-table side channel just for this, every VM panic
            ///< (this opcode's, and the checked-arithmetic opcodes' above)
            ///< reports one of a small fixed `panic_reason` set instead of
            ///< an arbitrary message; revisit once increment 6 lands real
            ///< strings. `op_panic` itself is for panics with a
            ///< source-level trigger (e.g. an unreachable-match arm), not
            ///< something increment 1's compiler emits yet, but reserved
            ///< now so the VM (increment 2) has one panic mechanism to
            ///< implement, not two.

  // --- Closures (spec/codegen-design.md increment 6) -----------------------
  op_make_closure,  ///< u8 dst, u16 function_index, u8 env_ptr —
                    ///< reg[dst] = a fresh 2-slot heap block
                    ///< `{ function_index; reg[env_ptr] }` (`src/runtime/
                    ///< layout.h`'s closure layout). `env_ptr` is a plain
                    ///< heap pointer to the capture block (itself a flat
                    ///< `op_alloc`'d slot block, one slot per free variable,
                    ///< populated via `op_store_slot`), or the sentinel `0`
                    ///< when the lambda captures nothing.
  op_call_indirect, ///< u8 dst, u8 closure, u8 first_arg, u8 argc — reads
                    ///< `{ function_index; env_ptr }` out of reg[closure],
                    ///< then calls that function exactly like `op_call`
                    ///< except with `env_ptr` prepended as the callee's
                    ///< hidden register-0 argument ahead of the `argc`
                    ///< declared arguments starting at reg[first_arg] — the
                    ///< indirect counterpart to `op_call`'s compile-time-
                    ///< resolved `function_index` operand, used whenever the
                    ///< callee isn't a direct reference to a named
                    ///< module-level function (a closure value held in a
                    ///< local, an immediately-invoked lambda literal, or any
                    ///< other computed callee expression).
};

} // namespace kira::bytecode
