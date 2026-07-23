# 34. Contracts

**Status:** Implemented

`pre`/`post`/`invariant` conditions on functions and types: static-vs-runtime enforcement, `return` in postconditions, `--no-contract-checks`, and the `pure`-only requirement on contract predicates.

Contracts attach preconditions, postconditions, and invariants to functions and types. The compiler proves them statically when it can (routing through the reasoning solver, see [Dependent and Refinement Types § The reasoning fragment](33-dependent-and-refinement-types.md#the-reasoning-fragment)) and enforces the rest at runtime.

## Syntax

```kira
def sqrt(x: float64) -> float64
    pre  x >= 0.0
    post return >= 0.0:
    ...
```

- `pre` introduces a precondition; `post` a postcondition; `invariant` (on a `type`) a struct invariant.
- In a `post` condition, `return` names the value the function returns. It is deliberately not called `result`, which would collide with the `result` type.
- Multiple conditions are written one per line, each with an optional message:

```kira
def reserve(buf: &mut buffer, n: usize) -> unit
    pre  n > 0,         "cannot reserve zero bytes"
    pre  n < max_alloc, "requested size exceeds maximum"
    post buf.capacity >= n:
    ...
```

- A struct invariant is attached after the type's field list:

```kira
type positive_int = { value: int32 }
    invariant self.value > 0
```

## Semantics

- **Checked once, at the right boundary.** A `pre` is checked once on entry to the function, so it answers for every caller, including ones the compiler never sees. A `post` is checked at each of the function's exits. An `invariant` is checked wherever a value of the type is constructed or one of its fields is written.
- **Static proof elides the check entirely.** A condition the compiler can already prove from the signature alone — `pre p > 0` on a parameter whose type is `positive` — is not checked at runtime at all, because it cannot fail. This is the same reasoning-solver machinery as refinement narrowing; see [Dependent and Refinement Types § Interaction with contracts](33-dependent-and-refinement-types.md#interaction-with-contracts).
- **Static refutation is a compile error.** When a condition is statically knowable and false, violation is diagnosed at compile time, not deferred to a runtime panic.
- **Otherwise, a runtime check.** A condition that depends on runtime values and is neither proven nor refuted lowers to a runtime check that panics with the contract's message in debug builds.
- **Preconditions are facts inside their own callee.** A function's `pre` is an *obligation* at each call site and a *fact* inside the function's own body — this duality is the whole of contract reasoning (see [Dependent and Refinement Types § What enters the facts environment](33-dependent-and-refinement-types.md#what-enters-the-facts-environment)).
- **`--no-contract-checks`** elides runtime contract checks in release builds. Passing it is the programmer's explicit assertion that all contracts hold by other means; it is not the default (`src/driver/cli.cpp`, `src/hir/lower.h`'s `contract_checks` flag).

### Purity requirement

Only `pure` functions may appear in `pre`/`post`/`invariant` conditions (`check.cpp:4430`; see [Compile-Time Execution § `pure` functions](31-compile-time-execution.md#pure-functions)). This guarantees contract evaluation has no side effects, which is what lets reflection (also `pure`-only) and reasoning treat a contract condition as a pure fact rather than an arbitrary effectful call.

## Example

```kira
type positive_int = { value: int32 }
    invariant self.value > 0

def needs_positive(x: positive) -> int32:
    x

def precondition_is_a_fact(x: int32) -> int32
pre x > 0
: needs_positive(x)
```

The `pre x > 0` on `precondition_is_a_fact` is a fact inside its own body, which is what lets the unconditional call to `needs_positive` (which demands `positive`) discharge statically with no runtime check.

## Lowering

An unproven condition survives past type checking as an `hir_contract_check` node (`src/hir/nodes.h`), tagged with which face of the contract it enforces (`contract_kind`: `pre`/`post`/`invariant`). Both backends lower it to a runtime check: the bytecode compiler (`src/bytecode_compiler/compile.cpp`) and the LLVM backend (`src/llvm_codegen/codegen.cpp`). A condition the checker discharged statically is recorded in `semantic::checked_types::elided_contracts` and never reaches HIR as a check at all.

## Implementation status

Implemented end to end: parsing (`pre`/`post`/`invariant` clauses), type checking (predicate bool-ness, `pure`-only calls — `check_test.cpp:1003`), static discharge/refutation through the reasoning solver, runtime lowering in both backends, and the `--no-contract-checks` release-elision flag. Exercised together with refinement types in `src/testdata/semantic_stress/027_dependent_and_refinement_types.kira` (`reserve`, `reserves`, `precondition_is_a_fact`, `positive_int`) and by `src/testdata/semantic_stress/007_contract_purity.kira`.

## See also

- [Contracts on Trait Methods](../intermediate/23-contracts-on-trait-methods.md) — behavioral-subtyping rules for contracts on trait method implementations (strengthening/weakening across `impl`).
- [Dependent and Refinement Types](33-dependent-and-refinement-types.md) — the solver contracts share, and the facts/obligations model.
- [Compile-Time Execution](31-compile-time-execution.md) — `pure`, the requirement contract predicates must satisfy.
