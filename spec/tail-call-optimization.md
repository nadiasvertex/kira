# Tail-Call Optimization Implementation Plan

> **Status: proposed (not started, 2026-07-17).** No tail-call machinery
> exists yet: the bytecode VM's `op_call` unconditionally pushes a frame
> (`src/bytecode/vm.cpp:1361`, `push_frame` at `:779`) and the LLVM backend
> emits a plain `CreateCall` with no tail marking
> (`src/llvm_codegen/codegen.cpp:1625`). This document plans guaranteed TCO
> for the bytecode VM and best-effort-but-guaranteed-where-provable TCO for
> the AOT/JIT backend.
>
> Should be implemented by Opus.

Kira has no tail-call optimization today. Deep or mutual recursion grows the
VM's `frames` vector (capped at `k_max_call_depth = 4096`, `vm.cpp:33`) or the
native stack until it overflows — a self-recursive `count_down` blows up well
before a `while` loop over the same range would. Because the language leans on
recursion as an expressive looping idiom and the compiler's stated job is to
*teach* rather than surprise, a call in tail position should reuse its frame so
recursion is a safe, constant-space control-flow construct — not a latent
stack-overflow.

This plan reuses the tail-position analysis both backends *already* perform
(the implicit-tail-expression handling in `compile_body_and_finish`,
`llvm_codegen/codegen.cpp:763`, and its bytecode twin), but lifts the notion of
"this call is in tail position" out of the two backends into a single
backend-agnostic HIR pass so the two tiers can never disagree about which calls
are eligible.

## Where the compiler stands today

Verified against the tree at 2026-07-17:

- **A call is always a frame push.** `op_call` copies args into a fresh
  `frame`, appends it to `frames` (`vm.cpp:1370`), and `op_return_value` /
  `op_return_unit` pop it (`vm.cpp:1373`, `:1385`). The frame carries
  `has_caller` and `result_reg` (`vm.cpp:771`–`776`) — the two fields a tail
  call must *preserve* so the tail-callee ultimately returns to the original
  caller.
- **The LLVM backend never marks tail calls.** No `setTailCallKind` /
  `MustTail` anywhere; `compile_call` returns a bare `CreateCall`
  (`codegen.cpp:1625` for the direct case, `:1678` for the indirect/closure
  case). All Kira return values lower to a scalar, a pointer (heap types are
  `ptr`, `codegen.cpp:296`), `i64` (the `unit` placeholder), or `void` — there
  is **no `sret` by-value struct return**, which materially simplifies
  `musttail`'s ABI-match preconditions.
- **Tail position is already known at codegen, just not reified.**
  `compile_body_and_finish` special-cases a trailing `hir_expr_stmt` and emits
  `CreateRet(*value)` (`codegen.cpp:780`–`799`); explicit `hir_return` lowers
  at `codegen.cpp:2894`+. The bytecode compiler mirrors both
  (`compile.cpp:2571`+ for return, `compile_block_as_value` at `:2530`).
- **Postconditions already disqualify wrapped returns, for free.**
  `lower_return_value` (`lower.cpp:570`) rewrites `return f(x)` into
  `let return = f(x); <postcondition checks>; return return` whenever
  `post_contracts_` is non-empty. After that rewrite the call sits in a `let`
  initializer, **not** in tail position — so any tail-position analysis that
  runs on lowered HIR will *correctly refuse* to treat a
  postcondition-guarded call as a tail call, with no special case needed.
- **Generators have their own return protocol.** A generator step function
  returns `option`-wrapped resume values (`build_option_some/none`,
  `codegen.cpp:2351`/`:2971`) under `is_generator_step_` (`codegen.cpp:3388`);
  a bare `yield`/`return` there is suspension bookkeeping, not an ordinary
  tail. Generator steps are excluded from TCO in v1.

Not done — the actual work: there is no representation of "this call is a tail
call," no VM opcode for frame reuse, and no LLVM tail marking.

## Strategy — four decisions that bound v1

State these in the code and in any diagnostic, the way the HKT plan states
its bounding decisions — they are what keep this a tractable feature.

