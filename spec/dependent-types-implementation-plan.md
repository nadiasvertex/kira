# Dependent / Refinement Types Implementation Plan

Implements the design from `kira-reference.md` (§Dependent Types, §Refinement
Types, §State Machines in Types) and its interactions with §Contracts,
§Checked Arithmetic, and §Compile-Time Semantics (the Evaluation/Reasoning
split). Written 2026-07-12 against the state of the tree at that date.

Should be implemented by Opus.

## Where the compiler stands today

Already done:

- **Surface syntax.** `refinement_type` (base + `where` predicate) parses
  (`src/parser/ast.h:501`); value type-parameters (`n: usize`) parse
  (`type_param.is_value_param`); contracts (`pre`/`post`/`invariant`) parse
  and are type-checked (predicate bool-ness, `pure`-only calls —
  `check.cpp:2262`, `check_test.cpp:1003`).
- **Const-generic literals.** A literal type argument (`vec[T, 3]`) interns
  as a `const_value_kind` type (`types.h:55`, `types.cpp::const_value`), so
  distinct lengths are distinct types; a bare identifier naming an in-scope
  value param keeps the slot generic (`check.cpp:1510`–`1568`).
- **The Evaluation subsystem exists.** `src/comptime/eval.*` interprets
  ordinary Kira at compile time — usable for closing arithmetic over known
  values.

Not done — the actual work:

- **Refinements are a no-op.** `resolve_type` on a `refinement_type` returns
  the base type (`check.cpp:1309`) — the predicate is checked for
  well-formedness, then discarded.
- **No symbolic value arithmetic.** `m + n`, `n + 1` in a type-argument slot
  resolve to `unknown` — deliberately, per the comment at `check.cpp:1518`,
  which cites `spec/dependent-types-design.md`. **That document does not
  exist**; Phase 0 writes it and fixes the dangling reference.
- **No Reasoning subsystem.** The spec's constraint solver — the thing that
  discharges refinement predicates, dependent-type obligations, and static
  contract conditions — has no code at all.
- **No typed proofs.** `index[n].try_from(raw)` (`option`-returning runtime
  proof) doesn't exist; nor does refined→base widening or base→refined
  obligation.
- **Contracts are never lowered** to runtime checks in either backend
  (`spec/todo.md` item 4) — relevant because refinements and contracts are
  spec'd to share one enforcement story (prove statically, check at runtime
  otherwise).

## Strategy

Three decisions that shape everything below:

1. **Fix the solver's scope in writing before writing it (Phase 0).** The
   entire risk of this feature is solver scope creep. Commit to
   quantifier-free **linear integer arithmetic** (a difference/octagon-style
   or small Presburger fragment) plus treating calls to `pure` functions as
   opaque uninterpreted terms (equal args ⇒ equal results, nothing more).
   No general SMT, no nonlinear reasoning, no quantifiers. Everything
   unprovable inside that fragment gets the *graceful* path: a
   teacher-quality diagnostic pointing at `try_from` or a contract. The spec
   only promises "statically where possible" — the fragment is the
   definition of "possible".
2. **Two representations, one canonical form.** Symbolic value expressions
   in types (`m + n`) normalize to an interned canonical linear polynomial
   (sorted terms, folded constants) so `m + n` and `n + m` are the *same*
   `type_id` — preserving the type table's id-equality invariant (and its
   `std::deque` storage; see memory note). Refinement predicates likewise
   canonicalize before interning/solving.
3. **Refinements are declaration-nominal, predicate-structural.** Each
   `type positive = int32 where ...` declaration mints its own named type
   (like `fresh_existential`, `types.h:148`), but the solver sees through it
   to (base, canonical predicate). Two identically-predicated refinements
   are distinct types to the user, interchangeable facts to the solver.
   This keeps diagnostics nominal ("expected `positive`") and proving
   structural.

