# Dependent and Refinement Types — Design

The contract every phase of `spec/dependent-types-implementation-plan.md`
codes against. It fixes the *reasoning fragment* — exactly which facts the
compiler collects, which obligations it raises, and which of those it can
decide — so that "the compiler proves it statically where possible" has a
precise meaning instead of an aspirational one.

Reading order: this document assumes `kira-reference.md` §Dependent Types,
§Refinement Types, §State Machines in Types, §Contracts, and
§Compile-Time Semantics (the Evaluation / Reasoning split).

## 1. The two subsystems, concretely

*Evaluation* (`src/comptime/eval.*`) runs ordinary Kira on **known** values.
*Reasoning* (`src/semantic/reason.*`) proves facts about **unknown** values.
They never substitute for one another:

- `vec[T, 2 + 3]` is evaluation: the expression is closed, so it folds to the
  constant `5` and the type is `vec[T, 5]`.
- `vec[T, m + n]` is reasoning: `m` and `n` are unknown, so the length is a
  *symbolic* value and any question about it (`is m + n the same length as
  n + m?`) is a proof, not a computation.

The rule the compiler follows: **fold if closed, symbolize otherwise.** A
value expression in a type position is evaluated when every name in it is a
compile-time constant, and otherwise becomes a canonical symbolic term.

## 2. Canonical value terms

Every compile-time value that appears in a type — an array length, a const
generic argument, a refinement's parameter — is one of:

| Form | Interned as | Example |
|---|---|---|
| A closed integer | `const_value_kind` | `3` in `vec[T, 3]` |
| A linear polynomial over value params | `symbolic_value_kind` | `n + 1` in `vec[T, n + 1]` |
| A sum-type variant | `const_variant_kind` | `open` in `connection[open]` |

### 2.1 The linear polynomial

A `symbolic_value_kind` entry carries a **canonical linear polynomial**:

```
poly ::= c0 + c1*v1 + c2*v2 + ... + ck*vk
```

with `ci` non-zero `int64_t` coefficients and `vi` distinct value-parameter
names **sorted lexicographically**. Canonicalization folds constants,
combines like terms, and drops zero coefficients. A polynomial with no terms
is not representable as `symbolic_value_kind` at all — it degrades to
`const_value_kind`, so there is exactly one representation of every value.

This is what makes type identity work: `m + n` and `n + m` canonicalize to
the same polynomial, produce the same intern key, and therefore *are the same
`type_id`*. Id-equality remains the whole of type equality.

Only `+`, `-`, unary `-`, and multiplication **by a literal** build a
polynomial. `m * n`, `n / 2`, `n % 4`, and every function call in a type
argument are **outside the fragment**: they resolve to `unknown`, which
unifies with everything and diagnoses nothing (the compiler's standing policy
for "I don't know" — see `k_unknown_type`). They are not errors; they are
simply not reasoned about.

### 2.2 Value-slot compatibility

Two value slots are compatible when the equation `P = Q` between their
polynomials is **satisfiable** over the parameters' domain (`usize` and the
unsigned family: `v >= 0`; the signed family: unconstrained). For a single
linear equation this is decidable exactly, and cheaply:

- No variables on either side: compatible iff the constants are equal. (This
  is the old `const_value` behavior, preserved.)
- Variables present: let `t` be the constant difference and `g` the gcd of
  the coefficients. Unsatisfiable when `g` does not divide `t`, or when every
  variable is non-negative-domained, every coefficient has the same sign, and
  `t` lies on the wrong side of zero.

This is what makes `head[T, n: usize](v: vec[T, n + 1])` reject an empty
`vec[T, 0]`: matching `n + 1` against `0` requires `n = -1`, and `n: usize`
cannot be negative — so the call is rejected *with the arithmetic explained*,
which is the entire point of the feature.

Anything satisfiable is accepted. The compiler does not *solve for* `n` and
propagate it; a satisfiable-but-not-forced value slot behaves like any other
unresolved generic parameter, exactly as `T` does today.

### 2.3 Variant values

A value parameter whose type is a user sum type (`S: conn_state`) takes a
bare variant as its argument (`connection[open]`). These intern as
`const_variant_kind` and compare by *variant identity only* — no arithmetic,
no ordering. This is all §State Machines in Types needs, and it is total.