1. **TCO is implicit and detected, not annotated.** A call is optimized iff a
   HIR analysis proves it is in tail position and eligible. There is no
   `become` keyword in v1 (see Future extensions). Implicit means silent: a
   tail call that *is* optimized produces no diagnostic; a syntactically
   tail-looking call that *cannot* be optimized (e.g. postcondition-guarded)
   simply runs as an ordinary call. We do **not** promise "every call that
   looks like a tail call is O(1) stack" — we promise "a call the analysis
   marks is O(1) stack on the VM, and on AOT where the ABI permits."

2. **Direct, statically-resolved calls only in v1.** Only the `op_call` /
   `CreateCall(found->second)` shape — a `hir_call` whose callee is a
   `hir_local_ref` resolving to a known `bytecode_function` / `llvm::Function`.
   Self-recursion and mutual recursion across named functions both qualify.
   **Excluded in v1:** indirect/closure calls (`op_call_indirect`,
   `codegen.cpp:1678`), intrinsic calls (`op_call_intrinsic` — a C++ call, no
   frame to reuse), and generator step functions. Each exclusion is a natural
   future extension, not a design wall.

3. **The VM guarantees TCO; AOT guarantees it where the ABI provably permits.**
   Frame reuse in the VM is unconditional and always correct, so the bytecode
   tier gives an *absolute* constant-space guarantee. On LLVM, `musttail` is a
   verify-or-die contract, so we emit it **only** when its preconditions
   provably hold (matching return storage type, same calling convention, no
   `sret` — which Kira never has); otherwise we fall back to a normal call
   carrying the non-binding `Tail` hint. The parity tests assert VM constant
   depth; the AOT tests assert *correctness*, and assert constant stack only
   for the `musttail`-eligible shapes.

4. **One analysis, both backends.** Tail-position eligibility is computed once,
   in `src/hir/tail_calls.{h,cpp}` (mirroring the existing `captures` /
   `live_across_yield` HIR passes), and recorded on the node. The backends
   *consume* the flag; they never re-derive it. This is the invariant that
   keeps the two tiers from disagreeing.

## Phase 1 — Tail-position detection (HIR, backend-agnostic)

Add `bool is_tail_call = false;` to `hir_call` (`nodes.h:278`), defaulting
false so nothing that doesn't run the pass changes behavior.

Add `src/hir/tail_calls.{h,cpp}` exposing:

```cpp
namespace kira::hir {
// Marks every eligible tail call in `fn`'s body (`hir_call::is_tail_call`).
// No-op for generator step functions. Idempotent.
auto mark_tail_calls(hir_function &fn) -> void;
}
```

The pass walks the body carrying a single boolean *tail-position* context and
sets `is_tail_call` on a `hir_call` that is (a) in tail position, (b) a direct
call to a statically-known function, and (c) not inside a generator step. Tail
position propagates as:

- **Function body block.** The body is in tail position. In a block, only the
  **last** statement inherits the block's tail position; every earlier
  statement is non-tail. The last statement is in tail position if it is a
  trailing `hir_expr_stmt` (the implicit tail — the same shape
  `compile_body_and_finish` returns) or an explicit `hir_return` whose `value`
  is the expression to inspect.
- **`hir_return value`.** `value` inherits tail position. (After
  `lower_return_value`'s postcondition rewrite, a guarded return's call is a
  `let` initializer, which is never in tail position — so guarded calls are
  excluded automatically, per the standing decision.)
- **`hir_if` / `hir_match` / nested `hir_block` in tail position.** Each
  branch body / arm body / the block's own tail inherits tail position. This
  is exactly the recursion `compile_block_as_value` already performs, now
  computed once here.
- **Everything else is non-tail.** A call under a `let`, inside a larger
  expression (`f(g(x))` — only `f` could be tail, and it isn't here since its
  result feeds `+`/a field/etc.), a binary operand, an argument, a loop body
  (the loop continues after it), etc.

