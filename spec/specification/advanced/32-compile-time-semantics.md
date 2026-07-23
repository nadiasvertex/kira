# 32. Compile-Time Semantics

**Status:** Implemented

The execution model shared by `static` evaluation, reflection, contracts, and dependent types: the phase/effect classification, the evaluation/reasoning split, compile-time memory, and the effects compile-time code may cause.

`static` evaluation, reflection, contracts, and dependent types (see [Compile-Time Execution](31-compile-time-execution.md), [Contracts](34-contracts.md), [Dependent and Refinement Types](33-dependent-and-refinement-types.md)) each have their own syntax; this chapter describes the environment they share — what runs at compile time, what it may do, and how the features interact.

## Two axes: phase and effect

Two independent properties classify every function.

- **Phase** — when it runs. Runtime code runs in the finished program. Compile-time code runs in the compiler; `static` forces it. Ordinary code is runtime code the compiler *may* also evaluate at compile time when its inputs are known.
- **Effect** — whether the function is `pure` (referentially transparent) or may cause effects. This is independent of phase.

A function may be:

| | pure | not pure |
|---|---|---|
| runtime-only | effect-free at runtime | ordinary effectful runtime code |
| `static` | evaluated at compile time, effect-free | evaluated at compile time, may emit diagnostics/code |

`pure` is about *effects*; `static` is about *phase*. They do not collapse into one axis.

## Two subsystems: evaluation and reasoning

Compile time has two distinct machineries.

- **Evaluation** (`src/comptime/eval.*`) is an interpreter that runs ordinary Kira — loops, functions, pattern matching — on **known** values to produce compile-time values. `build_lookup_table()`, `deriving`, and quote/splice construction are all evaluation.
- **Reasoning** (`src/semantic/reason.*`) is the constraint solver that discharges refinement predicates, contract conditions, and dependent-type obligations over **unknown** values. It proves; it does not run arbitrary code.

They never substitute for one another. `vec[T, 2 + 3]` is evaluation — the expression is closed, so it folds to the constant `5`. `vec[T, m + n]` is reasoning — `m` and `n` are unknown, so the length is a symbolic term and any question about it is a proof, not a computation. The governing rule: **fold if closed, symbolize otherwise.** See [Dependent and Refinement Types § The two subsystems, concretely](33-dependent-and-refinement-types.md#the-two-subsystems-concretely).

Contracts sit on the seam: a contract *condition* is produced by evaluation, but whether it *holds* is settled by reasoning when possible, and by a runtime check otherwise.

## Compile-time memory

Compile-time evaluation may allocate freely, on a heap that exists only during compilation. Nothing on that heap reaches the binary unless it is *reified* into a `static` constant, at which point it becomes frozen, immutable data baked into the program.

There is no persistent, program-global, mutable compile-time state. Compile-time evaluation is **confluent**: its results never depend on the order in which the compiler elaborates the program. This is what keeps compilation deterministic, parallel, and incremental — a module's meaning cannot depend on side effects left behind by another module.

## Effects at compile time

Compile-time code may cause exactly two effects:

1. **Emitting diagnostics** (as `static assert` does).
2. **Emitting code** (as quoting and splicing do).

Neither is observable *as data* by other compile-time code — there is no API to read back which diagnostics or definitions have already been emitted. So even effectful `static` code stays confluent: the order the compiler happens to run it in cannot change the program's meaning.

## Reflection

Reflection reads the program's structure — types, fields, names, module members. That structure is immutable, so reflection is referentially transparent, and a `pure` function may use it freely:

```kira
pure def field_count[T]() -> usize:
    T.field_count()
```

Reflection resolves at compile time. Because Kira monomorphizes generic code, the type a reflection queries is known statically at each instantiation, so it is computed once per instantiation and its result — an ordinary constant — flows into runtime like any other value. There is no *runtime* reflection: a value carries no type tag to interrogate.

Reflection may invoke only `pure` functions, so it can never cause an effect — which is exactly what makes it safe to use from inside a contract condition (see [Contracts](34-contracts.md)).

## Declarative queries, not registries

Because there is no mutable global compile-time state, program-wide information is not accumulated by having each definition register itself. Instead, a query states the shape wanted and the compiler answers it against the finished program:

```kira
# every type that satisfies the `command` concept, gathered at compile time
static COMMANDS: map[str, command] = map(
    for t in types_implementing[command]() => (t.name(), make_command(t))
)
```

A query observes the complete, elaborated program and returns the same result regardless of elaboration order — "this is the state I want," not "make the state this." A whole-program query naturally depends on the whole program, so it resolves in a late, post-elaboration phase; its result is still a pure function of the final program.

## How the pieces interact

| Question | Answer |
|---|---|
| Can a contract use reflection? | Yes. Reflected data is immutable and resolves to a compile-time constant, usable by the static verifier and, when reified, by a runtime check. |
| Can reflection call pure functions? | Yes — and only pure functions, so no effect can leak through it. |
| Can a pure function inspect types? | Yes. Type reflection is referentially transparent; it resolves at compile time and its result flows into runtime as a constant. |
| Can reflection allocate? | Yes, on the compile-time heap. Nothing survives compilation except values reified into `static` constants. |
| Can compile-time code mutate global state? | No. Compile-time evaluation is confluent; program-wide information comes from declarative queries, never mutation. |

## Implementation status

The evaluation subsystem (`src/comptime/eval.*`), reflection (`src/comptime/reflect.cpp`), and the reasoning subsystem (`src/semantic/reason.{h,cpp}`, `src/semantic/linear_poly.{h,cpp}`) all exist and are exercised by `src/comptime/eval_test.cpp`, `src/semantic/reason_test.cpp`, and the `check_test.cpp`/semantic-stress corpora referenced throughout [Compile-Time Execution](31-compile-time-execution.md) and [Dependent and Refinement Types](33-dependent-and-refinement-types.md). This chapter describes the model those implementations follow; there is no separate "semantics-only" surface to check independently of the constructs in the other chapters.

## See also

- [Compile-Time Execution](31-compile-time-execution.md) — the constructs (`static`, quoting/splicing, reflection call sites) that run inside this model.
- [Dependent and Refinement Types](33-dependent-and-refinement-types.md) — the reasoning subsystem's fragment, in full.
- [Contracts](34-contracts.md) — where evaluation (producing a condition) and reasoning (discharging it) meet.
