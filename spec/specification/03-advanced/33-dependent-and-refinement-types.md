# 33. Dependent and Refinement Types

**Status:** Partial

Types that carry values (`vec[T, n]`), refinement types (`type positive = int32 where self > 0`), the reasoning solver's exact fragment, typed runtime proofs, state machines in types, and interaction with contracts.

This chapter is the normative reference for `spec/dependent-types-design.md`, folded in and re-voiced; that document's precision on the reasoning fragment takes priority over the thinner tutorial account previously in `kira-reference.md`. See [Compile-Time Semantics § Two subsystems](32-compile-time-semantics.md#two-subsystems-evaluation-and-reasoning) for how evaluation and reasoning divide the work in general.

## The two subsystems, concretely

*Evaluation* (`src/comptime/eval.*`) runs ordinary Kira on **known** values. *Reasoning* (`src/semantic/reason.*`) proves facts about **unknown** values. They never substitute for one another:

- `vec[T, 2 + 3]` is evaluation: the expression is closed, so it folds to the constant `5` and the type is `vec[T, 5]`.
- `vec[T, m + n]` is reasoning: `m` and `n` are unknown, so the length is a *symbolic* value, and any question about it (is `m + n` the same length as `n + m`?) is a proof, not a computation.

Governing rule: **fold if closed, symbolize otherwise.** A value expression in a type position is evaluated when every name in it is a compile-time constant, and otherwise becomes a canonical symbolic term.

## Dependent types carrying values

```kira
# vec[T, n] carries its length in the type
type vec[T, n: usize] = { data: array[T, n] }

def concat[T, m: usize, n: usize](a: vec[T, m], b: vec[T, n]) -> vec[T, m + n]:
    ...

# head requires length >= 1 — calling head on an empty vec is a compile error
def head[T, n: usize](v: vec[T, n + 1]) -> T:
    ...
```

### Canonical value terms

Every compile-time value that appears in a type — an array length, a const generic argument, a refinement's parameter — is one of:

| Form | Interned as | Example |
|---|---|---|
| A closed integer | `const_value_kind` | `3` in `vec[T, 3]` |
| A linear polynomial over value params | `symbolic_value_kind` | `n + 1` in `vec[T, n + 1]` |
| A sum-type variant | `const_variant_kind` | `open` in `connection[open]` |

**The linear polynomial.** A `symbolic_value_kind` entry carries a canonical linear polynomial `c0 + c1*v1 + ... + ck*vk`, with non-zero `int64_t` coefficients and distinct value-parameter names sorted lexicographically. Canonicalization folds constants, combines like terms, and drops zero coefficients; a polynomial with no terms degrades to `const_value_kind`, so every value has exactly one representation (`src/semantic/linear_poly.h`).

This is what makes type identity work: `m + n` and `n + m` canonicalize to the same polynomial, intern to the same key, and are therefore the same `type_id` — id-equality remains the whole of type equality.

Only `+`, `-`, unary `-`, and multiplication **by a literal** build a polynomial. `m * n`, `n / 2`, `n % 4`, and any function call in a type argument are **outside the fragment**: they resolve to `unknown`, which unifies with everything and diagnoses nothing — the compiler's standing policy for "I don't know" (`k_unknown_type`). These are not errors; they are simply not reasoned about.

**Value-slot compatibility.** Two value slots are compatible when the equation `P = Q` between their polynomials is satisfiable over the parameters' domain (`usize` and the unsigned family: `v >= 0`; the signed family: unconstrained). A single linear equation is decided exactly and cheaply:

- No variables on either side: compatible iff the constants are equal (the plain-`const_value` case).
- Variables present: let `t` be the constant difference and `g` the gcd of the coefficients. Unsatisfiable when `g` does not divide `t`, or when every variable is non-negative-domained, every coefficient shares a sign, and `t` lies on the wrong side of zero.

This is what makes `head[T, n: usize](v: vec[T, n + 1])` reject an empty `vec[T, 0]`: matching `n + 1` against `0` needs `n = -1`, and `n: usize` cannot be negative, so the call is rejected with the arithmetic explained. Anything satisfiable is accepted; the compiler does not *solve for* `n` and propagate it — an unresolved-but-satisfiable value slot behaves like any other unresolved generic parameter, exactly as `T` does.