Eligibility (c-and-b) additionally requires the callee resolve the same way
the backends resolve it: `call.callee` is a `hir_local_ref` that is **not** a
local binding and **not** an intrinsic name (mirror the `lookup_local` /
`intrinsic_index_of` guard in both `compile_call`s). Indirect and intrinsic
calls stay `is_tail_call = false`.

Wire `mark_tail_calls` into the lowering driver right after a function body is
lowered (alongside where `captures` / `live_across_yield` run), guarded so
generator steps are skipped.

**Test:** `src/hir/tail_calls_test.cpp` — assert the flag is set on a
self-recursive tail call, on both arms of a tail `match`, on a tail call in an
`if` branch; and is *clear* on a non-tail call (`1 + f(x)`), a
`let`-bound call, an argument-position call, and a call whose `return` is
postcondition-wrapped.

## Phase 2 — VM frame reuse (guaranteed TCO)

Add `op_tail_call` to `src/bytecode/opcodes.h` with the **same operand layout
as `op_call`** (`u16 function_index, u8 first_arg_reg, u8 argc`) minus the
`dst` byte — a tail call has no destination register in the current frame; its
result flows to the current frame's *caller* via the preserved `result_reg`.

In `vm.cpp`, add a case that reuses the top frame instead of pushing:

```cpp
case opcode::op_tail_call: {
  const uint16_t fn_idx    = read_u16(code, ip);
  const uint8_t  first_arg = code[ip + 2];
  const uint8_t  argc      = code[ip + 3];
  // Snapshot args before we overwrite the register file.
  std::vector<slot_value> call_args(f.registers.begin() + first_arg,
                                    f.registers.begin() + first_arg + argc);
  const auto &callee = module_.functions.at(fn_idx);
  // Reuse THIS frame: keep has_caller / result_reg (so the tail-callee
  // returns to *our* caller), swap in the callee's body + register file.
  f.function = &callee;
  f.registers.assign(callee.register_count, slot_value{});
  for (size_t i = 0; i < argc; ++i) f.registers[i] = call_args[i];
  f.pc = 0;
  continue; // depth unchanged — this is the whole point.
}
```

`frames.size()` never grows, so `k_max_call_depth` is never approached by a
tail-recursive loop. `has_caller` / `result_reg` are untouched, so an eventual
`op_return_value` hands the result to the original caller exactly as if the
whole tail chain had been one call.

