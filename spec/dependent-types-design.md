# Dependent Types & Generics — Increment Notes

Status: partially implemented (`src/semantic/types.h`/`types.cpp`, `src/semantic/check.cpp`),
covered by `check_test.cpp`. This document records what shipped in this increment and what is
deliberately still missing, since a prior design doc
(`spec/parameter-inference-design.md`) already explains why the checker does not build a
second, earlier monomorphization mechanism ahead of the roadmap's phase-5 LLVM lowering —
this increment stays consistent with that decision rather than relitigating it.

## What was already in place

Before this increment, the parser fully parsed generic type parameters (`[T: show]`),
const-generic value parameters (`[n: usize]`), `concept` declarations, refinement-type
`where` syntax, and `requires`. `check.cpp` already did per-declaration generic instantiation
(`instantiate_user_type`), bound value-params as real compile-time values inside function
bodies, and resolved `concept` constraint subjects/bounds. State-machine dependent types
(types parameterized over marker types, e.g. `connection[open]`/`connection[closed]`) already
worked by construction, since ordinary type-argument identity (`type_table::user_type`'s
id-equality over its `args` vector) already distinguishes them — this increment added
regression tests locking that in, not new mechanism.

## What this increment added

- **Const-generic value identity** (`type_kind::const_value_kind`,
  `type_table::const_value`): a bare integer literal in a value-parameter slot (the `3` in
  `vec[T, 3]`) now interns to a distinct type keyed on `(underlying scalar type, value)`,
  instead of collapsing to `k_unknown_type` the way every non-type-expression argument did
  before. `vec[int32, 3]` and `vec[int32, 5]` are now different types; a function expecting
  one and called with the other gets an ordinary type-mismatch diagnostic. A bare identifier
  naming an in-scope value type-parameter (e.g. `n` used inside `vec[T, n]` in a generic
  function's own signature) still resolves to that parameter, keeping the function generic
  over it — the same way a type parameter `T` already did.
- **Refinement-predicate well-formedness** (`check_type_decl`): a `type X = T where pred`
  declaration now binds `self: T` in scope and requires `pred` to be `bool`, mirroring the
  existing struct-`invariant` check right below it. Previously the predicate was parsed and
  then silently discarded — not even checked for referencing real names or producing `bool`.
- **Generic parameters as call arguments in value position** (`infer_index`): fixed a
  side-effect bug this increment's own concept test surfaced — `size_of[T]()` inside a
  `concept`'s value constraint reported "undefined name `T`", because the generic-
  instantiation short-circuit in `infer_index` ran ordinary value-name resolution on the
  bracketed argument instead of checking the in-scope type-parameter stack first. Fixed
  narrowly: a bare identifier in that bracket position that names an in-scope type parameter
  is now recognized as a type argument, not a dangling value reference.

## What is deliberately still out of scope

- **Arithmetic in dependent-type positions** (`vec[T, m + n]`, `head`'s `n + 1` requirement
  from the spec's own example). `resolve_type_args` still resolves any argument that isn't a
  bare type expression, integer literal, or bare type-parameter identifier — a binary
  expression, a call, anything else — to `k_unknown_type`, exactly as before. Solving this
  needs a real constraint solver (unify `m + n` against a caller's concrete length, prove
  `n + 1 >= 1` implies `n >= 0`, etc.), which is a different, larger project than interning
  literals, and the same "no second monomorphization mechanism before phase 5" reasoning in
  `spec/parameter-inference-design.md` applies here too: a per-call-site arithmetic solver
  would need to re-check generic bodies per instantiation, which the checker's current
  single-pass-per-declaration architecture doesn't support without a real rework.
- **Refinement types staying nominally transparent.** A refined type (`type positive = int32
  where self > 0`) is still fully transparent to its base type today (same as a plain alias
  `type meters = float64`) — assigning an `int32` where `positive` is expected type-checks
  with no proof required. The spec's own example (`index[n].try_from(raw_index)` producing a
  proof-carrying `index[n]`) implies refinements should instead be nominally distinct,
  requiring an explicit conversion. Making that change needs an open design decision this
  increment didn't make: does a refined value implicitly *widen* back to its base (read-only,
  no proof needed) even if narrowing requires one? Is `try_from` a compiler-synthesized method
  on every refinement, or a real trait implementors can opt into? `check.cpp`'s existing stub
  at the "other type-level members (reflection, try_from)" comment still returns
  `k_unknown_type` for any `Type.try_from(...)` call — unchanged by this increment.
- **Static constraint proving.** The spec says the compiler "uses a constraint solver to
  verify predicates statically where possible." Nothing in this increment attempts to prove a
  refinement predicate at a call site (e.g. that a literal `5` satisfies `self > 0`); it only
  checks the predicate is well-formed at its declaration site.
