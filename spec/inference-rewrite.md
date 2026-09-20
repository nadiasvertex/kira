# Rewriting inference: one unifier, one queue, one blame pass

**Status:** Phase 0 done. Nothing else in `src/semantic/` has moved yet.

| Phase | What it makes possible | Status |
|---|---|---|
| 0 | A golden snapshot of every elaboration decision, so the rewrite is falsifiable | **Done** — `src/semantic/snapshot.{h,cpp}`, `snapshot_test.cpp`, golden at `src/testdata/inference_snapshot/session.snapshot` |
| 1 | `infer_ctxt`: one metavariable store over three sorts, with causes | Not started |
| 2 | One `unify`: rigid-rigid, pattern fragment, value slots | Not started |
| 3 | Value slots genuinely *solved*, not merely checked satisfiable | Not started |
| 4 | An obligation queue: methods, trait bounds, refinements, defaulting | Not started |
| 5 | Blame over a retained constraint graph, and the diagnostics it enables | Not started |
| 6 | Elaboration split out of checking — decisions recorded, flushed after solving | Not started |
| 7 | Constraint generation migrated onto the one unifier | Not started |
| 8 | Real metavariables at the leaves (`[]`, unannotated params, literals) | Not started |
| 9 | Generic bodies checked once, abstractly | Not started |

## Why

The checker has no inference *engine*. It has inference *sites*. In
`src/semantic/check.cpp` alone, as of this writing:

| Count | What |
|---|---|
| 28 | `unify_rigid` call sites (`check.cpp:9229`) |
| 67 | separate `std::unordered_map<std::string, type_id>` binding maps |
| 11 | `instantiate_impl_method_for` call sites (`check.cpp:9925`) |
| 20 | places recording a `resolved_callee` |
| 4 | distinct solvers: `unify_rigid`, `solve_generic_params` (`check.cpp:5832`), `solve_from_expected_type` (`check.cpp:6100`), `param_usage_inferrer` (`check.cpp:385`) |
| 6 | `in_*_template_` gates suppressing diagnostics on a body checked twice |

That is not a series of unlucky gaps. It is what the architecture forces,
and the reason is structural: the checker fuses three jobs into one in-order
walk.

1. **Constraint generation** — what the program says about types.
2. **Solving** — what follows from it.
3. **Elaboration** — recording `resolved_callee`s, instantiating impls,
   wiring `from_array`/iterator/index dispatches.

Because (3) happens *during* (1), every type must be final the instant it is
first observed. That single fact explains every workaround in the list above.
`var xs = []` cannot be deferred because a later `xs.push(v)` would elaborate
against `list[?v]` and mint an instance that is never compiled (todo 20).
Unannotated parameters needed a separate syntactic pre-pass because they
cannot participate in the main walk. Integer-literal defaulting is inlined at
`check.cpp:7589` behind a template gate. A generic body is checked twice with
a flag suppressing diagnostics on the abstract pass. These are one missing
abstraction wearing six costumes.

The accumulated cost of point-fixing those sites has already exceeded the
cost of the engine that removes them. This document is that engine.

## The engine

**Bidirectional elaboration + pattern unification over a single metavariable
store whose variables range over three sorts — types, constructors, and
values — with one theory extension, a postponement queue, and blame computed
over a retained constraint graph.**

Deliberately *not* Hindley-Milner: with value-indexed types (ch. 33),
trait-constrained generics (ch. 18), and refinement types, principal types
stop being a useful goal and let-generalization costs error quality for a
language that already requires top-level signatures. Bidirectional
elaboration stays exactly as it is; the engine handles the residue.

### One store, three sorts

`type_table` already embeds values and constructors as types —
`const_value_kind`, `symbolic_value_kind`, `const_variant_kind`,
`ctor_ref_kind`, `param_app_kind` are all `type_id`s (`types.h:41`). So a
single union-find over `type_id` covers type parameters, const generics,
variant values, and higher-kinded heads with no second mechanism. Each
metavariable carries its sort: a type, a constructor of declared arity, or a
value. Sort mismatch is a kind error, which is where ch. 37's existing kind
diagnostics are folded in rather than duplicated.

Today that unity is thrown away by three separate solvers keyed on strings.

### Three unification rules

**1. Rigid-rigid.** Structural descent, as today.

**2. Variable-headed applications** — `F[A] ~ option[int32]`. Ch. 37 already
fixes the restriction:

> Unification for a higher-kinded parameter is **rigid pattern-matching
> only**: `F[A] ~ option[int32]` solves `F = option, A = int32` by matching
> the outermost nominal constructor; `F[A] ~ int32` fails; two flexible
> constructors never unify with each other's applications.

That restriction *is* Miller's pattern fragment — a flex head applied to
distinct rigid arguments. Naming it as such is worth doing, because the
pattern fragment is decidable, **unitary** (one most-general solution, hence
no backtracking and no ambiguity errors), and has a textbook algorithm. Kira
picked the right restriction by instinct; adopting the name buys the theory
and the implementation instead of a bespoke matcher per call site. It is also
why Idris 2 is a live reference for this work rather than a curiosity — same
design point, same fragment, same postponement strategy.

One change to ch. 37's letter: a flex head applied to a non-distinct or
non-rigid argument leaves the fragment, and should **postpone** rather than
fail. Every place ch. 37 currently says "fails" needs re-reading against that
distinction — some are genuine failures, some are merely not-yet.

**3. Value slots**, solved by linear-Diophantine equation solving over the
canonical polynomials of `src/semantic/linear_poly.h`.

### Value slots must be solved, not checked

This is the substantive gap the rewrite closes on the dependent side. Ch. 33
says outright:

> the compiler does not *solve for* `n` and propagate it — an
> unresolved-but-satisfiable value slot behaves like any other unresolved
> generic parameter, exactly as `T` does.

That is todo item 20 again, in the dependent fragment. Unifying
`vec[T, n + 1]` against `vec[T, 3]` *should* yield `n := 2`; today it yields
"satisfiable, carry on", and every downstream consumer is left with an
unsolved `n` that some later site has to guess at. That pressure is what
produced `solve_value_params`, `bind_generic_constant`, and the const-generic
special cases. A single linear equation over the integers is decided *and
solved* exactly by extended Euclid — less code than the satisfiability check
it replaces, and it subsumes all of them.

### What does *not* become harder

Kira's dependent fragment is **canonical by construction**: fold if closed,
symbolize otherwise; polynomials normalized, sorted, and interned to
id-equality (ch. 33). So unification never needs general definitional
equality or a conversion check — the thing that makes dependent-type
unification genuinely hard. Idris 2 must normalize terms while unifying;
Kira must not, because two equal value terms are already the same `type_id`.

That property is load-bearing and must be defended explicitly:

> **Every metavariable solution is substituted back through the `type_table`
> constructors, never by patching an entry in place.**

Otherwise canonical form breaks and id-equality stops being type-equality.
The invariant belongs in `zonk`'s implementation and in its tests.

### Totality

Pattern fragment: decidable, unitary. Linear Diophantine: decidable.
Nonlinear arithmetic is outside the fragment by ch. 33 and degrades to
`unknown`. Refinement obligations go to `reason.cpp`, which is already total
by construction (Fourier-Motzkin with integer tightening, bounded by
`k_atom_limit`). So the whole engine terminates with no search, no
backtracking, and no timeout — the same stance ch. 33 takes for the reasoning
solver, extended to inference.

### Obligations, one queue

Refinement narrowing is *not* unification and must not touch the
substitution. Neither is trait selection or method resolution. All three are
**obligations**: registered when a decision cannot yet be made, woken when a
variable they mention is solved, retried to fixpoint, and reported when the
queue stalls. Four kinds, one queue, one retry loop:

| Kind | Discharged by |
|---|---|
| Method call | candidate assembly over the existing impl lookup |
| Trait bound | the same, plus declared `where` clauses when abstract |
| Refinement narrowing | `reason.cpp`'s `solve(facts, goal)` (`reason.h:86`) — already written |
| Defaulting | declared last-resort candidates (below) |

The Idris 2 lesson is to keep this at *one* mechanism with several candidate
sources. If the rewrite produces `method_obligation`, `trait_obligation`, and
`operator_obligation` as separate machinery, it has rebuilt the present state
with better types.

### Blame is a separate pass

Reporting the first mismatch reports the *last* constraint, not the *wrong*
one. Constraints are retained as a graph, with provenance on each — span,
reason, both sides' descriptions, and a parent link forming an elaboration
context stack (Idris 2's "While processing..." is the cheap version of this,
and it was in its first release rather than retrofitted; Rust bolted
`ObligationCause` on afterwards and still pays for it).

On failure, a heuristic pass asks which constraint is the outlier rather than
which arrived last. This is what makes "you meant `xs.push(1)` to be a `str`
like the others" possible instead of "expected str, found int32". It is the
part most compilers skip, and it is the part that serves goal 1.

