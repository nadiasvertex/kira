# 39. Tail-Call Optimization

**Status:** Planned

A call in tail position is planned to reuse its caller's frame — guaranteed constant-space on the bytecode VM, and on LLVM/AOT wherever the ABI provably permits — so recursion is a safe looping idiom rather than a latent stack overflow.

## Design

Today every call pushes a frame: the VM's `frames` vector is capped (`k_max_call_depth`), and deep or mutually recursive calls exhaust it or the native stack well before an equivalent `while` loop would. This chapter describes the target design; none of it is implemented.

### Scope, fixed by four decisions

1. **Implicit and detected, not annotated.** A call is optimized iff a backend-agnostic analysis proves it is in tail position and eligible; there is no `become` keyword (see Future extensions). The optimization is silent: an eligible tail call produces no diagnostic when optimized, and a syntactically tail-looking but ineligible call (e.g. postcondition-guarded) simply runs as an ordinary call. The guarantee is "a call the analysis marks is O(1) stack on the VM, and on AOT where the ABI permits" — not "every call that looks tail-recursive is O(1) stack."
2. **Direct, statically-resolved calls only.** Only a call whose callee resolves to a known function at compile time. Self- and mutual recursion across named functions both qualify. Excluded: indirect/closure calls, intrinsic calls, and calls inside generator step functions (which have their own suspend/resume return protocol) — each a natural future extension, not a design wall.
3. **The VM guarantees TCO; AOT guarantees it where the ABI provably permits.** VM frame reuse is unconditional and always correct — an absolute constant-space guarantee. On LLVM, `musttail` is verify-or-die, so it is emitted only when its preconditions provably hold (matching return type, same calling convention, no `sret` — Kira has no by-value struct return, which simplifies this); otherwise an ordinary call is emitted with the non-binding `Tail` hint. Correctness never depends on whether `musttail` was used, only the stack-space guarantee.
4. **One analysis, both backends.** Tail-position eligibility is computed once as a backend-agnostic HIR pass and recorded on the call node; the backends consume the flag and never re-derive it, so the two tiers cannot disagree about which calls are eligible.

### Tail-position detection

A dedicated HIR pass walks a function body carrying a single tail-position context and marks each `hir_call` that is (a) in tail position, (b) a direct call to a statically-known function, and (c) not inside a generator step:

- The function body block is in tail position. Within a block, only the *last* statement inherits tail position; a trailing expression statement or an explicit `return` at that position is tail; every earlier statement is non-tail.
- `return value` — `value` inherits tail position.
- `if`/`match`/a nested block in tail position — each branch body, arm body, or the block's own tail inherits tail position.
- Everything else is non-tail: a call under a `let`, nested inside a larger expression (only the outermost call of `f(g(x))` could be tail, and it is not, since its result feeds another operation), a binary operand, an argument, or a loop body.

A postcondition-guarded `return f(x)` is automatically excluded: lowering rewrites it into a `let`-bound result followed by the postcondition checks and a final `return`, so the call sits in a `let` initializer — never in tail position — by the time this analysis runs, with no special case required.

### VM frame reuse

A dedicated tail-call opcode carries the same operand layout as an ordinary call, minus a destination register (a tail call's result flows to the *current* frame's caller, not to a register in the current frame). Executing it reuses the top frame instead of pushing a new one: the callee's function pointer and register file replace the current frame's, `has_caller`/`result_reg` are preserved unchanged, and the instruction pointer resets to the callee's entry — call depth (`frames.size()`) does not grow. An eventual return hands its result to the original caller exactly as if the whole tail chain had been one call. The bytecode compiler emits the tail-call opcode, with no following return opcode, at exactly the sites the HIR pass marked, in place of an ordinary call followed by a return.

### LLVM `musttail`

At a return site whose value is a marked tail call:

1. Compute the callee `llvm::Function*` as for an ordinary call.
2. Check preconditions: callee return type equals the current function's return type (scalar, pointer, or both `void`); same calling convention; the call is immediately followed by the matching `ret` with nothing between.
3. If they hold: mark the call `musttail`, then emit the matching `ret` directly after — `musttail` requires the `ret` to immediately consume the call's result.
4. If they do not hold: emit an ordinary call with the non-binding `Tail` hint and a normal return. Correctness is unaffected; only the stack-space guarantee weakens for that call.

`musttail` is never emitted for a generator step, an indirect/closure call, or an intrinsic call. A unit-returning tail call that feeds a match arm's value (rather than being the function's own tail) is not in function-tail position and is excluded even though it superficially resembles one.

### Diagnostics

None planned for v1 — the optimization is silent, matching decision 1. If a future explicit `become` keyword lands (see below), that construct would get a teacher-quality diagnostic explaining precisely why a given call is not a tail call.

## Future extensions (explicitly out of scope for v1)

1. **`become f(x)`** — a keyword requiring a tail call, diagnosing when the call site is not eligible.
2. Tail calls through closures (indirect calls, both backends).
3. Self-recursion lowered to a loop as an AOT alternative to `musttail`, sidestepping the LLVM verifier for the common self-recursive case.
4. Tail calls inside generator step functions.

## Implementation status

Not implemented. No tail-call opcode exists in the bytecode VM (calls unconditionally push a frame), no `musttail`/tail-call marking exists in the LLVM backend, and no HIR representation of tail-position eligibility exists. `spec/tail-call-optimization.md` is the design source this chapter is drawn from; nothing in it has landed.

## See also

- [Advanced Concurrency: Execution Graphs](40-execution-graphs.md) and [Advanced Cancellation](41-advanced-cancellation.md) — other advanced-layer chapters built on the same coroutine transform whose generator step functions are excluded from tail-call eligibility here.