**Variant values.** A value parameter typed as a user sum type (`S: conn_state`) takes a bare variant as its argument (`connection[open]`), interning as `const_variant_kind` and comparing by variant identity only — no arithmetic, no ordering. This is all that state machines in types (below) need, and it is total.

## Refinement types

```kira
type positive        = int32 where self > 0
type index[n: usize] = usize where self < n

def safe_get[T, n: usize](v: array[T, n], i: index[n]) -> T:
    ...
```

A refinement declaration mints a `refinement_kind` type table entry: a base type, the declaration's resolved value/type arguments, and a predicate — the `where` expression, in which `self` names the constrained value.

- **Identity is declaration-nominal.** `type a = int32 where self > 0` and `type b = int32 where self > 0` are different types, spelled differently in diagnostics, exactly like two structs with identical fields. Two instantiations of the same declaration with the same arguments are the same type (`index[n]` interns once per `n`). The solver, however, sees straight through the name to `(base, predicate)` — nominal to the user, structural to the prover.
- An *anonymous* refinement written inline (`def f(x: int32 where self > 0)`) mints a refinement keyed on its predicate AST node, one type per written occurrence.

### Widening and narrowing

- **Widening is free.** A `positive` is usable wherever its base `int32` is: the refinement is a compile-time fact about the value, not a different representation. Arithmetic on a `positive` operates on `int32` and produces `int32` — refinements do not propagate through operators.
- **Narrowing is an obligation.** Using an `int32` (or a differently-refined `int32`) where a `positive` is expected raises a proof obligation: `predicate[self := <the expression>]` must follow from the facts in scope. Provable ⇒ silently accepted. Unprovable ⇒ a diagnostic naming the goal, the available facts, and both ways out (below).

Layout never sees a refinement: HIR resolves `refinement_kind` to its base before lowering, so a `positive` is an `int32` in every backend. The single exception is `try_from` (below), whose predicate compiles to an ordinary runtime expression.

## The reasoning fragment

**Quantifier-free linear integer arithmetic over uninterpreted atoms.** This is the whole of it, and it is a hard boundary — extending it is a revision of the design document, not a patch to `src/semantic/reason.cpp`.

### Atoms

An atom is an opaque, canonically-keyed term the solver treats as a single unknown integer:

| Source | Atom key |
|---|---|
| A local / parameter / value param | the name |
| A field access `x.len` | `x.len` |
| A call to a `pure` function `f(a, b)` | `f(<a>,<b>)` |

Calls are **uninterpreted**: `f(x)` equals `f(x)`, and nothing else is known about it — equal arguments imply equal results, the only law. A call to a non-`pure` function is not an atom at all; the containing constraint is dropped as a fact, or unprovable as a goal, since an effectful call has no stable value to reason about.

### Constraints

A constraint is `poly RELOP 0` over atoms, `RELOP ∈ {=, ≠, ≥, >}` (`<`, `≤` normalize by negating the polynomial). A boolean term that is not a comparison (a call returning `bool`, e.g. `is_sorted(v)`) enters as the constraint `atom = 1` — deliberately the weakest thing still useful: an identical fact discharges an identical goal and nothing more.

`and` is conjunction; `not` negates. `or` in a **goal** is discharged by proving either disjunct; `or` in a **fact** is dropped (weakening a fact set is always sound — it can only make the compiler prove less).

### The decision procedure

To discharge goal `G` from facts `F`: decide whether `F ∧ ¬G` is unsatisfiable. `¬G` is a conjunction, or (for a negated equality) a two-way case split; each case is a conjunction of linear constraints, decided by Fourier–Motzkin elimination with integer tightening — bounds are rounded inward (`x ≥ 3/2` becomes `x ≥ 2`) before comparison.

Real-relaxation unsatisfiability implies integer unsatisfiability, so a "proved" answer is always sound. The converse does not hold: an integer-unsatisfiable system can look satisfiable over the rationals, in which case the compiler answers "cannot prove" and takes the graceful path. This asymmetry is deliberate: the solver is **total** — it always terminates, no timeout, no search — at the cost of being incomplete. Kira never silently accepts an unproven obligation, and never hangs trying to prove one.

