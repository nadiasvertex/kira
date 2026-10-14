# 63. Type Checking and Inference

**Status:** Implemented

How the reference compiler checks types: the checker's phases, the inference engine it is built on, how generics are checked and instantiated, and what the checker hands to later stages.

## The checker

The checker (`src/semantic/check*.cpp`) is one class that walks every module in a session. Its output, `checked_types`, is a set of side tables keyed by AST node: the type of every expression, the function or instance every call resolved to, operator and index dispatch, literal conversions, generic instances, elided checks, and drop plans. HIR lowering reads these tables; it never re-infers anything.

A session runs in this order:

1. Register `static def` functions and decide which bracketed parameters are bounded type parameters and which are value parameters.
2. Materialize parameterized-module (functor) instantiations, so the `impl`s and `extend`s they declare exist before the method table is built.
3. Build the method table and check impl coherence ([Coherence and the Orphan Rule](../02-intermediate/21-coherence-and-orphan-rule.md)).
4. Run item-level splices and `deriving` clauses. These execute compile-time code, which may check functions out of order; each `impl` they produce is added to the method table and coherence records as it is made.
5. Check functor bodies, then each file in turn. After each file, the generic instances that file asked for are made, so their diagnostics appear next to the code that caused them.
6. Make any remaining instances and settle any results still open.

## The inference engine

Inference lives in `src/semantic/infer/` as a set of small, separately tested parts.

- **Metavariable store** (`infer_ctxt`). One union-find over type ids holds every unknown, whatever it ranges over: a type, a type constructor of known arity (the `F` in `functor[F]`), or a compile-time value (the `n` in `vec[T, n]`). Generic parameters, const generics, and higher-kinded heads therefore share one mechanism. The store also records a cause for each constraint.
- **Unifier** (`unify`). Applies three rules in order. Two rigid types unify structurally, and different constructors are a mismatch. A variable-headed application such as `F[A] ~ option[int32]` solves by matching the outermost nominal constructor, which keeps higher-kinded inference decidable with one most-general answer ([Higher-Kinded Traits](../03-advanced/37-higher-kinded-traits.md)). Value slots such as `vec[T, n + 1] ~ vec[T, 3]` go to the value solver. A constraint outside these rules is postponed, not failed, because its shape may still become known.
- **Value solver** (`value_solver`). Decides and solves a single linear equation over integers. It distinguishes flexible variables (the callee's `n`, which the call determines) from rigid ones (the caller's own `m`, fixed but unknown). So `n + 1 = m + 1` solves to `n := m`.
- **Obligation queue** (`obligations`). Method resolution, trait selection and refinement narrowing are decisions that may need to wait until a type is known. They share one queue: each obligation is retried when something it depends on is solved, and reported if it never can be.
- **Blame** (`blame`). Constraints are kept after they are solved. When constraints disagree, blame chooses which one is the mistake by majority (four branches say `int32` and one says `str`, so the `str` branch is reported) and builds the message from the decisions that led there. A golden corpus of wrong programs (`src/testdata/inference_diagnostics/`) fixes the expected messages.

Integer and float literals start as open leaves and settle from the context that demands them. Demand is scoped: an operand of `==` or `<` ties to the other operand, but an expected type alone never forces a leaf.

## Generics

A generic function is checked once, against its declared bounds ([Generics and Inference](../02-intermediate/19-generics-and-inference.md)). Inside the body, a type parameter is opaque except for what its `where` bounds and any enclosing `static if` provide. Bounds are enforced at each call site, so an error points at the call that violates them rather than inside the generic body.

Each call names an instance: the template plus the concrete types and constants it was solved with (for example `get$3`). The instance queue (`instance_queue.cpp`) decides when each instance is made. Most are made by substitution: the instance copies the template's checked records with its parameters replaced, and replays only the decisions that depended on a type parameter (`instance_substitution.cpp`). Each backend then compiles every instance as an ordinary concrete function. There is no runtime polymorphism for generics; `box[trait]` and trait objects are the explicit exception ([Trait Objects](../02-intermediate/24-trait-objects.md)).

## Stability checks

`snapshot.cpp` renders every elaboration decision in `checked_types` as stable text ordered by source position. A test diffs this snapshot, so a change that silently resolves a call to a different instance is caught even when every program still compiles.

## See also

- [Proof Solving](64-proof-solving.md) — the reasoning engine the checker calls for refinements and contracts.
- [Compile-Time Execution in the Compiler](65-compile-time-execution-in-the-compiler.md) — how the checker runs code while checking.
