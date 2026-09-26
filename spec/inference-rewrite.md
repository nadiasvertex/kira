# Rewriting inference: one unifier, one queue, one blame pass

**Status:** Phases 0-7 done. Phase 8 closed todo item 20 for concrete code;
phase 9 has since made its stated acceptance test pass — the standard
library's `partition` and `from_iter` build their lists from an unannotated
`[]` — with byte-identical elaboration. Phase 8's other two leaves and phase
9's gate deletion both remain, and both wait on the same two things: one pass
over a generic body, and `method_call` as an obligation.
All 28 matcher sites run the one unifier, with no compatibility shims left
anywhere.

| Phase | What it makes possible | Status |
|---|---|---|
| 0 | A golden snapshot of every elaboration decision, so the rewrite is falsifiable | **Done** — `src/semantic/snapshot.{h,cpp}`, `snapshot_test.cpp`, golden at `src/testdata/inference_snapshot/session.snapshot` |
| 1 | `infer_ctxt`: one metavariable store over three sorts, with causes | **Done** — `src/semantic/infer/infer_ctxt.{h,cpp}`, `infer_ctxt_test.cpp` |
| 2 | One `unify`: rigid-rigid, pattern fragment, value slots | **Done** — `src/semantic/infer/unify.{h,cpp}`, `unify_test.cpp` |
| 3 | Value slots genuinely *solved*, not merely checked satisfiable | **Done** — `src/semantic/infer/value_solver.{h,cpp}`, `value_solver_test.cpp` |
| 4 | An obligation queue: methods, trait bounds, refinements, defaulting | **Done** — `src/semantic/infer/obligations.{h,cpp}`, `obligations_test.cpp` |
| 5 | Blame over a retained constraint graph, and the diagnostics it enables | **Done** — `src/semantic/infer/blame.{h,cpp}`, `blame_test.cpp`, golden corpus at `src/testdata/inference_diagnostics/` |
| 6 | Elaboration split out of checking — decisions recorded, flushed after solving | **Done** — `src/semantic/check.cpp` (`flush_pending_instances`), fixture `codegen_stress/089_elaboration_snapshot_gaps.cn` |
| 7 | Constraint generation migrated onto the one unifier | **Done** — `src/semantic/infer/rigid_match.{h,cpp}`, `rigid_match_test.cpp`; scoped `type_param`; all three allowances retired |
| 8 | Real metavariables at the leaves (`[]`, unannotated params, literals) | **Done** — empty `[]`, integer-literal defaulting, and unannotated parameters (implicit generics, phase 8b below) |
| 9 | Generic bodies checked once, abstractly | **Partly done** — the template/instance boundary is fixed and phase 8's acceptance test passes; the `in_*_template_` gates cannot come out until the second pass does (experiment recorded below) |

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
no backtracking and no ambiguity errors), and has a textbook algorithm. Cinder
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

 Cinder's dependent fragment is **canonical by construction**: fold if closed,
symbolize otherwise; polynomials normalized, sorted, and interned to
id-equality (ch. 33). So unification never needs general definitional
equality or a conversion check — the thing that makes dependent-type
unification genuinely hard. Idris 2 must normalize terms while unifying;
 Cinder must not, because two equal value terms are already the same `type_id`.

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
043_impl_on_generic_target.cn:72:29 [call]
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

### Phase 1 — `infer_ctxt` *(done)*

`src/semantic/infer/infer_ctxt.{h,cpp}`, tested by
`//src/semantic:infer_ctxt_test`. Compiled into `:semantic` but called from
nowhere, so it can be an algorithm under test before phase 6 puts it on the
critical path.

- Union-find with path compression over `type_id`; metavariables sorted
  type / constructor-of-arity-k / value; occurs check across sorts.
- `zonk`, memoized, substituting **through `type_table` constructors**.
- `cause`: `source_location`, reason, both sides' descriptions, parent link,
  in an arena whose index 0 is `k_no_cause` so lookup is total.

**The canonicity assertion.** `test_zonk_is_canonical` requires a zonked type
to be *id-equal* to the same type written directly — for a nested generic, for
a composite of `fn`/`ref`/`tuple`, and for the higher-kinded case. The last is
the one with teeth: zonking `F[A]` under `F := option` must collapse to the
ordinary interned `option[int32]`, not leave a `param_app` carrying a solved
head, or one type has two ids and id-equality stops being type-equality.

**Failability, verified.** Each of four mechanisms was broken in turn and the
assertion that should catch it did, alone:

| Broken | Failure reported |
|---|---|
| `param_app` no longer collapses a solved head | ``expected `F[A]` to zonk to the interned `option[int32]`` |
| occurs check disabled | `expected the occurs check to refuse the bind` |
| `zonk_memo_.clear()` removed from `bind` | ``expected `list[?a]` to zonk to the interned `list[int32]`` |
| sort check disabled | `expected a value not to solve a type var` |

Full suite after reverting: 30/30.

Three decisions worth keeping:

**Sorts are checked, arities are the kinds.** `check_sort` refuses a value
standing for a type, and a constructor of the wrong arity standing for `F[_]`
— which is where ch. 37's kind errors get folded in rather than duplicated.
`unknown` and `error` are accepted for every sort, because one gap in
knowledge must not cascade, and that rule outranks sorts.

**One genuinely ambiguous case, resolved explicitly.** An arity-0
`type_param_kind` is how the table spells *both* an ordinary `T` and a value
parameter `n`, so it is accepted for both `type_sort` and `value_sort` and
refused only for `ctor_sort`, where the arity settles it.

**Two kinds deliberately zonk to themselves.** `existential_kind` is nominal
— minted fresh per `some Trait` and never interned — so rebuilding one would
mint a second distinct type for the same declaration. `symbolic_value_kind`'s
polynomial is over *named* parameters rather than `type_id`s, so substituting
into it is value solving, which is phase 3.

Also unlike a per-kind switch: the occurs check walks `args`/`result`
generically, since a kind added later would otherwise silently stop being
checked and let an infinite type through.

### Phase 2 — the unifier *(done)*

`src/semantic/infer/unify.{h,cpp}`, tested by `//src/semantic:unify_test`.
One `unify(expected, found, cause)` covering every `type_kind`, with the three
rules above. Still called from nowhere.

**Zonk first, and the rules become exhaustive.** Each step zonks both sides
before dispatching, so a metavariable reaching a rule is necessarily
*unsolved* and an `F[A]` whose head is already solved has collapsed into the
application it denotes. Neither needs a case, which is what keeps one function
covering every kind.

**Two things the old matcher did not do**, and they are the phase's point:

- **An array's length is unified along with its element.** `unify_rigid`
  descended into `result` only for `array_kind`, skipping `args[0]` — which is
  exactly why an `n` in `array[T, n]` came back looking unsolved and needed
  `solve_value_params` standing beside it (`check.cpp:10062`).
- **An undecidable constraint is kept.** The old matcher's contract was
  "a failed match simply binds nothing", so a constraint it could not decide
  vanished. `unify` postpones it into `deferred_constraint`s that
  `retry_deferred` re-runs to fixpoint; phase 4's queue takes them over.

**Where ch. 37's letter is refined.** "Fails" covers two different situations
and they must not be conflated. A flex head against something that can never
be an application (`F[A] ~ int32`) is a real failure — `out_of_scope`. Two
flexible heads (`F[A] ~ G[B]`) are merely undecided, and postpone. So does a
*rigid* `F[A]` met inside a generic body, which stands for whatever the
instantiation supplies and can neither match nor refute anything yet.

**Errors carry both pairs.** `unify_error` records the outermost pair the
caller asked about *and* the innermost pair that actually clashed:
`fn(int32) -> list[str]` against `fn(int32) -> list[int32]` reports the
function types and `str`/`int32`. Reporting only the outer buries the fault;
only the inner loses the context. Phase 5 chooses which to lead with.

**Refinements strip; mutability does not.** A refinement is its base for every
shape question — comparing predicates here would make every narrowing site a
type mismatch and the solver would never get to prove anything. Mutability is
the opposite: `&mut T` and `&T` are different types, and whether one *coerces*
to the other is a call-site question, not unification's. The old matcher
crossed references freely in both directions; that allowance moves to
coercion in phase 7.

**Failability, verified.** Seven mechanisms broken in turn, each caught alone:

| Broken | Failure reported |
|---|---|
| `array_kind` stops unifying the length | `expected the length to solve` |
| mutability check removed | `expected mutability to matter` |
| `equation_satisfiable` ignored | `expected an unsolvable equation to refuse` |
| inner pair reported as the outer | `expected the inner pair to be where it actually went wrong` |
| flex head not bound | ``expected the head to solve to the `option` constructor`` |
| postponements dropped | `expected the undecided constraint to be kept` |
| `retry_deferred` always claims progress | `expected no progress while both heads are unsolved` |

Full suite after reverting: 31/31.

### Phase 3 — value solving *(done)*

`src/semantic/infer/value_solver.{h,cpp}`, tested by
`//src/semantic:value_solver_test`, and called from `unify_values`. Ch. 33's
"the compiler does not *solve for* `n` and propagate it" is now false:
`vec[T, n + 1] ~ vec[T, 3]` yields `n := 2`, and every type mentioning `n`
sees it.

Ch. 33's domain rules survive unchanged: `usize` and the unsigned family
constrain `v >= 0`, so `n + 1 ~ 0` is still rejected — the difference is that
`n + 1 ~ 3` now *answers* rather than shrugging.

**Names are keys into the one store, not a binding map beside it.** A
polynomial's unknowns are *named* rather than numbered, because both of
`linear_poly`'s users already had stable names for them. So a solved `n`
could easily have become a 68th ad-hoc `string -> type_id` map. Instead,
`infer_ctxt::value_param(name)` mints-or-returns the *value metavariable*
for that name: the name is a lookup key into the one union-find, and `n := 2`
is an ordinary `bind` that the occurs check, the sort check and `zonk` all
see. This is the test of whether the rewrite actually holds its line, and the
answer had to be one mechanism.

`zonk` now substitutes into a `symbolic_value_kind`'s polynomial and
re-interns through `type_table::symbolic_value`, which degrades a closed
polynomial to a `const_value` — so `n + 1` with `n := 2` zonks to the very id
that `3` has. Canonicity, again, by construction rather than by care. The one
cycle the occurs check cannot see is a value parameter solved in terms of
itself (the cycle is through polynomial *names*, not `type_id`s), so `zonk`
carries an in-progress set.

**How far it solves, and why it stops there.** Extended Euclid is used for the
exact refutation: an equation has an integer solution only if the gcd of its
coefficients divides its constant. That is a complete criterion and catches
`2n = 5` and `2m + 4n = 5` without enumerating anything. Solving proper stops
at one unknown, because the general two-unknown equation has a parametric
*family* rather than an answer, and ch. 33 already takes the position that a
site which does not pin a value down uniquely should leave it open rather
than guess — `m + n ~ 5` postpones.

**Failability, verified.** Four mechanisms broken in turn:

| Broken | Failure reported |
|---|---|
| gcd criterion removed | ``expected `2n = 5` to be refused`` |
| unsigned negativity ignored | ``expected `n + 1 = 0` to have no unsigned solution`` (and the unifier's own value test) |
| multi-unknown guesses the first | ``expected `m + n = 5` to determine neither unknown`` |
| polynomial substitution dropped from `zonk` | ``expected `n + 1` to zonk to the interned `3``` |

Full suite after reverting: 32/32.

### Phase 4 — obligations *(done)*

`src/semantic/infer/obligations.{h,cpp}`, tested by
`//src/semantic:obligations_test`. Queue, per-variable wake index,
retry-to-fixpoint, stall detection, the four kinds above. Candidate assembly
is a `std::function` per kind, so the existing impl lookup drops in unchanged
and `reason.cpp` plugs in behind the refinement kind with no new prover; the
queue interprets neither, and carries an opaque `payload` for whatever table
the caller assembles candidates from.

**The wake signal.** `infer_ctxt::take_newly_solved()` returns the variables
bound since the last call, and the queue wakes only their watchers — so a
pass costs what actually changed rather than a sweep over every pending
decision. A *merge* counts as news for the same reason a solution does: an
obligation watching the variable that was absorbed must be re-pointed at the
one that absorbed it, so every attempt that leaves an obligation waiting
re-indexes its watches by their current representative. A wake index that
silently stops waking is worse than none, since the symptom is a stall with
no explanation.

**The unifier is inside the same fixpoint.** A deferred `F[A] ~ G[B]` and a
waiting `T: ord` can unblock each other, so `run_fixpoint` interleaves
`retry_deferred` with obligation attempts rather than draining them in
sequence. Two loops would make the answer depend on which drained first —
the order-dependent inference this rewrite exists to remove.

**Defaulting is structurally last, not politely last.** It is excluded from
`run_fixpoint` entirely and attempted only by `flush`, one candidate at a
time, each followed by another full fixpoint.

**A test that could not fail, and the bug it was hiding.** The first version
of `test_defaulting_is_last_resort` had each resolver check
`solution(...)` before binding — so the *resolver* was enforcing last-resort
and the test passed however the queue ordered them. Deliberately breaking
`flush` to run defaults first did not fail it. Rewritten so both resolvers
bind unconditionally and ordering alone decides the answer, it failed
immediately **on the unbroken code**: `run_fixpoint` was attempting defaulting
obligations along with everything else, and a default could beat a constraint
that was one pass away. That is the real fix in this phase, and nothing else
in the suite would have found it.

**Failability, verified.** Five mechanisms broken in turn:

| Broken | Failure reported |
|---|---|
| wake index ignored (every obligation retried every pass) | ``expected an unrelated solution not to wake it`` |
| watches not re-indexed after a merge | ``expected the merged-away watch to still wake it`` |
| defaulting attempted in the main fixpoint | ``expected the real constraint to decide, not the default`` |
| `retry_deferred` not driven by the fixpoint | ``expected the postponed constraint to have been retried and resolved`` |
| a resolver's failure swallowed | ``expected the failure to stop the flush`` |

Full suite after reverting: 33/33.

### Phase 5 — blame and diagnostics *(done)*

`src/semantic/infer/blame.{h,cpp}`, tested by `//src/semantic:blame_test`;
the golden corpus in `src/testdata/inference_diagnostics/`, exercised by
`//src:inference_diagnostics_test`.

**The corpus came first, and that is the point.** The acceptance bar is "no
message may be worse than what the same program produces today", and a bar
like that can only be enforced if *today* was written down before anything
started changing it. Nine wrong programs, each with its rendered diagnostics
captured byte for byte. A corpus captured after the heuristics would record
whatever the new code happens to say, which is not a bar at all.

Three entries are marked `bar: BELOW BAR TODAY` in their own leading
comments. Their goldens are a floor, not a target, and phase 5 is not
finished on them until the wiring in phase 7 lets the new message replace the
old one:

| Case | What is wrong with today's message |
|---|---|
| `003_unmet_trait_bound` | An unsatisfied `T: ordering` surfaces as a missing *method* inside the generic body. It names neither the goal (`point: ordering`) nor the bound that required it. |
| `007_array_length_cascade` | One mistake, two errors: the literal's length, then the same disagreement again as a type mismatch. |
| `009_ref_mutability` | `&point` versus `&mut point` is printed but never explained, with no `help:` line at all. |

Two entries already meet their bar and must not regress:
`001_empty_list_literal` keeps the hand-written text that avoids pointing
into `src/std/list.cn`, and `002_dependent_length_unsatisfiable` already
explains that `n + 1` can never equal `0` for `n: usize` — ch. 33's own
standard.

**The retained constraint graph.** `constraint_graph::record` keeps every
constraint *including the ones that succeeded*, because a constraint that
succeeded is precisely the evidence that makes a later one an outlier.
`demands_on` asks what each constraint required of one variable, resolving
through `find` rather than by id — a constraint recorded against a variable
that was later merged still counts, or the evidence silently shrinks after a
merge and the majority can flip.

**The outlier rule.** When four branches say `int32` and one says `str`, the
`str` one is the mistake and the other four are why: the caret goes on the
minority and the agreeing sites are shown as the reason it is one. Pointing
at whichever constraint came last is a coin flip that reads as
authoritative.

**An even split blames neither**, deliberately. One against one genuinely
does not say which side is wrong, so `outliers` returns nothing and the
message shows both sides and admits it will not guess. A heuristic that
invents a winner from a tie is worse than no heuristic: the reader has no way
to tell that is what happened.

**`explain_stall` exists so that "type annotations needed" never has to be
printed.** It names the variable as the source wrote it, where it came from,
what is already known about it, and which decision is blocked on it. A stall
with facts in scope and a stall with none are different mistakes and read
differently — collapsing them is exactly how a compiler ends up telling
someone who wrote five constraints to add an annotation.

`blame.h` deliberately does not include `diagnostic.h`. It produces the
*content* of a message — where to point, what to say, what else to show — and
the checker renders it, which keeps the heuristics testable as functions of a
constraint graph rather than of a whole compiler session.

**Failability, verified.** Seven mechanisms broken in turn:

| Broken | Failure reported |
|---|---|
| `outliers` returns the majority | ``expected exactly one outlier`` |
| tie detection removed | ``expected a tie to produce no outlier`` |
| `demands_on` compares ids instead of `find` | ``expected constraints on both halves of the merged class`` |
| a reflexive `var ~ var` counted as a demand | ``expected the reflexive constraint to be ignored`` |
| the cause's own wording ignored | ``unexpected headline: expected `int32`, found `str``` |
| stall help identical with and without facts | ``expected a stall with facts to read differently from one without`` |
| a report fabricated where everything agrees | ``expected no report where there is no disagreement`` |

And three on the corpus harness itself: a perturbed golden (reported as a
diff), a deleted golden (refused rather than skipped), and a corpus program
edited until it compiles (``expected the program to be rejected``) — that
last one being the shape a golden test fails silently in.

Full suite after reverting: 35/35.

### Phase 6 — the elaboration split *(done)*

`find_or_check_generic_instance` is split in half in `src/semantic/check.cpp`.
The clone is still made at the site and the pointer still returned there, so
every caller keeps getting a real declaration to record in its dispatch map
exactly as before. What moved is the *checking* of the cloned body, which now
happens in `flush_pending_instances` after the walk.

Checking an instance body in the middle of the expression that asked for it is
what forces every type to be final the instant it is first observed — the
structural reason the checker grew 28 `unify_rigid` sites and four solvers.
Breaking that is the enabling move for phases 7-9.

**Verified behavior-preserving by the strongest evidence available**: the
phase 0 golden is byte-identical before and after, across 28,436 lines and
~28,000 recorded decisions.

**A pending instance carries its own context.** By flush time the walk has
moved on and nothing the deferred half reads is still standing, so module,
file, parameter slots, `self`, the enclosing type-parameter scopes, the
instantiation chain and the depth are all captured at request time. Two of
those changed shape rather than merely moving:

- **Depth** was the C++ call stack's own depth, because bodies were checked by
  recursive calls. It is now a field each pending instance carries, or
  `climb[n]` requesting `climb[n + 1]` never bottoms out — verified: zeroing
  it does not fail a test, it *hangs the compiler*.
- **The instantiation chain** was read off a live stack. A deferred body has
  no such stack, so the whole chain is captured. "instantiated from here, as
  `biggest$point`" is often the only line in a monomorphization diagnostic
  that points at code the user wrote.

**One thing cannot wait: a compile-time-only function.** Its body is not code
to compile later, it is the answer to a call being checked right now, and the
fold that consumes it happens in that very expression. Those are still checked
eagerly. Deferring them leaves the call unfolded and it reaches the backends as
a call to a function no module compiles — which is how `is_signed_integer$int32`
turned up missing the first time this split was tried.

**Drop resolution needed its own flush.** `resolve_drop_plans` runs after
`run_impl` has finished, so it is the one requester with no walk left behind
it. Without a flush there, every `list[T]::drop` and the `mem::free[T]` each
one calls was cloned, named, and never checked — a function the backends are
told to compile and nothing ever compiled. That is precisely the defect phase
0 existed to catch, and it caught it.

#### The net was extended first

Phase 0 recorded which `checked_types` maps its fixtures left empty. Four of
them are written by the sites this phase moves, so
`src/testdata/codegen_stress/089_elaboration_snapshot_gaps.cn` was added
before any code moved, covering `try_conversions`/`try_conversion_types`,
`ord_dispatch_result_types`, `runtime_fill_dispatches`,
`comprehension_iterator_dispatches` and `resolved_fn_values`. Moving code whose
only safety net has zero coverage over it was the whole risk.

Writing that fixture found a real bug, unrelated to the rewrite and older than
it: **`[v; n]` with a non-`usize` runtime count was accepted and then
miscompiled.** The counting loop it lowers to compares a real `usize` index
against the count, so an `int32` count produced an `icmp` over an `i64` and an
`i32` and tripped an assertion inside LLVM itself — while the bytecode VM, whose
values are all tagged 64-bit words, ran the same program and gave the right
answer. Cross-tier agreement could not have found it; only one tier was ever
wrong. The checker now requires the count to be a `usize`, phrased to match the
diagnostic a non-`usize` *subscript* already got, since a repeat count and a
subscript are the same requirement wearing different syntax
(`src/testdata/inference_diagnostics/010_fill_count_not_usize.cn`).

#### Failability, verified

| Broken | What caught it |
|---|---|
| drop-plan flush removed | snapshot (16 instances missing) **and** `std_test` |
| comptime-only instances deferred like the rest | `std_test` — `is_signed_integer$int32` unresolvable |
| worklist drains a snapshot instead of to fixpoint | all four: snapshot, `std_test`, `codegen_stress_test`, the diagnostics corpus |
| instantiation chain not carried | the diagnostics corpus **alone** — nothing else in the suite sees a `note:` |
| depth not carried | the compiler does not terminate (corpus case 011) |

One field is **captured but not distinguished by any test**:
`enclosing_type_params`. It is non-empty at request time on the order of fifty
times per fixture, yet zeroing it changes nothing the suite can see. It is kept
because the eager code checked instance bodies with those scopes live, and
reproducing that exactly is what "behavior-preserving" means here — but the
honest status is that no test tells the two apart, and this note is the record
of that.

Full suite: 35/35.

### Phase 7 — constraint generation *(done)*

`checker::unify_rigid` no longer contains a matcher. It calls
`infer::match_pattern` (`src/semantic/infer/rigid_match.{h,cpp}`, tested by
`//src/semantic:rigid_match_test`), which hands the pattern and the concrete
type to the phase-2 `unifier` and translates at the edges: the declared
parameters are adopted as metavariables of a *local* store
(`infer_ctxt::adopt`) and the solutions are read back under the names all 67
binding maps expect. All 28 call sites keep their interface and run the one
unifier.

**Zero recorded decisions changed.** Across the phase 0 golden, `node_types`,
`resolved_callees`, every dispatch map, `const_generic_instances` and
`synthesized_decls` are identical before and after. Only `drop_plans` (19 →
52) and `view_bearing_types` (63 → 137) move, and both only grow, for the
reason below.

#### Type parameters are now scoped

`type_table::type_param` interned by *name*, with no scope, so the `T` of a
callee's signature and the `T` of the caller being checked were the **same
`type_id`**. That is not untidiness, it is a soundness hole: matching
`list[T]` against `list[slice[T]]` asks for `T := slice[T]`, an infinite type
that any matcher with an occurs check must refuse — and the old walk had no
occurs check, so it recorded it and `std.algo::windows` depended on the
answer.

`type_param` now takes the `ast::type_param` that declared it and keys on it,
exactly as `ctor_ref` has always keyed on its declaration. Ten call sites, all
of them declaration sites with the node already in hand. Parameters with no
declaration node to name them (`self` inside a trait, an associated type, a
concept parameter) keep the by-name identity, because there is nothing else to
key them on.

The two growing sections are the whole visible cost: same-named parameters
from different declarations are now different types, so an abstract `list[T]`
appears once per declaration in the two walks that enumerate *every* interned
type. No instantiated type is affected, and `array[slice[T], n]` — the entry
whose loss is what proved the capture was real — is back.

#### All three allowances are retired

The swap landed behind a `legacy_compat` struct reproducing each of
`unify_rigid`'s defects, so that one step changed one thing. Each then came
off on its own:

| Retired | Effect on the golden |
|---|---|
| `ignore_array_length` — an array's length now solves from an argument (todo 20, in the dependent fragment) | none beyond the interned-type count |
| `ignore_mutability` — `&T` no longer matches `&mut T` | none; it *removed* interning noise |
| `ref_coercion` — replaced by the rule it stood in for | none at all |

The third was not a defect. It was a real rule of the language wearing a
matcher's blindness: a call site **borrows** when a `&T` parameter is given a
value, and **dereferences** when a by-value parameter is given a reference.
`nums.iter()` depends on the first — `iter`'s receiver is declared `&list[T]`
and `nums` is a `list[int32]` — and without it `T` never solves.

That rule is now written out as `infer::coerce_at_call_site` and applied at
the **outermost position only**, which is the whole difference between a
coercion and a blind spot: `list[&T]` against `list[int32]` is a real
disagreement and stays one, where the old allowance fired at every depth and
could not tell the two apart. `unify.cpp` carries no migration shim.

Two things had to be got right, and the suite refused both wrong versions:

- **It cannot be a rewrite of the inputs.** Erasing references everywhere
  binds `T := int32` where the rule binds `T := &int32`. That was the first
  attempt, and `std_test`'s `is_view_through_nested_call` caught it.
- **A type parameter is not a by-value parameter.** It is willing to *be* a
  reference, so dereferencing on its behalf discards what the call site said.
  `describe_view[T](x: T)` given `&n` must infer `T = &int32`, and
  `std.traits.is_view[T]()` inside it reads exactly that. The second attempt
  missed this, and the same test caught it again.

#### The matcher interns nothing

Not a micro-optimization. Every type built through the table's constructors is
permanently in the session's one table, and both `resolve_drop_plans` and
`compute_view_bearing_types` walk every interned type. An earlier version that
rewrote the pattern left scratch `list[?a]` behind, which reached drop
resolution and produced an instance named `list::drop$list___` — a
monomorphization of a type that existed only because the matcher had been
there. Adopting the parameter ids in place is what avoids that, and it is why
`infer_ctxt::adopt` exists.

#### Smaller results that stand on their own

- **`unify::step` checks absence before binding.** Solving `?a := unknown`
  buys nothing and costs the variable: it is answered, and no later constraint
  can teach it anything.
- **`meta_var::sort_is_ambiguous`.** An arity-0 `type_param_kind` is how both
  an ordinary `T` and a value parameter `n` are spelled, so a variable adopted
  from one accepts a solution of either sort — the mirror of the latitude
  `check_sort` already gave a *value* spelled that way. Without it every const
  generic is rejected while ordinary generics stay green.

#### Failability, verified

| Broken | What caught it |
|---|---|
| `type_param` scoping ignored (back to by-name) | the snapshot — `array[slice[T], 0]` disappears from `node_types` and `view_bearing_types` drops to 61 |
| the implicit borrow removed | `std_test`, `move_check_test`, and the unit tests |
| a type parameter dereferenced anyway | `std_test` — `is_view_through_nested_call` — and the unit tests |
| the coercion applied at every depth instead of the outermost | `std_test`, `move_check_test`, and the unit tests |
| `ignore_array_length` made a no-op | ``expected compat to reproduce the old behavior of leaving `n` open`` (before retirement) |
| `ignore_mutability` made a no-op | ``expected compat to reproduce the old permissiveness`` (before retirement) |
| absence no longer outranks binding | ``expected the unpinned one to be absent, not bound to `unknown``` |
| sort ambiguity not recorded | ``expected the length to solve — the gap this replaces`` |

Full suite: 36/36.

### Phase 8 — metavariables at the leaves *(the empty literal is done; the other two are blocked)*

**`spec/todo.md` item 20 is closed for concrete code**, which is the result
this whole document was written to get. An empty `[]` with nothing to read a
type from is now pinned by a later use:

```kira
def collect_evens(n: int32) -> list[int32]:
    var out = []            # list[?a]
    ...
        out.push(i)         # ?a := int32
    return out
```

Both tiers agree on the answer, asserted as a computed value rather than as a
clean compile
(`src/testdata/codegen_stress/090_empty_literal_inferred_from_use.cn`).

It closed **as a consequence, not as a feature**. Nothing here re-solves
anything: the literal mints one leaf unknown, and the solving happens in the
places a value already met a declared type.

#### The three pieces

- **One session-level store.** `checker::leaf_ctxt_` plus the one unifier. A
  leaf minted in one function can be pinned by a constraint in another, so a
  per-body store would need a protocol to say so.
- **`type_mismatch` is where leaves learn.** It is already the single place a
  value meets a declared type — argument, initializer, assignment, field,
  element, return — which is why phase 8 needs no second walk and why a
  `push` a hundred lines away can pin the literal. Several callers ask
  `compatible` themselves and only reach `type_mismatch` when the answer is
  no; `list[?a]` is compatible with everything, so those paths get `agrees`
  instead. `return out` in `std.iter::from_iter` is exactly one of them.
- **The wiring is deferred.** `flush_leaf_literals` runs the literal's
  `from_array` wiring per file, once the element is known and before phase
  6's instance flush, which wiring requests instances of its own. Doing it at
  the literal would name `list[?]::from_array` — an instance of a type no
  value has.

#### Three things that had to be got right

- **Re-name the instance after the arguments pin the receiver.** `out.push(i)`
  is instantiated from the receiver's type, but it is *checking the arguments*
  that solves the receiver. Naming the instance first produces
  `list::push$list___`, a function the backends are told to compile and
  nothing ever compiles — phase 6's defect, arriving from the other
  direction.
- **Whole-table walks must skip types carrying an inference variable.** A
  `list[?a]` stays interned after `?a` is solved, and `resolve_drop_plans`
  enumerates every interned type. Without the guard it asks for the drop plan
  of a type no value ever has and mints `list::drop$list___`.
- **The solver must only run when a leaf is involved.** Running the unifier on
  every pair also solves *value* parameters, and those are keyed by name in
  the store — so `at[n: usize]` called at `n = 3` and then at `n = 5` measured
  the second call against the first's answer. `check_test` caught it.

#### The diagnostic changed, and had to

`src/testdata/inference_diagnostics/001_empty_list_literal` was phase 5's
acceptance bar, and its text said "the element type is not inferred from a
later `push`". That is now false. The message names the uses that *would*
have pinned it and says none of them happen in this function — the difference
between a rule and a fact about the code in front of the reader. The corpus
caught the change and required it to be read before it was accepted, which is
the whole reason it was written first.

#### What is not done

The plan's stated acceptance test — removing the annotations on `partition`'s
`yes`/`no` and `from_iter`'s `out` — **does not pass yet**, and the reason is
phase 9's, not a gap in this mechanism. Those bodies are *generic templates*:
the leaf's solution there is a type *parameter*, not a type, and the template
is checked abstractly before any instance exists. Removing the annotations
type-checks and then fails in lowering, because the instance the abstract pass
named was never the one compiled. Phase 9 — generic bodies checked once,
abstractly — is where that is addressed.

#### The other two leaves are blocked, and by the same thing

Unannotated parameters and integer-literal defaulting were meant to follow
the empty literal through the same door. They do not, and the reason is worth
recording because it reorders what is left rather than adding to it.

**Integer-literal defaulting was built and reverted.** The change is small and
reads correctly: `infer_literal` mints a leaf instead of answering `int32`,
raises a `defaulting` obligation carrying `int32` as its candidate, and
`flush_leaf_literals` runs `obligation_queue::flush` — which applies defaults
only once the fixpoint has stalled, one at a time, each followed by another
full fixpoint. The range check moves with the type, since a range is a fact
about a type and `let x = 300` is only too large once something has said
`int8`. Fourteen of thirty-six targets failed, and every failure was one
sentence:

```
error: `alloc`'s compile-time argument `T` is not a type this call can name
  note: instantiated from here, as `list::from_array$5$list___`
      3 |     let numbers = [1, 2, 3, 4, 5]
```

An array literal's elements are inferred against no expectation, so each is a
leaf, so the literal's element type is a leaf, so `list::from_array` is
instantiated on a `T` nothing has solved — `list___`, phase 6's defect
arriving for the third time. Deferring the wiring the way the empty literal
defers it handles that one. It does not handle the next one:

```
error: cannot tell what `T` is in this call to `iter`
   10 |     let nv = nums.iter().values()
```

A method call on a `list[?a]` receiver cannot wait, because method dispatch is
not an obligation yet — it is a lookup performed during the walk, which must
have a concrete receiver to perform. **That is the blocker, and it is the
`method_call` obligation kind: built in `infer/obligations.h`, never wired.**

**Unannotated parameters are blocked on the adjacent half of the same thing.**
`param_usage_inferrer` (`check.cpp`, ~540 lines) is a second inference engine
with its own union-find, its own environment, its own AST walk, and its own
deliberately-partial type resolver — the clearest instance in the compiler of
the duplication this document exists to end. Replacing it with a leaf is one
line of minting. What is not one line is *when the leaf is solved*:
`param_types_for` is asked for a callee's parameter types at the call site,
which may be checked before the callee's body, and the private walk exists
precisely so an answer is available on demand. A leaf solved only by the body
check makes the answer depend on the order files and functions are walked in
— the same order-dependence that phase 8's value-parameter bug already
demonstrated is not survivable. The regular answer is to check the body on
demand, quietly, and read the leaf afterwards; the checker has no quiet mode
for a body (only `resolve_ctx{.quiet = true}` for types), so that is a piece
of work, not a call.

So both remaining leaves want the same thing, and it is not more leaves: the
walk still *decides* eagerly — dispatch, instantiation, parameter types — and
any leaf that survives to a consumer meets one of those decisions. Phase 9
(generic bodies checked once, abstractly) and wiring `method_call` through the
obligation queue are what unblock them. The two leaves should be reattempted
after, not before.

#### Failability, verified

| Broken | What caught it |
|---|---|
| `type_mismatch` no longer solves leaves | `codegen_stress_test` |
| the instance not re-named after the arguments pin it | `codegen_stress_test` |
| whole-table walks see scratch types again | `codegen_stress_test` **and** the diagnostics corpus |
| leaf literals never flushed | `codegen_stress_test` **and** the diagnostics corpus |

Full suite: 36/36.

### Phase 8b — unannotated parameters as implicit generics *(done)*

An unannotated parameter is a leaf (`param_types_for`), and its own body says
what it is — never a caller (`04-functions.md`). Once the body has been
checked the function is one of two things, decided **once, before the
defaulting flush** (`classify_param_decls`): a literal in the body (`x * 2`)
must not default `x` to `int32` and turn a function generic over every number
into an `int32` one.

- **Pinned** (`wide(x)`, or a return annotation): an ordinary function. Each
  call's argument is unified with the pinned type
  (`resolve_open_param_calls(false)`, before any literal defaults, so
  `narrow(3)` makes the `3` an `int64`).
- **Open**: an implicit type parameter. Each call is monomorphized for its
  own argument types — a clone of the declaration whose `param_types_for` cache
  is *seeded* with those types, so no synthesized AST or hidden type
  parameter is needed (`instantiate_open_param_call`, instance `probe$int64`).
  Lowering skips the template (`checked_types::open_param_templates`).

**The callee's body is not checked on demand.** The call is recorded
(`pending_open_param_call`) and decided after the walk, when the callee has
been checked wherever it lives. That is what makes the answer independent of
file and function order without a quiet mode for bodies.

**The probe.** A function with an unannotated parameter is checked once
against the leaf to find out which case it is. What a body says about a
still-open leaf is not a mistake (`bits(x)` on an open `x` is a call waiting
for its instance), so that check's diagnostics are *held* and replayed only
if the function turned out pinned; the deferred literal wiring and method
calls it queued are dropped if it turned out open. Each instance is checked
against concrete types and reports for real.

Tests: `codegen_stress/096_unannotated_param_is_a_leaf.cn`
(`# expect: 12800000017`), `check_test`'s
`test_pinned_param_rejects_wrong_argument`, `lower_test`'s two unannotated
cases. Verified failing without: the instance-name suffix, the literal skip,
and the diagnostic hold.

**Not done, deliberately:**
- Free-function calls, including UFCS (`x.probe()`, whose receiver is the
  first argument), are elaborated. A *method* in an `impl`/`extend` block
  with an unannotated parameter still resolves to the template.
- An unannotated *return* type is inferred (below) only for free functions and
  implicit-generic instances; methods and explicit generics still have none.
- A generic template that is never called is not checked for errors, as with
  explicit generics, since its first check's diagnostics are discarded.
- No trait bounds are inferred; an instance is simply checked per type.
  "Widest type the body permits" as a declared `where` clause waits for
  phase 9's single abstract check.

**Unannotated return types.** The checker takes the join of every
`return <value>` and the tail (`note_inferred_return`; a `never` path says
nothing) and stores it in `checked_types::inferred_return_types`, which lowering
reads where it would read the annotation. A callee declared after its caller
is checked on demand from `signature_return_type`, in its own module and file,
and skipped when its turn comes; a call from inside its own body gets a leaf
that the body's returns pin. A result still hanging on one of the function's
own parameters is left open for each instance to answer, and a leaf open at
the end (a bare literal) is defaulted then, since the body compiles once.
A call to an open callee answers with a per-call leaf, solved once the
instance's body has been checked (`finish_open_results`).

Tests: `codegen_stress/097_unannotated_return_is_inferred.cn`
(`# expect: 11000000332`), `check_test`'s
`test_inferred_return_type_is_held_against_the_caller`, `lower_test`'s two
return-type cases. Verified failing without: the on-demand check, the
recursive-call solve, and the inferred-return lookup.

Known hazard: the end-of-body default runs the leaf queue, which can default
a caller's pending literal earlier than a later constraint would have pinned
it.

### Phase 9 — abstract generic bodies *(the boundary is fixed; the gates are not)*

**Phase 8's acceptance test passes.** The annotations are off `partition`'s
`yes`/`no` (`src/std/algo.cn`) and `from_iter`'s `out`
(`src/std/iter.cn`), and the whole suite is green:

```kira
pub def partition[I, T](it: I, pred: fn(T) -> bool) -> (list[T], list[T]) where I: iterator[T]:
    var yes = []
    var no = []
```

The snapshot is the evidence, and it is stronger than "it compiles": every
recorded type at those lines is **byte-identical to the annotated version**
— `list[T]`, and `from_array from array[T, 0]` — with the only differences
being the column numbers the removed annotations moved, and ten more interned
types. Removing an annotation changed no elaboration decision anywhere.

#### It was never the leaf. It was the phase boundary.

Phase 8 assumed the stdlib case failed because a leaf's solution there is a
type *parameter*. It is not. A generic body is checked twice — abstractly as
the template, then once per instance — and the template's `[]` **defers** its
`from_array` wiring to a flush that runs after the walk is over. By then
`in_type_generic_template_` reads false, so the deferred wiring instantiated
`list::new` for an abstract `T` and named `list::new$list___`: a function the
backends are told to compile and nothing ever compiles. Phase 6's defect, for
the third time, and from a third direction.

Three fixes, all of them the same shape.

- **Deferred work carries the context it was deferred from.**
  `pending_leaf_literal` now records the module and both template flags and
  restores them around the wiring, exactly as `pending_instance` carries its
  site chain and its depth. A deferral that does not carry its context does
  something different from what it would have done in place, which is not a
  deferral.
- **The two queues are one fixpoint.** Wiring a leaf requests instances;
  checking an instance walks a *cloned* body whose `[]` is a different AST
  node and mints a leaf of its own. `flush_deferred` drains both until
  neither has anything left, for the same reason `obligation_queue`
  interleaves resolvers and postponed constraints.
- **Solutions are substituted at the boundary.** A leaf is recorded at the
  node that minted it, before anything has said what it is, so the recorded
  type stayed the *variable* however long ago the answer arrived.
  `take_checked_types` now zonks `node_types` alongside erasing refinements —
  same place, same reason: nothing downstream should have to know an
  inference variable ever existed. Without it the template recorded
  `list[_]` and handed a type containing a metavariable to lowering.

`resolve_drop_plans` and its flush also moved *above* those sweeps, since
checking those bodies is itself a walk that records types and can mint leaves
of its own. That dropped two abstract `list[T]` drop plans (52 → 50) — plans
for a type no value ever has — and added two `*mut slice[T]` view-bearing
types (137 → 139), the conservative direction for the borrow checker.

#### The gates are not deletable yet, and the experiment says why

The plan's line for this phase was "delete the double check and the six
`in_*_template_` gates". The natural first move is to turn each gate from a
*mode* ("which pass am I in") into a *fact* ("is this type concrete"), since
a mode is precisely what deferred work loses — that is the bug above. So
`instantiate_from_array_for`'s gate was tried as `mentions_type_param(target)`
instead of the two flags.

**Refuted, and informatively.** Thirty-five targets stayed green and the
snapshot moved 537 lines: the fact-gate instantiates from *inside* template
bodies whenever the target happens to be concrete — extra `list[usize]` and
`list[(usize, usize)]` instances the mode-gate suppressed, two of them
recording `list[_]`. The gate is not an approximation of a property of the
type. It means what it says: the template pass must instantiate *nothing*,
concrete things included, because the instance pass will do it under the
right names.

So the distinction the gates draw **is** the double check. They cannot be
deleted one at a time as a cleanup; they come out when the second pass does,
and that is the remaining work of this phase:

- Checking a generic body once and obtaining each instance's types by
  substitution rather than by re-walking the clone.
- Wiring `method_call` through the obligation queue, so a call on a
  not-yet-concrete receiver waits instead of being suppressed.

Those two together are also what unblocks phase 8's other two leaves.

#### Integer-literal defaulting, attempted again — and what it taught

Parked on `wip/literal-defaulting-demand-points`, six of thirty-six targets
red. Worth reading before it is picked up again, because the mechanism is
settled and only the list of sites is unfinished.

The literal mints a leaf and raises a `defaulting` obligation carrying
`int32`, rather than answering `int32` at the leaf from no evidence. The
candidate is spent by **`demand(id)`**: zonk, and only if still open run the
queue, whose `flush` attempts defaults one at a time once everything else has
stalled. Where `demand` is called is the entire design — not at the leaf,
which is what the old inline answer did and why it was wrong so often, and at
a call only *after* the arguments are checked, since checking them is what
usually supplies the answer. Wired so far: a call's generic-parameter solve,
method resolution, arithmetic operand selection, indexing. Still to wire:
higher-kinded static/trait dispatch (`option.pure(20)`, `.map(f)`), comptime
splice operands, and two lowering paths that still see an abstract parameter.

That this is a list of sites rather than one rule is not a defect in the
approach. A consumer that must choose an impl, an instruction, or an
instance genuinely needs a type; Rust forces its own integer fallback at the
same kind of boundary.

Two rules fell out, and both are fixes in their own right. **The first is
landed on master**; the second is meaningless without a defaulting resolver
and stays on the branch.

**Landed: an open leaf is not an answer.** Three pieces, each independently
covered:

- A call's argument solve reads the *settled* type. Reading the raw recorded
  one matched `list[T]` against `list[?a]` for a `?a` a `push` had already
  pinned — a stale read, nothing more.
- An argument still carrying an open leaf after that answers *nothing*.
  Binding anyway records the parameter as solved **to a variable**; the
  binding map cannot tell that apart from a real answer, so the later
  argument that could have said something is never consulted and the call
  reports `T` unsolved having discarded the one thing that would have solved
  it. `sum_from(out, 0)` is exactly that shape, and the parameter order was
  the whole difference — `sum_from(0, out)` worked.
- `solve_leaves` refuses a declared type mentioning a type parameter not in
  scope. Without it the leaf is bound to the *callee's* `T`, which
  type-checks and then asks lowering for `list::from_array$0$list_T_`, an
  instance of a type no value has.

`codegen_stress/092_open_leaf_is_not_an_answer.cn` (`# expect: 46`) holds
the positive case; `inference_diagnostics/013_open_leaf_at_a_generic_call`
holds the refusal, where declining to answer produces a message on the
reader's own `var out = []` instead of a capture that reaches the backend.
That case also forced a wording fix: the old help asserted "Nothing in this
function does any of those", which is false when the literal *is* passed as
an argument — it now says "an argument whose type is already known", which is
the clause that makes the sentence true.

Both rules as originally stated:

- **A leaf must not be bound to a type mentioning a type parameter that is
  not in scope.** That parameter is the *callee's pattern*, not a declared
  type, and binding a leaf to it captures it: `pick(1, 2)` answered "the
  literal's type is `T`" and then "`T` is unsolved" — the checker blaming
  the call for a circle it drew itself. Scope is the whole distinction, and
  it is a fact rather than a mode: inside `from_iter`'s own body `T` is rigid
  and `return out` against `-> list[T]` is exactly how that leaf is meant to
  solve. This alone removed fifteen of twenty-three failures.
- **A default that will not bind has lost, not failed.** It is a last
  resort, so "does not apply here" is a discharge. Reported as a failure it
  aborted the whole flush, and one leaf's refusal left every later literal
  open.

#### Failability, verified

| Broken | What caught it |
|---|---|
| deferred wiring does not restore the template flags | `std_test` — `alloc[T]` "no concrete checked type", and `snapshot_test` |
| solutions not substituted at the boundary | `snapshot_test` — `list[_]` for `list[T]`, at **identical byte size**, which a size-only comparison would have missed entirely |
| `091`'s expected value | `codegen_stress_test` — "returned 116, but `# expect:` says 115" |

One invariant is *not* covered: `flush_deferred`'s loop. At the corpus's
current nesting depth a single pass happens to suffice, because two more
flush points follow it. It stays a loop because it is the correct invariant
— a leaf minted while checking the last instance of the last flush has
nothing behind it — but this is an assertion the suite does not yet test, and
it is recorded as such rather than claimed.

`src/testdata/codegen_stress/091_unannotated_accumulator_in_generic.cn`
(`# expect: 116`) is the local reproduction: an unannotated `[]` accumulator
in a generic body, instantiated at `int32` and at `int64`. Two
instantiations at different widths, so an element type leaking from the first
into the second gives a plausible wrong answer rather than a crash, and the
total is asserted rather than left to cross-tier agreement, which would agree
on that wrong answer. It is in the snapshot corpus too, where the template's
`list[T]` and the instances' `list[int32]`/`list[int64]` now appear as three
rows against the same line.

### Phase 10 — `method_call` as an obligation *(in progress)*

**Update, literal defaulting landed with the UFCS deferral.** An integer
literal now mints a leaf and a `defaulting` obligation; `demand(id)` spends
the default at points that need a type (generic solve, arithmetic, indexing,
`for` iterable, structural patterns, splice operands, interpolation, a lambda's
expected parameter types). A method call on a *named* open receiver with no
method of that name reaches UFCS and is deferred rather than defaulted
(`codegen_stress/095`, `# expect: 6000000006`; verified failing without the
deferral). Temporaries in a chain are still demanded, since nothing later can
pin them. Also fixed on the way: an array literal's leaf element was replaced
by the next element instead of unified with it, and `for`/comprehension
dispatch maps were not zonked at the boundary. Known limit: `demand` flushes
*every* pending default, not only those inside `id`.


Phase 8's two remaining leaves and phase 9's gates both wait on the same
missing thing: a call on a receiver that is not concrete *yet* is currently
**decided anyway**, or **suppressed**, and never *postponed*. The queue for
postponing it has existed since phase 4 and had no caller.

Two shapes, with the same cause.

**A dispatch that decides anyway.** `var xs = []` then `xs.len()` before any
`push`: `check_impl_generic_method_call` had a `mentions_type_param` guard
for a receiver still written in a *type parameter*, and nothing at all for a
receiver still written in a *metavariable*. So it named an instance after
one — `std.list::list::len$list___` — which is a function nothing ever
compiles. It happened to run only because `len` never touches the element
type; the snapshot is what made it visible, since the program's answer was
right.

Now the elaboration half is split out (`finish_impl_generic_method_call`) and
the call is registered as a `method_call` obligation watching the receiver's
own metavariables, carrying the file, module and both template flags it was
deferred from — the same discipline `pending_leaf_literal` and
`pending_instance` follow, for the same reason. `flush_deferred` runs the
queue after the leaves have settled, then drains the leaves again, because
running a call requests instances and wires literals of its own.

The arguments are deliberately *not* deferred with it. They have already been
checked, and they are frequently the thing that solves the receiver:
`out.push(7)` on a `list[?a]` is how `?a` becomes `int32` at all. Only the
decision waits.

A call still waiting when the queue stalls reports nothing from the queue:
the receiver's own literal has already said "cannot tell what an empty `[]`
is a list of", which is the same mistake in words the user can act on. Before
this change that program compiled, with a `len$list___` in it.

**A dispatch that is suppressed.** `xs.iter()` on the same open `xs` goes
through UFCS to the free `def iter[T](xs: &list[T])`, where
`solve_from_argument_types` refuses an argument still carrying an open leaf
(correctly — "an open leaf is not an answer") and `solve_generic_params` then
reports `T` unsolved.

That half is **written and then reverted**, because it cannot be tested. The
deferral is easy — the same `defer_method_call`, closing over
`instantiate_generic_function` instead — but a probe on every shape that
could exercise it found only one that reaches it at all:

| Shape | Deferrals |
|---|---|
| `var it = xs.iter()` before any `push` | 1 |
| `xs.iter()` after the `push`es | 0 |
| a user generic reached by UFCS, `xs.sum_or(5)` | 0 |

The last two are zero for a good reason: `check_ufcs_call` checks the
arguments *before* it instantiates, and checking them is what solves the
receiver, so by the time the deferral point is reached there is nothing left
to wait for. Only a receiver solved by something strictly *after* the call
reaches it qualifies — and a program that calls a method on a list before the
list has any elements is one whose call has nothing to do. The shape is
reachable (it is what the parked defaulting branch hit, where array-literal
elements are leaves too) but on master there is no leaf source that survives
past a call, so there is no program that both exercises the deferral and
computes anything.

Probing this is what turned up `spec/todo.md` item 21 — since fixed, and
unrelated to inference: a `&` to a number could be used *as* the number, and
the address was used instead.

So the branch was reverted rather than landed untested. What stayed is the
generalization it forced: the pending record holds a `std::function` for the
work and nothing path-specific, so the second caller is a closure at the call
site rather than a second table and a second resolver — which `obligations.h`
names as the way this queue gets rebuilt into the state it replaced. Item 21
is the gate.

#### Failability, verified

| Broken | What caught it |
|---|---|
| the resolver always answers `waiting` | `snapshot_test` — the call loses its resolved callee; `codegen_stress_test` does **not** catch it, because `len` lowers anyway |
| no deferral at all (the state before this phase) | `snapshot_test` — `len$list___` returns, in both instance tables |

Note which test did the work. `codegen_stress/093` passes with the wrong
instance name *and* with no instance at all: the value it computes is right
either way. Only the golden records which function the call resolved to,
which is the thing `snapshot_test.cpp`'s own header says nothing else in the
suite can see.

## Definition of done

A ledger, not a feeling:

| Metric | Now | Done |
|---|---|---|
| `unify_rigid` call sites | 28 | 0 |
| Ad-hoc `string -> type_id` binding maps | 67 | 0 |
| `in_*_template_` gates | 6 | 0 |
| Distinct inference solvers | 4 | 1 |
| `param_usage_inferrer` | deleted; unannotated params are leaves, open ones monomorphized per call (phase 8b) | deleted |
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