Elimination is bounded: obligations with more than a small number of distinct atoms (`k_atom_limit`) are answered "cannot prove" rather than risking Fourier–Motzkin's exponential blowup. A bound the user can hit is preferred over a compiler that stops responding.

### Explicitly out of scope

Not in the fragment, now or by patch:

- Nonlinear arithmetic (`m * n`, `n * n`, division, modulo).
- Quantified predicates (`forall i, v[i] > 0`).
- Reasoning about the *contents* of aggregates (lists, maps, strings) beyond uninterpreted-atom equality.
- Reasoning about floats. A refinement on a float base (`type prob = float64 where self >= 0.0`) is legal and enforced by `try_from` and by runtime contract checks, but never *proved* statically.
- Bit-level reasoning, overflow semantics of the underlying machine word beyond the declared range of the type.
- Narrowing through a function call, a closure, or an `&mut` alias.

## Facts and obligations

### What enters the facts environment

Per function being checked, in this order:

1. **Value-parameter domains.** `n: usize` contributes `n ≥ 0`.
2. **Refined parameter types.** A parameter `i: index[n]` contributes its predicate with `self := i`, i.e. `i < n`.
3. **Refined local bindings.** Same rule, at the point of binding.
4. **Array lengths.** A parameter `v: array[T, n]` contributes `len(v) = n`, which is what lets `v[i]` with `i: index[n]` prove its bound.
5. **Preconditions.** Each `pre` of the function under check is a fact inside its body (an *obligation* at each call site, a *fact* within the callee — this duality is the whole of contract reasoning; see [Contracts](34-contracts.md)).
6. **Path conditions.** Inside `if c:`, `c` holds; in the `else`, `¬c` holds. Inside a `match` arm, the arm's guard holds. Facts are scoped exactly to the dominated region.

A fact is **invalidated** by any assignment to a name it mentions, or by passing that name as `&mut`. There is no narrowing through a call.

### Obligation sites

| Site | Goal |
|---|---|
| Base → refined coercion (assignment, argument, return, field init) | the refinement's predicate at the value |
| Indexing `v[i]` | `i < len(v)` — provable ⇒ the bounds check is elided |
| `pre` at a call site | the precondition, with parameters substituted by the arguments |
| `post` / `invariant` at a definition | the condition, against the body's facts |
| Checked arithmetic | "the result fits" — an ordinary precondition on the operation |

A *refuted* obligation (the negation is provable) is always a compile error: the code is definitely wrong. An obligation that is merely *unproven* takes the graceful path.

## The graceful path — failure UX

An unprovable obligation is not a solver failure to apologize for; it is the normal, designed state of a conservative prover, and the diagnostic is the product. It always says three things:

1. **The goal** — the exact fact needed, in source terms (`i < len(v)`), not solver terms.
2. **The facts** — what was actually known about the values involved, so the gap is visible rather than guessed at.
3. **Both ways out** — strengthen a type or contract *upstream* (make the parameter an `index[n]`, add a `pre`), or prove it *here* with a runtime check (`index[n].try_from(i)`).

```
error: cannot prove `i < n` for this argument
  --> src/grid.kira:12:15
   |
12 |     safe_get(v, i)
   |                 ^ expected `index[n]`, found `usize`
   |
 note: known here: `n >= 0`, `i >= 0`
 help: either constrain `i` upstream — take it as `index[n]` — or prove it
       here:
           if let @some(idx) = index[n].try_from(i):
               safe_get(v, idx)
```

## Typed runtime proofs

Every refinement type has an intrinsic:

```kira
index[n].try_from(raw: usize) -> option[index[n]]
```

It evaluates the predicate on the value and yields `@some(value)` (same representation, refined type) or `@none`. After `if let @some(i) = ...`, `i` *has* the refined type — the unwrap **is** the proof; no flow analysis is required to see it. This is the designed escape hatch for everything the fragment cannot decide.

```kira
if let @some(i) = index[n].try_from(raw_index):
    v[i]    # i is now index[n] — access is safe, no bounds check needed
```

## State machines in types

Types parameterized over state values enforce valid operation sequences at compile time:

```kira
type connection[S: conn_state]

def connect(addr: str)              -> connection[closed]: ...
def open(c: connection[closed])     -> connection[open]:   ...
def send(c: connection[open], data: bytes) -> unit:        ...
def close(c: connection[open])      -> connection[closed]: ...
```