## 3. Refinement types

```kira
type positive        = int32 where self > 0
type index[n: usize] = usize where self < n
```

A refinement declaration mints a `refinement_kind` type: a **base** type, the
declaration's resolved value/type arguments, and a **predicate** — the
declaration's `where` expression, in which `self` names the value being
constrained.

Identity is **declaration-nominal**: `type a = int32 where self > 0` and
`type b = int32 where self > 0` are different types, spelled differently in
diagnostics, exactly like two structs with identical fields. Two
instantiations of the *same* declaration with the same arguments are the same
type (`index[n]` interns once per `n`).

The solver, however, sees straight through the name to `(base, predicate)`.
Nominal to the user, structural to the prover.

An *anonymous* refinement written inline (`def f(x: int32 where self > 0)`)
mints a refinement keyed on its predicate AST node — one type per written
occurrence.

### 3.1 Widening and narrowing

- **Widening is free.** A `positive` is usable wherever its base `int32` is:
  the refinement is a compile-time fact about the value, not a different
  representation. Arithmetic on a `positive` operates on `int32` and produces
  `int32` — refinements do not propagate through operators.
- **Narrowing is an obligation.** Using an `int32` (or a differently-refined
  `int32`) where a `positive` is expected raises a proof obligation:
  `predicate[self := <the expression>]` must follow from the facts in scope.
  Provable ⇒ silently accepted. Unprovable ⇒ a diagnostic that names the goal,
  the available facts, and the two ways out (§6).

Layout never sees a refinement: HIR resolves `refinement_kind` to its base
before lowering, so a `positive` is an `int32` in every backend. The single
exception is `try_from` (§5), whose predicate compiles to an ordinary runtime
expression.

## 4. The reasoning fragment

**Quantifier-free linear integer arithmetic over uninterpreted atoms.**
That is the whole of it, and it is a hard boundary: extending it is a
revision of *this document*, not a patch to the solver.

### 4.1 Atoms

An atom is an opaque, canonically-keyed term the solver treats as a single
unknown integer:

| Source | Atom key |
|---|---|
| A local / parameter / value param | the name |
| A field access `x.len` | `x.len` |
| A call to a `pure` function `f(a, b)` | `f(<a>,<b>)` |

Calls are **uninterpreted**: `f(x)` equals `f(x)`, and nothing else is known
about it. Equal arguments imply equal results; that is the only law. A call
to a non-`pure` function is not an atom at all — the containing constraint is
dropped (as a fact) or unprovable (as a goal), since an effectful call has no
stable value to reason about.

### 4.2 Constraints

A constraint is `poly RELOP 0` over atoms, with `RELOP ∈ {=, ≠, ≥, >}` (`<`,
`≤` normalize by negating the polynomial). A *boolean* term that is not a
comparison (a call returning `bool`, e.g. `is_sorted(v)`) enters as the
constraint `atom = 1`, which makes an identical fact discharge an identical
goal and nothing more — deliberately the weakest thing that is still useful.

`and` is conjunction. `not` negates. `or` in a **goal** is discharged by
proving either disjunct; `or` in a **fact** is dropped (weakening a fact set
is always sound — it can only make the compiler prove less).

### 4.3 The decision procedure

To discharge goal `G` from facts `F`: decide whether `F ∧ ¬G` is
unsatisfiable. `¬G` is a conjunction, or (for a negated equality) a two-way
case split; each case is a conjunction of linear constraints, decided by
**Fourier–Motzkin elimination with integer tightening** — bounds are rounded
inward (`x ≥ 3/2` becomes `x ≥ 2`) before comparison.

Real-relaxation unsatisfiability implies integer unsatisfiability, so a
"proved" answer is always sound. The converse does not hold: an
integer-unsatisfiable system may look satisfiable over the rationals, in which
case the compiler answers *"cannot prove"* and takes the graceful path. That
asymmetry is deliberate — the solver is **total** (it always terminates, with
no timeout and no search), at the cost of being incomplete. Kira never
silently accepts an unproven obligation, and never hangs trying to prove one.

Elimination is bounded: obligations with more than a small number of distinct
atoms (`k_atom_limit`) are answered "cannot prove" rather than risking
Fourier–Motzkin's exponential blowup. A bound the user can hit is better than
a compiler that stops responding.