**Compiler side (`bytecode_compiler/compile.cpp`).** Where `compile_call`
currently emits `op_call` (`:1271`), branch on `call.is_tail_call`: emit
`op_tail_call` (no `dst`) instead of `op_call` + a following `op_return_*`.
This requires the return to be elided — the tail call *is* the return. The
cleanest seam is the return/tail sites (`compile.cpp:2571`+ and
`compile_body_and_finish`'s twin): when the value being returned is a
`hir_call` with `is_tail_call`, emit `op_tail_call` and stop (do not emit the
trailing `op_return_value`). Keep the ordinary `op_call` path for
`is_tail_call == false`.

**Test:** a `codegen_stress`/VM fixture doing deep self-recursion
(e.g. `count_down(1_000_000)`) that would exceed `k_max_call_depth` under
`op_call` but completes under `op_tail_call`; plus a mutual-recursion pair
(`is_even`/`is_odd`) proving cross-function tail chains stay flat. Assert the
result *and* (via a VM hook or a depth-instrumented test build) that peak
`frames.size()` stays bounded.

## Phase 3 — LLVM `musttail` (guaranteed where the ABI permits)

At the return sites where a `hir_return`/implicit-tail value is a
`is_tail_call` `hir_call`, thread that knowledge into `compile_call` so it can
mark the `llvm::CallInst`:

1. Compute the callee `llvm::Function*` as today (`codegen.cpp:1607`–`1625`).
2. **Precondition check** (all must hold, else fall back):
   - callee return type == current function return type (both the same scalar,
     the same pointer type, or both `void`) — Kira's lack of `sret` means this
     is a plain `llvm::Type*` equality;
   - same calling convention (all Kira functions share the default CC today);
   - the call is immediately followed by the matching `ret` (guaranteed by the
     tail-site seam).
3. If the preconditions hold: `call->setTailCallKind(llvm::CallInst::TCK_MustTail)`,
   then emit `CreateRet(call)` (or `CreateRetVoid()` for a unit tail) **directly
   after**, with nothing between — `musttail` requires the `ret` to immediately
   consume the call.
4. If they do not: emit an ordinary `CreateCall` with
   `TCK_Tail` (a non-binding hint LLVM's backend *may* honor at `-O2`+) and the
   normal return. Correctness is unaffected; only the guarantee weakens, which
   the plan explicitly permits for these shapes.

Because `musttail` is verify-or-die, keep the precondition check conservative:
when in doubt, fall back to `TCK_Tail`. Never emit `musttail` for a generator
step, an indirect/closure call, or an intrinsic.

**Interaction with the void-store fix.** The recent
`compile_block_as_value` fix (a `void` unit-call tail stores an `i64` zero
placeholder) is orthogonal: a *unit-returning tail call in a match arm* whose
result feeds the match value is **not** in function-tail position (its value is
stored into the match slot and the function returns *after* the match), so the
tail pass leaves it unmarked and it keeps the placeholder-store behavior. Only
a unit tail call that is itself the function's tail (`return noop(x)` /
trailing `noop(x)` as the whole body tail) becomes a `musttail ... ret void`.

**Test:** extend `codegen_test` / `aot_test` — a self-recursive tail function
and an `is_even`/`is_odd` pair, asserting the correct result through both JIT
and a real `--compile` binary; and an IR-inspection check that the eligible
call carries `musttail`. Add a deep-recursion AOT fixture (large N) that
overflows the native stack without TCO and completes with it, as the
end-to-end guarantee test.

## Phase 4 — Diagnostics, docs, and the teacher angle

- **Reference doc.** Add a short "Tail calls" note to `kira-reference.md`
  (Layer 2 or 3): recursion in tail position runs in constant stack space on
  the VM and, where the ABI permits, in AOT; state the v1 limits (direct calls,
  no postcondition-guarded returns, no closures/generators) plainly so users
  can predict when they get the guarantee.
- **No new error diagnostics in v1** — TCO is a silent optimization. (If a
  future `become` keyword lands, *that* gets teacher-quality "this is not a
  tail call because …" diagnostics; see below.)
- **`spec/todo.md`.** Add the feature and link this plan.

## Risks and open questions

- **`musttail` strictness.** The single real AOT risk is emitting `musttail`
  where LLVM's verifier rejects it, which fails compilation outright. Mitigated
  by the conservative precondition gate + `TCK_Tail` fallback; the IR-inspection
  test guards against silently *never* emitting it.
- **Argument aliasing during VM frame reuse.** Args are snapshotted into a
  local `call_args` vector *before* the register file is reassigned, so a tail
  call passing its own registers as arguments is safe. (Same pattern `op_call`
  already uses at `vm.cpp:1367`.) A future zero-copy optimization must preserve
  this.
- **Debuggability.** Collapsed frames mean a tail-recursive stack trace won't
  show every logical call. Acceptable and expected for TCO; worth a one-line
  note in the reference so it doesn't surprise users.
- **Register-count growth.** Frame reuse reallocates `f.registers` to the
  callee's `register_count`; for mutual recursion between very different-sized
  functions this reallocates each hop. Correct, and cheaper than a push; a
  capacity-reuse tweak is a possible later optimization.

## Future extensions (explicitly out of v1 scope)

1. **Explicit `become` (guaranteed-or-error).** A `become f(x)` keyword that
   *requires* a tail call and emits a teacher-quality diagnostic when the call
   is not eligible ("`become` requires a tail call, but a postcondition runs
   after `f(x)` returns; …"). This is the natural home for the diagnostics v1
   deliberately omits.
2. **Tail calls through closures** (`op_call_indirect` frame reuse; LLVM
   indirect `musttail`).
3. **Self-recursion → loop lowering** as an alternative to `musttail` on AOT,
   sidestepping the verifier entirely for the common self-recursive case.
4. **Tail calls in generator step functions**, once the resume protocol's
   interaction with frame reuse is worked out.
