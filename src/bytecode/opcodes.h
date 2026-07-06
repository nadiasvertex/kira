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

  // --- Diagnostics ---------------------------------------------------------
  op_panic, ///< u16 message_const_index — abort execution; the checked-
            ///< arithmetic opcodes above (`op_add` et al., `op_div`/
            ///< `op_mod`'s divide-by-zero case) trigger the same
            ///< underlying panic path on failure without going through
            ///< this opcode explicitly — `op_panic` itself is for panics
            ///< with a source-level trigger (e.g. an unreachable-match
            ///< arm), not something increment 1's compiler emits yet, but
            ///< reserved now so the VM (increment 2) has one panic
            ///< mechanism to implement, not two.
};

} // namespace kira::bytecode