### 4.4 Explicitly out of scope

Not in the fragment, now or by patch:

- Nonlinear arithmetic (`m * n`, `n * n`, division, modulo).
- Quantified predicates (`forall i, v[i] > 0`).
- Reasoning about the *contents* of aggregates (lists, maps, strings) beyond
  uninterpreted-atom equality.
- Reasoning about floats. A refinement on a float base (`type prob = float64
  where self >= 0.0`) is legal and enforced by `try_from` and by runtime
  contract checks, but is never *proved* statically.
- Bit-level reasoning, overflow semantics of the underlying machine word
  beyond the declared range of the type.
- Narrowing through a function call, a closure, or an `&mut` alias (§5.2).

## 5. Facts and obligations

### 5.1 What enters the facts environment

Per function being checked, in this order:

1. **Value-parameter domains.** `n: usize` contributes `n ≥ 0`.
2. **Refined parameter types.** A parameter `i: index[n]` contributes its
   predicate with `self := i`, i.e. `i < n`.
3. **Refined local bindings.** Same rule, at the point of binding.
4. **Array lengths.** A parameter `v: array[T, n]` contributes `len(v) = n`,
   which is what lets `v[i]` with `i: index[n]` prove its bound.
5. **Preconditions.** Each `pre` of the function under check is a fact inside
   its body (it is an *obligation* at each call site, and a *fact* within the
   callee — this duality is the whole of contract reasoning).
6. **Path conditions** (Phase 5). Inside `if c:`, `c` holds; in the `else`,
   `¬c` holds. Inside a `match` arm, the arm's guard holds. Facts are scoped
   exactly to the dominated region.

A fact is **invalidated** by any assignment to a name it mentions, or by
passing that name as `&mut`. There is no narrowing through a call.

### 5.2 Obligation sites

| Site | Goal |
|---|---|
| Base → refined coercion (assignment, argument, return, field init) | the refinement's predicate at the value |
| Indexing `v[i]` | `i < len(v)` — provable ⇒ the bounds check is elided |
| `pre` at a call site | the precondition, with parameters substituted by the arguments |
| `post` / `invariant` at a definition | the condition, against the body's facts |
| Checked arithmetic | "the result fits" — an ordinary precondition on the operation |

A *refuted* obligation (the negation is provable) is always a compile error:
the code is definitely wrong, and saying so is the compiler's job. An
obligation that is merely *unproven* takes the graceful path.

## 6. The graceful path — failure UX

An unprovable obligation is not a solver failure to be apologized for; it is
the normal, designed state of a conservative prover, and the diagnostic is the
product. It must always say three things:

1. **The goal** — the exact fact that was needed, in source terms
   (`i < len(v)`), not in solver terms.
2. **The facts** — what was actually known about the values involved, so the
   user can see the gap rather than guess at it.
3. **Both ways out** — strengthen a type or contract *upstream* (make the
   parameter an `index[n]`, add a `pre`), or prove it *here* with a runtime
   check (`index[n].try_from(i)`).

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

## 7. Typed runtime proofs

Every refinement type has an intrinsic:

```kira
index[n].try_from(raw: usize) -> option[index[n]]
```

It evaluates the predicate on the value and yields `@some(value)` (same
representation, refined type) or `@none`. After `if let @some(i) = ...`, `i`
*has* the refined type — the unwrap **is** the proof, and no flow analysis is
required to see it. This is the designed escape hatch for everything §4
cannot decide, which is what allows the fragment to stay small without making
the language incomplete.

## 8. Interaction with contracts

Contracts and refinements share one enforcement story:

> Prove it statically; check it at runtime when you cannot; refuse it when you
> can prove it false.

A `pre` proven at a call site is compiled away entirely. A `pre` refuted at a
call site is a compile error. A `pre` neither proven nor refuted lowers to a
runtime check that panics with the contract's message — elidable in release
with an explicit flag, which is the programmer's assertion that it holds by
other means.

A struct `invariant` is checked at construction and at mutation boundaries,
and enters the facts environment for every value of that type — an
`invariant self.value > 0` means a `positive_int`'s field needs no further
proof to be used where a `positive` is expected.