## Decisions this plan takes

**Defaulting is a declared last-resort candidate**, Idris 2 `%defaulthint`
style, never a rule inside the solver. An unconstrained integer literal picks
`int32` because a prelude declaration says it loses to every real constraint
— visible in source, explicable in a diagnostic, and without the edge cases a
built-in fallback generates.

**Generic bodies are checked once, abstractly**, with obligations discharged
from the declared `where` clauses. This deletes the template/instance double
check and all six `in_*_template_` gates rather than porting them. It also
settles what a `where` clause means: the complete set of facts a body may
rely on.

## Phases

Each phase is independently green. Phases 0-5 are new code with its own tests
and no integration risk; 6 is the dangerous one; 7-9 are long but mechanical.

### Phase 0 — the snapshot harness *(done)*

`semantic::render_snapshot` (`src/semantic/snapshot.h`) renders every map in
`checked_types` as deterministic text; `snapshot_test` checks it against
`src/testdata/inference_snapshot/session.snapshot`. Every phase through 7
must reproduce that golden **byte-identically**; phases 8-9 change it in
reviewable diffs. Regenerate with `KIRA_UPDATE_SNAPSHOTS=1 bazelisk run
//src/semantic:snapshot_test`, and read the diff — a regeneration nobody read
is the one way this harness is worse than nothing.

**The premise, measured.** Mangling one monomorphized instance name
differently (`holder::get$holder_int32_` → `get$holderXint32X`) changes what
every call site resolves to. With that change in the tree, **all 28
pre-existing test targets passed** and `snapshot_test` was the only failure:

```
043_impl_on_generic_target.kira:72:29 [call]
  golden:   ...::holder::get$holder_int32_ trait=get_it recv
  snapshot: ...::holder::get$holderXint32X trait=get_it recv
```

That is the exact class of regression the rewrite risks, and before this
phase nothing in the suite could see it.

Three decisions worth keeping:

**One session, not one per file.** A session's snapshot is dominated by the
stdlib's own ~24,000 decisions, so a golden per fixture would check the
stdlib in once per fixture (1.2 MB each). All 17 fixtures share one session
and one 1.3 MB golden; the marginal cost of adding a fixture is its own lines.

**Rendered by name, never by id.** Types print as their display spelling and
declarations as their (mangled) names, so the golden is stable across runs
and a diff says what changed rather than that *something* did.

**`checked_types::node_files`.** An `ast::node` carries a `source_span` with
no file, and every file in a session has a byte 0 — so a decision map alone
cannot say where its keys came from. `record_expr_type` now also records the
file it was standing in. That one funnel covers the great majority of keys in
the other maps too; a key without one renders `?:<offset>` and still sorts
stably.

**Coverage gaps, recorded rather than papered over.** Five maps are empty in
the current fixture set: `type_param_reflections`, `runtime_fill_dispatches`,
`comprehension_iterator_dispatches`, `try_conversions`/`try_conversion_types`,
and `functor_instances`. They are rendered (so they will populate the moment a
fixture exercises them), but they are *not* currently protected. Phase 8
touches `runtime_fill_dispatches` and phase 7 the comprehension path
directly — add a fixture for each before starting the phase that moves it.

### Phase 1 — `infer_ctxt`

New directory `src/semantic/infer/`, own tests, nothing calls it yet.

- Union-find with path compression over `type_id`, sorted metavariables
  (type / constructor-of-arity-k / value), occurs check across sorts.
- `zonk`, memoized, substituting **through `type_table` constructors** so
  canonicity holds. Its test asserts that a zonked type is id-equal to the
  same type written directly.
- `cause`: span, reason, both sides' descriptions, parent link.

### Phase 2 — the unifier

One `unify(a, b, cause)` covering every `type_kind` uniformly, with the three
rules above. The pattern-fragment rule includes the escape check, and outside
the fragment it postpones.

The existing 28 `unify_rigid` call sites enumerate the shapes it must handle;
that list *is* the test plan.

### Phase 3 — value solving

Extended Euclid over `linear_poly.h`'s canonical polynomials, replacing
satisfiability-only matching. Independently testable, and it closes ch. 33's
"does not solve for `n`" gap on its own.

Ch. 33's domain rules survive unchanged: `usize` and the unsigned family
constrain `v >= 0`, so `n + 1 ~ 0` is still rejected — the difference is that
`n + 1 ~ 3` now *answers* rather than shrugging.

### Phase 4 — obligations