Calling `send` on a closed connection is a compile error, diagnosed by variant-value-slot incompatibility (above). No runtime state field is needed — the state is erased after compilation.

## Interaction with contracts

Contracts and refinements share one enforcement story:

> Prove it statically; check it at runtime when you cannot; refuse it when you can prove it false.

A `pre` proven at a call site is compiled away entirely. A `pre` refuted at a call site is a compile error. A `pre` neither proven nor refuted lowers to a runtime check that panics with the contract's message — elidable in release with `--no-contract-checks`, which is the programmer's assertion that it holds by other means.

A struct `invariant` is checked at construction and at mutation boundaries, and enters the facts environment for every value of that type — an `invariant self.value > 0` means a `positive_int`'s field needs no further proof to be used where a `positive` is expected. See [Contracts](34-contracts.md) for the full `pre`/`post`/`invariant` surface.

## Example

The stress corpus `src/testdata/semantic_stress/027_dependent_and_refinement_types.kira` exercises this chapter end to end: symbolic const generics (`commutes`, `reassociates`), refinements proven from a literal and from an already-refined value, widening across an operator, provable and unprovable indexing, flow-sensitive narrowing (`if y > 0: needs_positive(y)`), `try_from` runtime proofs, contract facts, struct invariants, and the explicitly-out-of-scope nonlinear case (`matrix[rows, cols]`), all in one file that is expected to check with **no** diagnostics.

## Implementation status

Substantially implemented, ahead of `spec/dependent-types-implementation-plan.md`'s "Where the compiler stands today" section (dated 2026-07-12) — that document's "not done" list (refinements as no-ops, no symbolic value arithmetic, no reasoning subsystem, no typed proofs, contracts never lowered) no longer reflects the tree:

- **Symbolic const generics** — `symbolic_value_kind`, the canonical linear polynomial, and value-slot compatibility are implemented in `src/semantic/linear_poly.{h,cpp}` and `src/semantic/types.{h,cpp}`, unification over value slots in `check.cpp`.
- **Refinement types as real types** — `refinement_kind` type entries, `resolve_type`'s `ast::node_kind::refinement_type` case (`check.cpp:1847`), widening/narrowing, and predicate substitution (`refinement_subst`, `check.cpp:2862`) are implemented, not the erase-to-base no-op the implementation plan describes.
- **The reasoning solver** — `src/semantic/reason.{h,cpp}` implements the fragment described above (`relation`, `constraint`, `proof_result` in `reason.h`), exercised by `src/semantic/reason_test.cpp`.
- **Typed runtime proofs** — `<refinement>.try_from(value)` is recognized and lowered (`check.cpp:9783`, `infer_refinement_try_from`, `build_try_from_fragment`, `~check.cpp:10288`–`10394`).
- **Flow-sensitive narrowing** (design doc's Phase 5) and **contracts on the same solver plus runtime lowering** (Phase 6) — both are exercised by the stress corpus's `narrowed`/`narrowed_by_conjunction` cases and by `hir_contract_check` lowering in both backends (`src/hir/lower.cpp`, `src/bytecode_compiler/compile.cpp`, `src/llvm_codegen/codegen.cpp`).

Still matching the design doc's stated scope exactly (i.e., not gaps, but the deliberate boundary of the fragment): nonlinear arithmetic, quantified predicates, aggregate-content reasoning, float reasoning, and narrowing through calls/closures/`&mut` are all unimplemented **by design** — see "Explicitly out of scope" above, not a to-do list.

Not verified by this chapter's evidence gathering (design-doc Phase 7 "polish" territory — value-parameterized types through `some Trait` returns, concept value constraints routed through the solver, module-metadata survival of refined signatures across separate compilation): treat as unconfirmed rather than either implemented or missing.

## See also

- [Compile-Time Semantics](32-compile-time-semantics.md) — the evaluation/reasoning split in general.
- [Contracts](34-contracts.md) — `pre`/`post`/`invariant`, the same solver, runtime lowering, `--no-contract-checks`.
- [Concepts](35-concepts.md) — value constraints like `size_of[T]() <= 64` are intended to route through this same solver.