Every phase ends with regression tests (`check_test.cpp`, semantic stress
corpus; backends' stress corpus once lowering is touched) and a warning-free
build. Diagnostics follow the compiler-is-a-teacher standard — an
unprovable obligation must say what fact was needed, what facts were
available, and both discharge routes (strengthen a type/contract upstream,
or `try_from` at the site).

---

## Phase 0 — Write `spec/dependent-types-design.md`

The missing design doc that `check.cpp:1518` already cites. Contents: the
solver fragment (exact grammar of provable predicates), the canonical
polynomial form, the fact-collection rules (what enters the environment and
where), the obligation sites, the failure UX, and what is explicitly out of
scope (nonlinear arithmetic, quantified predicates, refinements over
non-integer bases beyond equality). Small, but it is the contract every
later phase codes against.

## Phase 1 — Symbolic const generics

Make value-parameterized types real beyond literals. All in `src/semantic/`.

- New `type_table` entry kind for a **symbolic value** — a canonical linear
  polynomial over value-parameter variables with a `usize`/integer
  underlying type (`const_value_kind` becomes the constant-only special
  case). `resolve_type_args` (`check.cpp:1524`) learns to build it from
  arithmetic expressions over in-scope value params and literals; closed
  expressions fold via `comptime::eval` to a plain `const_value`.
- **Unification over value slots**: matching `vec[T, n + 1]` against a
  concrete `vec[int32, 3]` solves `n = 2`; against `vec[T, 0]` fails with a
  diagnostic that explains the arithmetic ("`n + 1` can never be `0` for
  `n: usize`" — the spec's `head`-on-empty-vec example). Linear solving
  only; anything else is a unification failure with the graceful message.
- Value params of **user sum types** (`S: conn_state`) unify by variant
  equality — this alone delivers §State Machines in Types; add a stress
  corpus for the `connection[open]`/`connection[closed]` example.
- Monomorphization: lowering (`hir/`) receives fully-solved const values;
  reject (diagnose) any `pub` surface that would require polymorphic
  arithmetic at runtime.

## Phase 2 — Refinement types as real types

- New `refinement_kind` `type_table` entry: display name, base `type_id`,
  canonical predicate (an interned expression over `self` and the
  declaration's value params). Declaration-nominal identity per Strategy #3.
  `resolve_type` stops erasing at `check.cpp:1309`.
- **Widening is free**: a `positive` is accepted wherever `int32` is
  expected (assignment, args, arithmetic — the value participates as its
  base type). Implement as an implicit coercion edge in the checker.
- **Narrowing is an obligation**: using a base (or differently-refined)
  value where a refinement is expected emits a *proof obligation* recorded
  against the expression, discharged in Phase 3. Until Phase 3 lands, keep
  a temporary flag that downgrades undischarged obligations to the
  "unproven" diagnostic so Phase 2 is testable standalone.
- `packed`-on-refinement stays an error (already spec'd, `ast.h:1547`).

## Phase 3 — The Reasoning solver

The core phase. New module: `src/semantic/reason.{h,cpp}` (one public
entity per header, per conventions: a `solve(facts, obligation)` free
function over the canonical forms).

- **Facts environment**, built per checked function from: parameter types
  (each refined param contributes its predicate with `self ↦ param`),
  value-param domains (`n: usize` ⇒ `n ≥ 0`), `pre` conditions, refinement
  predicates of locals, and — later, Phase 5 — path conditions.
- **Obligation sites**: base→refined coercions (Phase 2), refined-param
  call arguments, `pre` at call sites, `post`/`invariant` at definition
  sites, array indexing `v[i]` (prove `i < n` ⇒ elide the bounds check),
  checked-arithmetic overflow ("the result fits" as an ordinary
  precondition, per spec §Checked Arithmetic).
- **Engine**: negate obligation, conjoin facts, decide unsatisfiability in
  the linear fragment; pure-function calls as uninterpreted terms. Keep it
  deterministic and total — no timeouts, no search explosion; the fragment
  is chosen so decision is cheap.
- **Failure UX** is a first-class deliverable: show the goal, the nearest
  facts, and the two fixes. Add negative tests asserting on diagnostic
  wording, as `check_test.cpp` already does for predicate bool-ness.

## Phase 4 — Typed runtime proofs: `try_from`

- Every refinement type gets an intrinsic `try_from(base) ->
  option[refined]`: lower to "evaluate predicate on the value; `@some`
  (same representation, refined type) or `@none`". The predicate must be
  runtime-evaluable — refinements are over pure expressions already, so
  this is the same expression compiled normally.
- After `if let @some(i) = index[n].try_from(raw)`, `i` simply *has* the
  refined type — no flow analysis needed; the option unwrap is the proof.
  This is the designed escape hatch for everything Phase 3 can't prove, so
  land it immediately after (or interleaved with) Phase 3.
- Backends: HIR erases `refinement_kind` to its base for layout (layout is
  untouched — refinement is a compile-time fact, `src/runtime/layout.*`
  never sees it); only `try_from`'s predicate check materializes as code.
  VM first, then LLVM, per the usual order; extend the codegen stress
  corpus.

## Phase 5 — Flow-sensitive narrowing

- Feed **path conditions** into the facts environment: inside
  `if x > 0:`, the fact `x > 0` holds, so passing `x` where `positive` is
  expected discharges statically; same for `match` guards and refuted
  branches (`else` gets the negation). Scope facts exactly to the
  dominated region; mutation of `x` invalidates its facts.
- This is deliberately *after* `try_from`: the language is complete without
  narrowing (the proof type covers it); narrowing is ergonomics. Keep the
  fact-collection rules in the design doc honest about what flows and what
  doesn't (no narrowing through function calls, closures, or `&mut`).

## Phase 6 — Contracts on the same solver + runtime lowering

- Route contract conditions through Phase 3: statically-refuted `pre` at a
  call site is a compile error; statically-proven checks are marked
  elidable. This also closes `spec/todo.md` item 4: lower unproven
  `pre`/`post`/`invariant` to runtime checks (panic with the contract's
  message) in both backends — debug builds check, the existing
  release-elision flag skips.
- Struct `invariant` enforcement at construction and mutation boundaries,
  as spec'd, with the invariant entering the facts environment for any
  value of that type.
- End-to-end tests tying the features together: `safe_get` with `index[n]`
  (bounds check provably elided — assert on emitted bytecode), overflow
  proofs on literal arithmetic, the spec's `sqrt`/`reserve` examples.

## Phase 7 — Polish and hardening

- Refinements/const-generics through the remaining surface: `some Trait`
  returns, generics bounds (`concept` value constraints like
  `size_of[T]() <= 64` route through the same solver), module metadata
  (protobuf) so refined signatures survive separate compilation, error
  recovery (`k_unknown_type` must keep unifying with everything so one bad
  predicate doesn't cascade).
- Fuzz-ish stress corpus: `semantic_stress` cases mixing solved/unsolved
  obligations, deep `where` nesting, and value-param arithmetic at the
  limits of the fragment.

---

## Sequencing and risk

Dependency chain: 0 → 1 → 2 → 3 → 4 → (5, 6 in either order) → 7.
Phases 1–2 are useful on their own (state-machine types ship after
Phase 1); the feature is honestly usable after Phase 4.

Highest-risk items:

1. **Solver scope creep** (Phase 3) — the fragment defined in Phase 0 is a
   hard boundary; every "just add multiplication" temptation converts a
   total decision procedure into a search problem. Extensions go through a
   design-doc revision, not a patch.
2. **Type-identity subtleties** (Phases 1–2) — canonical forms must be
   truly canonical or id-equality silently splits types that should be
   equal. Property-test the normalizer (commuted/reassociated expressions
   intern identically). Respect the `type_table` `std::deque` invariant
   (entries must not move; see memory: type-table-deque-fix).
3. **Diagnostic quality under failure** (Phases 3, 5) — an unprovable
   obligation with a bad message makes the feature feel broken rather than
   conservative. Budget real time for the failure UX; it is the product.