Queue, per-variable wake index (so the fixpoint is not quadratic),
retry-to-fixpoint, stall detection, the four kinds above. Candidate assembly
stays pluggable so the existing impl lookup drops in unchanged, and
`reason.cpp` plugs in behind the refinement kind with no new prover.

### Phase 5 — blame and diagnostics

The graph, the outlier heuristics, the renderer. A golden corpus of *wrong*
programs asserting exact message text, written **before** the heuristics.

Acceptance bar: no message may be worse than what the same program produces
today. Specifically:

- `var xs = []` with nothing to infer from must read at least as well as the
  current hand-written diagnostic at `check.cpp:16121`.
- `head[T, n: usize](v: vec[T, n + 1])` called on a `vec[T, 0]` must still
  explain that `n = -1` is required and that `usize` forbids it — ch. 33's
  own bar ("rejected with the arithmetic explained").
- A failed trait obligation must name the goal, the facts in scope, and both
  ways out, per ch. 33's refinement rule.

"Type annotations needed" is not an acceptable message in this compiler, and
it is a constraint solver's natural output. Designing against it is the point
of this phase existing separately and early.

### Phase 6 — the elaboration split

Call sites record a *decision* keyed by AST node instead of a resolved
callee; the 11 instantiation sites move behind a flush that runs after the
fixpoint. No deferred types yet, so this is behavior-preserving by
construction and verified against phase 0.

This is the enabling move and the dangerous one: it touches every elaboration
site at once, and its only safety net is the phase 0 golden file. It is also
what would have prevented the defect recorded at
`src/testdata/codegen_stress/054_static_dispatch_on_generic_types.kira:18` —
a static call that type-checked while nothing was ever compiled for it.

### Phase 7 — constraint generation

Replace the 28 `unify_rigid` sites and 67 binding maps with the one unifier,
a function body at a time, golden file green throughout.

### Phase 8 — metavariables at the leaves

Empty `[]`, unannotated parameters, unsuffixed integer literals. Delete
`param_usage_inferrer` rather than extend it; delete the ad-hoc defaulting at
`check.cpp:7589`. **Todo item 20 closes here as a consequence, not as a
feature** — which is the whole argument for this document.

The acceptance test is the standard library itself: remove the annotations on
`partition`'s `yes`/`no` (`src/std/algo.kira`) and `from_iter`'s `out`
(`src/std/iter.kira`) and confirm the tree still builds.

### Phase 9 — abstract generic bodies

Delete the double check and the six `in_*_template_` gates.

## Definition of done

A ledger, not a feeling:

| Metric | Now | Done |
|---|---|---|
| `unify_rigid` call sites | 28 | 0 |
| Ad-hoc `string -> type_id` binding maps | 67 | 0 |
| `in_*_template_` gates | 6 | 0 |
| Distinct inference solvers | 4 | 1 |
| `param_usage_inferrer` | 385 lines | deleted |
| Value slots solved rather than checked | no | yes |

If a phase ends without moving one of those counts toward its target, it was
scope creep.

## Risks

**Phase 6 is the one that can go wrong quietly.** Mitigated only by phase 0
being genuinely exhaustive, which is why it comes first and why "exhaustive"
means every map in `checked_types`, not a sample.

**Blame heuristics can regress messages that are good today.** Mitigated by
phase 5's golden corpus being written against current output before any
heuristic exists, so a regression is a test failure rather than a judgement
call.

**Canonicity loss through in-place substitution** would break id-equality as
type-equality across the entire compiler, silently. Mitigated by the `zonk`
invariant above, asserted directly in phase 1's tests.

**Scope creep into the reasoning fragment.** Ch. 33's fragment is a hard
boundary — quantifier-free linear integer arithmetic over uninterpreted
atoms, no nonlinear arithmetic, no quantifiers, no aggregate contents.
This rewrite changes how obligations are *scheduled*, never what
`reason.cpp` can prove. Extending the fragment is a revision of ch. 33, not
a patch made in passing here.

## See also

- `spec/specification/03-advanced/33-dependent-and-refinement-types.md` — the
  value fragment, canonical value terms, the reasoning solver's exact scope.
- `spec/specification/03-advanced/37-higher-kinded-traits.md` — kinds as
  arities, and the pattern-matching restriction this plan renames.
- `spec/specification/02-intermediate/19-generics-and-inference.md` — what
  inference promises today.
- `spec/todo.md` item 20 — the gap that prompted this, and the one that
  closes as a side effect.
- `spec/list-migration-design.md` — the precedent for a staged migration with
  an acceptance test rather than a flag day.
