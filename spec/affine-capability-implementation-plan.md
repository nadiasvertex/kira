# Affine Capability Core: Implementation Plan

Status: proposed implementation roadmap; no affine task below is complete. Q1 is resolved;
Q2's ownership-default, inference, basic surface-spelling, `copy`/`copy_owned` operation names,
the `copy` constraint name, single-element view consolidation, scoped range construction
and passing, retained frozen ranges with library `.slice()`, callable parameter names and
result-origin bounds, write bounds including `writes nothing` and the callable environment
root `self`, callable receiver spellings with inferred lambda call modes, capture lists
with value defaults, and capture origins using `from self` are accepted. Remaining contract
spellings and legacy compatibility mappings are pending.
The remaining T01 work and all proof milestones are pending.

Source: [Affine capability core](affine-capability-core.md).
Historical repository inspection: `3a41f74`, inspected on 2026-11-13.
Compiler-integration baseline: completed [value semantics](value-semantics.md), with the
landed revision and verification evidence to be recorded at V0 before integration starts.
Scope: complete roadmap, including formal validation, experimental implementation,
compiler/runtime integration, standard-library migration, and adoption.

## Overview

Implement the sequential affine capability core as an explicitly elaborated and independently
checked intermediate language. Surface value passing moves owned values by default;
duplication follows checked scalar, frozen-handle, and other capability duplication rules.
Declared inspection and edit parameters borrow or reborrow. Independent copy construction
requires an explicit request and a structural witness; freezing an owner is explicit too.
The core records every transfer, duplication, copy, loan, and freeze operation explicitly.
The compiler must account for every live carrier, loan,
initialized slot, cleanup obligation, storage version, and call outcome before executable
code is emitted. Deliver the same behavior through bytecode, LLVM, and compile-time execution,
then migrate the library and language documentation through an explicit adoption gate.

This plan does not make the proposal normative or claim its metatheorems already hold.
Formal validation precedes compiler implementation, as required by the proposal. A small
executable model may support that validation, but testing is not a substitute for proofs.

Included are all primitives and validation families in the proposal, structural copy and
freeze witnesses, sealed capabilities, checked closure contracts, finite proof certificates,
diagnostics, native-boundary contracts, and a migration path from current Cinder semantics.
Sequential generators are included, with checked suspension, resume, yielded-result origins,
stable frame storage, and synchronous disposal of suspended state. Named-field borrowing and
checked mutable-range splitting are included in the initial model and proof scope.

Excluded from the initial core are cyclic mutable ownership, interior mutation, raw-pointer
authority, concurrent mutation, thread-transfer
guarantees, exception unwinding, asynchronous cancellation, custom frozen finalizers, arbitrary
user copy axioms, release-only custom cleanup for freezing, and must-consume protocols.
Existing Cinder features in those categories require a
defined interoperability boundary or explicit rejection in the experimental mode. Their
existence must be reconciled before claiming general Cinder-wide adoption.

## Repository Findings

Line spans are historical navigation anchors for the inspected revision, not permanent
interfaces or a description of the completed value-semantics implementation. Re-query `graft`
and refresh the affected findings against the V0 baseline before integration changes.

| Concern | Inspected implementation | Consequence for this plan |
|---|---|---|
| Pipeline | `src/driver/driver.cpp:327–437` | Semantic validation runs before ownership checking and HIR lowering. Compile-time VM calls occur during semantic checking, so a final driver-only check is insufficient. |
| Ownership state | `src/semantic/ownership_cfg.h:49–154`, `311–352` | Existing locals include argument temporaries, join values, return slots, captures, and fields. Loans identify a local root and mutability; the new core needs explicit regions, granting chains, lifetime dependencies, and carrier availability. |
| Borrow propagation | `src/semantic/checker_results.h:830–891`; `src/semantic/check_type_resolution.cpp:429–477` | Borrow-bearing type summaries and generic origins are useful adapters, but do not describe the proposal's complete state or outcome contracts. |
| Drop construction | `src/semantic/check_type_resolution.cpp:536–620`; `src/hir/lower.cpp:3294–3439` | Current lowering calls custom drop and then field cleanup in declaration order. The proposal requires reverse structural order and a custom recipe that accounts for obligations without a second unconditional field pass. |
| Conditional cleanup | `src/hir/drop_schedule.cpp:189–303` | Existing drop flags encode maybe-moved states. The core instead requires predecessor normalization before unavailable bindings disappear at joins. Existing machinery cannot silently define the new semantics. |
| Surface borrows | `src/parser/parser.cpp:3785–3856`; `src/semantic/check_operators.cpp:1003–1332` | `&` and `&mut` already have parser, inference, index-dispatch, and fact-invalidation behavior. Proposed conversion annotations need an explicit language-mode and elaboration design. |
| Proof reasoning | `src/semantic/linear_poly.h:50–62`; `src/semantic/reason.cpp:251–283`; `src/semantic/predicates.cpp:120–145` | Existing polynomials use `int64_t` coefficients, and solving returns a proved/refuted/unknown classification. The new trusted kernel needs finite certificates, arbitrary-precision checking, and stable identities with dependencies. |
| Lowering during checking | `src/semantic/check_lowering_view.cpp:606–655`, `1165–1236`; `src/comptime_vm/program.cpp:257–278` | New contracts and verified bodies must be available in the incremental lowering view, not just in final checker results. |
| Physical representation | `src/runtime/layout.cpp:249–299`; `src/semantic/type_table.cpp:657–672` | The inspected revision used heap pointers for many aggregates. Integration uses the completed inline layout and shared storage pipeline from value semantics; these historical representation decisions must not be carried forward. |
| Library sharing | `src/std/shared.cn:1–65` | `shared[T]` is a library struct using raw pointers and atomic counts; final release invokes mutable value cleanup. It is not automatically the proposal's permanently frozen capability. |
| Library sequences | `src/std/list.cn:90–189` | `push`, `pop`, replacement, and cleanup use machine operations. The new core needs checked sequence representation, transfer, and initialized-prefix contracts. |
| Test entry points | `src/semantic/ownership_check_test.cpp:182–188`; `src/bytecode_compiler/compile_test.cpp:91–116`; `src/llvm_codegen/codegen_test.cpp:124–140` | Existing fixture patterns are reusable, but execution fixtures must explicitly invoke the new verification boundary. Type-checking success alone is not evidence of capability safety. |

The older merged grammar/reference/roadmap paths in the repository instructions are absent
from this checkout. Use `spec/cinder-grammar.ebnf`, `spec/CONVENTIONS.md`, and the chapter tree
under `spec/specification/`, especially chapters 14–17, 33–34, and 62–67.

### Required value-semantics foundation

The [value-semantics work](value-semantics.md) is nearing completion and is the required
foundation for affine compiler integration. Plan against its completed representation:
inline aggregates and descriptors, concrete recursive layouts, typed places, explicit
initialization and transfers, caller-provided result storage, mandatory RVO, shared NRVO,
and verified frame/lifetime decisions on both backends and compile-time paths.

**V0** is the external baseline gate: record the landed revision, actual interfaces and test
targets, and completion evidence for value-semantics T-01–T-14 in
`spec/affine-capability/decisions.md`. Refresh the historical findings above at that revision.
This plan does not certify those tasks complete or alter their status. Formal work T01–T09
and the independent core/kernel T10–T29 can proceed without V0; source integration T30–T38
and runtime/HIR integration T39–T46 require it. The task grid expresses those dependencies
through T30 and T39. No integration path falls back to pre-value-semantics boxed aggregates.

Value semantics preserves source ownership rules while changing physical storage and the
ABI. Affine capabilities changes the source rules and supplies checked operations to that
storage infrastructure. Extend the completed implementation instead of creating another
layout engine, aggregate ABI, destination-lowering pass, or backend ownership analysis.

| Foundation from value semantics | Affine integration obligation |
|---|---|
| T-02–T-04: concrete layouts, typed places, aggregate ABI, initialization and drops | T39/T42 map semantic regions and checked copy/transfer/cleanup recipes onto those interfaces; authority never follows from layout alone. |
| T-05–T-08: VM/LLVM storage, runtime boundaries, containers, statics, compile-time marshalling | T40–T45 extend the same prepared storage and call/result contracts for capability operations and retained projections. |
| T-09: captures, stable suspension storage, arbitrary-size results | T27/T42 preserve capture and sequential-generator frame ownership, dependencies, and address stability under the T02–T04 model. T52 covers async/concurrency crossings; representation support alone does not establish affine safety. |
| T-10–T-12: mandatory RVO, shared NRVO, frame budgets and tail-call safety | T46 preserves these guarantees and validates transformations against capability origins, source availability, and cleanup obligations. |
| T-13–T-14: allocation/backend evidence and removal of legacy representations | V0 records the baseline evidence; T55–T58 retain those regressions and measure capability-specific costs separately. |

## Decision Gates

These are implementation decision gates. None prevents writing this roadmap. Resolve each
in the named task before its dependent implementation starts; record the answer and rationale.
Resolved choices do not establish their associated proofs or complete their owning tasks.

| ID | Question | Decision or proposed starting position | Blocks |
|---|---|---|---|
| Q1 | Which formalization tool and proof artifact format will establish the required properties? | Resolved: Lean 4 with Lake; version-controlled `.lean` definitions and proofs are authoritative. Require a pinned toolchain and dependencies, reproducible kernel checking, theorem traceability, and assumption reports. See [decision Q1](affine-capability/decisions.md#q1--formalization-toolchain-and-proof-artifacts) and the [formal artifact contract](affine-capability/formal/README.md). Toolchain bootstrap and proofs remain pending in T01–T09. | T02–T09 |
| Q2 | What are the final declaration, result-origin, effect, local-builder, and conversion syntaxes? How do existing `self`, `&mut`, views, `copy`, and `shared` map? | Partially resolved: move-by-default, inferred checked contracts with optional constraints, and basic [surface spellings](affine-capability/surface.md) are accepted. `&` requests observation, `&mut` editing; neither is silently reinterpreted. Preserve reference/receiver shorthand and `let`/`var`; use `shared_read[T]` in the experiment without aliasing legacy `shared[T]`. Use `copy(x)` for same-type value copying and `copy_owned(h)` for fresh-owner materialization; neither operation is named `clone`. The `copy` constraint requires a structural witness; retire its legacy implicit-duplication meaning in the experiment without introducing another public constraint. Collapse cells into `inspect[T]`/`edit[T]`, including checked slot views; preserve slice/text spellings. Scoped ranges observe by default, use explicit mutable slicing, reborrow by parameter mode, and transfer affine views without losing origins. Retained frozen ranges use `shared_slice[T]`/`shared_str` and ordinary library `.slice()` over a checked intrinsic. Callable parameter names and per-invocation result-origin bounds are accepted; write bounds use `writes nothing` or nonempty root sets, with `self` naming the callable environment. Callable receivers use `fn(self, ...)`, `fn(mut self, ...)`, or `fn(self: own[Self], ...)`, with lambda call modes inferred and optional constraints checked. Captures use `[x]`, `[&x]`, `[&mut x]`, or `[]`; omitted lists infer value captures. Capture-derived result bounds use `from self` or mixed origins such as `from {part, self}`, preserving actual dependencies. Remaining contract syntax and legacy compatibility mappings stay open for T08. See [decision Q2](affine-capability/decisions.md#q2--ownership-default-and-explicit-copying). | T08, T30–T36 |
| Q3 | How do semantic regions map to the completed value-semantics typed places and genuine backing allocations? | Reuse concrete layouts and storage provenance. Regions denote semantic storage identity, not every C++ or VM allocation. An inline field shares its containing region; checked field paths and range partitions refine access footprints without creating new regions. Address reuse never reuses a semantic identity. | T39–T46 |
| Q4 | Which frozen-handle representation retains an inline projection's backing allocation? | Use a shared control block plus a checked projection descriptor, or an equivalent representation justified in T39. Reference counting alone does not establish deep freeze. | T39–T44, T49 |
| Q5 | Which allocation failures are ordinary error outcomes and which remain fatal process failures? | Model recoverable construction failure explicitly. Preserve existing fatal behavior until a deliberate runtime policy is implemented; never imply that panic unwinds. | T04, T39, T41, T57 |
| Q6 | How is the standard library selected during the experimental transition, and may legacy and capability modules mix? | One semantics mode per compilation session, including the selected prelude. Reject unchecked cross-mode calls; permit only explicitly contracted adapters. | T30, T37–T38, T47–T52 |
| Q7 | Can existing async, scheduler, machine, and shared APIs remain available under the eventual default? | Sequential generators, including loans across yield and synchronous suspended-state disposal, are inside the core claim. Async, asynchronous cancellation, scheduler/concurrency, and unchecked machine/shared APIs remain outside it. T52 must choose tested boundaries, an explicit unsupported subset, or a separately specified extension before general adoption. | T52, T60–T62 |
| Q8 | What compile-time and runtime resource limits are acceptable? | Establish baseline workloads first. Set certificate-node, integer-bit, abstract-state, compile-time, and retained-memory budgets from measurements, never by accepting unverified results. | T20–T23, T29, T58, T60 |

## Assumptions

1. The user-selected deliverable is a complete adoption roadmap. No implementation, release,
   or default-language change is authorized by this planning document itself.
2. New semantics initially run in an opt-in experimental mode. The proposed switch name
   `--experimental-affine-core` is provisional. Existing compilation retains current behavior
   until the adoption gate; incompatible modules never silently share an ABI.
3. Introduce a small `src/capability/` library for the core IR and verifier. Introduce
   `src/proof/` for the certificate kernel. Both directories are new proposed locations,
   following the repository's separate-library and adjacent-test conventions.
4. Representation types remain nominally unchanged by freezing. Capability access controls
   projections; there is no recursive `Frozen[T]` type rewrite.
5. The initial checker may conservatively reject programs when aliasing, lifetimes, effects,
   or loop invariants cannot be established. Resource-limit exhaustion is an explicit failure
   to check, never proof success or permission to ignore an obligation.
6. Fresh runtime regions allocated in loops require symbolic allocation sites, binders, and
   sound summaries in static analysis. They cannot be modeled by an ever-growing list of
   concrete identities during fixed-point iteration.
7. Last-use analysis proposes explicit retirement. The verifier checks it. The specified
   observable cleanup schedule includes predecessor-edge disposal for values moved on only
   some branches, as well as scope exits, replacement, and explicit disposal. Moving cleanup
   earlier than those points requires a separate checked transformation.
8. Native memory and external-resource operations form a small, documented trusted boundary.
   Ordinary Cinder modules cannot mint authority, witnesses, or effect summaries by assertion.
9. C++ changes follow `spec/CONVENTIONS.md`: Clang 22.1+, C++26 mode, snake_case, explicit value
   ownership, RAII, and `std::expected` for recoverable checking failures. Runtime C ABI glue
   may require raw addresses; isolate it and document the storage contract.
10. All source tasks include their matching Bazel declarations and focused tests. New tests
    follow existing adjacent `*_test.cpp` and `src/testdata/<suite>/` patterns.
11. Formal proof completion and language adoption are gates with evidence, not calendar
    estimates. Split theorem work into individual lemmas when T01 selects the formal system.
12. Both legacy and experimental source modes use the completed value-semantics storage
    foundation. Their source ownership rules differ; experimental mode must not restore
    aggregate shell allocations or bypass shared storage verification. References below to
    `value_storage`, `value_lowering`, `value_verify`, and `return_storage` use the interface
    names from that plan; V0 maps them to the actual landed files and symbols.

## Architecture and Integration Boundaries

```text
source + experimental language mode
    -> parse / resolve / infer representation types and callable interfaces
    -> elaborate ordered capability operations and proof obligations
    -> insert justified retirement, edge normalization, and cleanup
    -> verify core bodies, contracts, witnesses, and proof certificates
    -> verified capability program
    -> lower explicit runtime operations to typed HIR; retain required provenance
    -> shared HIR transformations; refresh capture and suspension facts
    -> value-semantics layouts and shared storage lowering with mandatory RVO
    -> shared NRVO and safe storage reuse
    -> final frame budgets, tail-call validation, and storage verification
    -> erase remaining static evidence after its last validation use
    -> bytecode VM / LLVM JIT and AOT from the same prepared storage decisions

compile-time calls -> the same per-body capability gate and verified storage pipeline
```

The integration may require a staged type-checking protocol: resolve enough types and
interfaces to elaborate a body, then discharge storage-dependent obligations in core order.
Do not let an earlier AST-level bounds decision authorize code generation after core effects
invalidate the fact. Generic bodies are checked once using abstract regions, type-parameter
dependency variables, and checked interfaces. Instantiation substitutes dependency summaries
as well as types and regions; instantiated bodies and witnesses retain those constraints.

| Component | Owns | Must not do |
|---|---|---|
| Elaborator | Evaluation order, explicit conversions, carriers, source spans, proposed cleanup and evidence | Invent permission, erase dependencies, or insert unchecked implicit copies |
| Core verifier | Well-formed state, forest conflicts, loans, initialization, cleanup accounting, effects, outcome transitions | Re-run arbitrary Cinder code or assume distinct region names imply disjointness |
| Proof kernel | Finite derivations over immutable identities, versioned terms, linear arithmetic, and nominal witnesses | Manufacture ownership, retarget region identities, or trust automation output |
| HIR adapter | Translate checked copy, transfer, retain/release, allocation, and cleanup into typed HIR consumed by shared storage lowering | Recompute a different cleanup schedule or collapse structural copy into a byte transfer |
| Shared value-storage pipeline | Concrete layouts, typed places, destinations, aggregate ABI, RVO/NRVO, frame budgets, and physical lifetime/initialization verification | Infer capability authority from bytes or reuse storage in conflict with checked origins, loans, or cleanup |
| Runtime | Allocation, stable addresses where required, retention, release, primitive commits, native contracts | Thaw frozen storage or recreate mutable destructor authority on final release |

Use a verified-program result type or similarly explicit validation record so normal
experimental backend entry points cannot accept an unverified body. This is an engineering
boundary, not a claim that a C++ wrapper alone proves the compiler correct. The formal core,
its implementation correspondence, native boundary, and backend realization remain distinct
verification obligations.

Capability verification and storage verification are cumulative gates. The first establishes
permission and semantic obligations; the second validates their physical realization after
storage transformations. Preserve the origin, initialization, and cleanup information needed
for downstream checks even when proof terms have already been erased. Apply both gates to
normal compilation, synthesized functions, compile-time calls, and direct-HIR test helpers.

The core state must distinguish region IDs, loan IDs, carrier IDs, immutable value IDs,
storage version IDs, and outcome IDs. It records exclusive and frozen edges separately,
initialization/cleaning states, pending cleanup, capability availability, loan granting chains,
lifetime dependencies, and facts with their complete dependency sets. Keep representation
layout data out of permission logic.

## Task Grid

No affine task is complete; T01 has a recorded Q1 decision, with its remaining work pending.
T08 has accepted Q2 ownership-default, inference, surface-spelling, copy-operation naming,
`copy` constraint naming, single-element view consolidation, scoped range construction
and passing, retained frozen-range/library `.slice()`, callable result origins, write bounds,
inferred callable receivers, capture spellings/defaults, and capture-origin decisions;
the remaining Q2 choices are pending.
V0 is an external prerequisite, not an affine implementation task.
**P0** denotes a semantic foundation or execution-safety gate;
**P1** denotes required integration, validation, or adoption work. There are no optional tasks
hidden among the completion requirements. Dependencies are technical prerequisites; independent
rows may proceed independently. This table does not assign agents or prescribe delegation.

Paths under `src/capability/`, `src/proof/`, and `spec/affine-capability/` are **new** unless
created by an earlier task. Other explicitly new files are marked **new**. Brace notation
such as `forest.{h,cpp}` denotes the corresponding header and implementation files.

### Required model decisions before proofs

The following decisions are requirements for T02–T04, not deferred compiler choices for
T32 or compatibility choices for T52. T01 records them and reconciles the core, surface,
translation, and formal documents. T08 supplies the representative translations before
T05–T07 begin. Acceptance criteria below require proof of these rules, not just prose agreement.

**Named-field separation.** Keep allocation identities for ownership and lifetime while
refining access and loan footprints by checked named-field paths. Distinct structural fields
may be borrowed independently; equal and prefix paths overlap. Preserve resolved target
provenance through stored aliases and private abstraction contracts. Whole-parent operations
and whole-receiver requirements cover every affected field and owned descendant. Ownership
edge changes check the containing slot and incoming/outgoing owners before changing the
forest. Effects preserve any required footprint precision; fact invalidation may remain
allocation-wide. T05 proves separation and lifetime safety before migration relies on them.

**Checked mutable range splitting.** Include built-in `split_at(view, index)` and its core
partition transition. Check the index against the visible range before exposing either
affine result; no caller-supplied disjointness proof is required. Preserve backing identity,
granting dependencies, and partition ancestry through nested splits, transfers, calls, and
returned/stored descendants. Empty partitions permit no element access. Conflicting parent
authority resumes only when no surviving loan blocks it. Arbitrary indices or independently
chosen subrange bounds remain insufficient evidence of separation. Runtime bounds failure
uses the selected policy without creating partial child-loan state.

**Deferred extensions.** Release-only custom cleanup for freezing and nonfreezable scalar
interior mutability remain outside this plan's initial model. Keep mandatory custom unique
cleanup nonfreezable and ordinary inspection nonmutating. A later cleanup proposal must
establish recursive obligation accounting and observable effects/timing; a later interior
mutation proposal must address alias authority, fact invalidation, callback effects, and
closure sharing. T48/T49 must identify unsupported container-freezing cases rather than
silently assuming either extension. The accepted absence of a public duplication bound and
explicit structural-copy policy remain unchanged. See the
[decision record](affine-capability/decisions.md#deferred-ergonomic-extensions).

**Loan-bearing type parameters.** Associate each opaque type parameter with abstract loan
dependency variables and constraints; absence of known fields never means an empty dependency
set. Define composition through products, sums, containers, and callable environments,
including `list[str]` and `option[&T]`. Moves, extraction, replacement, returns, and cleanup
preserve or discharge the corresponding carrier obligations. Type and region substitution
also substitutes these summaries, preserving loan identity, granting chains, and lifetime
constraints without inventing independence between symbolic origins. T05 establishes the
substitution lemma needed to check generic bodies once against their bounds. No new public
annotation is required merely to store a view in a generic container.

**Receiver-stored result origins.** Use one origin model for closures, trait receivers, and
generators. Distinguish a loan of receiver/frame-owned storage, a loan carried inside the
receiver from external backing, and a fresh dependent reborrow. `from self` remains a bound
on permitted sources; it does not by itself identify which relationship holds. Contracts
must expose abstract stored-dependency binders and result relationships without exposing
private fields. T08 settles any additional surface spelling needed to express that precision
at a trait or opaque callable boundary. T24/T32/T37 preserve and check those relationships
through dispatch, substitution, and metadata; callers never inspect the implementation.
An iterator over externally borrowed backing must support two successive `next` results
remaining live when advancing it cannot invalidate either result. A result borrowing
receiver-owned storage may prevent another mutating call or receiver disposal. Neither the
origin bound nor a dependency binder bypasses authority or conflict checks.

**Cleanup at joins.** Predecessor-edge disposal is a specified source-language cleanup point.
When a value is moved on one branch and unavailable after the join, each predecessor that
still owns it disposes it before the join, in the specified cleanup order. This can run an
observable destructor before lexical scope exit; it is not a last-use optimization. Apply
its effects before merging facts and reject disposal conflicting with a surviving loan.
Alternatively, predecessors may initialize a compatible common result. The core does not
retain maybe-initialized obligations through runtime drop flags. Assumption 7 protects this
schedule, including the edge disposal points, from unjustified movement.

**Sequential generators.** Model a generator as a closure environment plus a state machine
with stable frame storage. Define creation, resume, yield, completion, modeled failure, and
synchronous disposal transitions. Each suspension state records initialized slots, live
carriers, authority, pending cleanup, and result origins. Loans remain active across yield;
caller actions between resumes must preserve the suspended state's invariant. Reject
reentrant resume and any resume, relocation, or disposal that conflicts with a live loan.
External-backed yielded observations may survive subsequent resumes when checked effects
permit; frame-backed results retain frame dependencies and restrict resume and destruction.
Moving a generator handle must preserve the identity and address stability of borrowed
frame storage. Completion and early loop exit/drop dispose the appropriate initialized
state exactly once in its specified order. This synchronous disposal is required even if
legacy lowering calls it cancellation; async cancellation and concurrent resume stay outside
the claim. Stable storage alone supplies neither loan safety nor cleanup correctness.

T08 must translate iteration over `self.items` with direct updates of `self.total`, simultaneous
edits of split children, and nested/empty splits with returned or stored child dependencies.
Include rejected same/prefix-field accesses, whole-receiver conflicts, stored aliases,
invalid split bounds, forged/widened partitions, and premature parent reuse. These complement
a generic container of views, two live iterator results through a trait,
an observable destructor on the nonmoving branch, and a borrowed generator suspended then
resumed or dropped. Include rejected dangling-backing and conflicting frame-loan variants.
These examples validate model coverage before proofs; T54–T57 later supply executable evidence.

### Formal foundation

| ID | Task | Target File(s) | Depends On | Priority | Acceptance Criteria |
|---|---|---|---|---|---|
| T01 | Establish requirement IDs, decision records, and formal tooling | `spec/affine-capability/decisions.md`, `spec/affine-capability/requirements.md`, `spec/affine-capability/formal/README.md` | — | P0 | Every proposal rule and example has an ID; Q1 is resolved; proof commands, assumptions, and the validation matrix format are specified. Record all required model decisions and reconcile their scope and cleanup semantics across the core, surface, translation, and formal documents before T02. |
| T02 | Define core syntax, heap typing, contexts, and state well-formedness | `spec/affine-capability/formal/state.md`, `spec/affine-capability/formal/syntax.md` | T01 | P0 | Regions, carriers, loans, facts, initialization, cleanup, and distinct edge kinds have precise judgments. Separate allocation identity from named-field footprints and checked range partitions, including resolved target provenance, indexed-access conservatism, partition ancestry, and owning-slot paths. Include type-parameter dependency variables and composition, abstract receiver-stored origins, and sequential-generator frames with per-suspension initialization and loan invariants. |
| T03 | Define primitive, projection, copy, freeze, and disposal transitions | `spec/affine-capability/formal/primitives.md` | T02 | P0 | Every primitive has a transition rule, rejection conditions, and pre/post-state invariants, including partial construction and atomic commit points. Define named-field projection and atomic split_at reborrowing with checked bounds, empty/nested partitions, and no partial result on bounds failure. Define generator creation/resume/yield/completion/failure/disposal transitions, with stable storage, conflict checks, and exactly-once state cleanup. |
| T04 | Define contracts, outcome alternatives, sequencing, joins, and loops | `spec/affine-capability/formal/control.md`, `spec/affine-capability/formal/contracts.md` | T03 | P0 | Evaluation contexts retain intermediates; ordinary error variants have complete states; loops and cleanup exits are explicit. Define dependency substitution and stored-origin contract relationships, preserving field footprints and split ancestry through returned/stored views; predecessor disposal is a specified cleanup point. Sequential suspension preserves loans across intervening caller operations and exposes yielded-result origins. |
| T05 | Establish preservation, progress, and loan/freeze safety lemmas | `spec/affine-capability/formal/safety.md`, formal proof modules selected in T01 | T04, T08 | P0 | Derivations establish preservation, progress modulo stated failures, no conflicting access, permanent freeze, and nonescape of dead storage under documented assumptions. Prove named-field and split-partition separation, no conflicting parent access, dependency-preserving transfers, and whole-parent transition safety. Prove generic dependency substitution and safety of trait stored-origin instantiation and sequential suspension/resume, including intervening caller actions. |
| T06 | Establish copy equivalence/separation and cleanup accounting lemmas | `spec/affine-capability/formal/copy-cleanup.md`, formal proof modules | T04, T08 | P0 | Copy recipes preserve source state and construct equivalent fresh mutable reach; every completed exit accounts for each obligation once. Cover observable predecessor-edge cleanup timing and each generator suspension state on completion, failure, and synchronous disposal. |
| T07 | Establish kernel soundness and proof-erasure correspondence | `spec/affine-capability/formal/proofs.md`, `spec/affine-capability/formal/erasure.md`, formal proof modules | T04, T08 | P0 | Assumption discharge, machine-integer premises, dependency preservation, and erasure are justified without granting new authority. Preserve stored/generic dependencies, suspension invariants, and specified cleanup points through erasure. |
| T08 | Translate representative Cinder programs and define the surface mapping | `spec/affine-capability/surface.md`, `spec/affine-capability/translations.md`, `spec/affine-capability/decisions.md` | T04 | P0 | Resolve the remaining Q2 syntax and compatibility mappings under the accepted move-by-default policy; mutation, resource pop, fallible calls, closures, sharing, and generic examples translate to explicit core operations, including explicit copy/freeze and default reborrows. Before T05–T07, settle trait stored-origin spelling and translate all required model examples with their rejection counterparts; no model requirement is deferred to T32/T52. |
| T09 | Publish the formal readiness evidence | `spec/affine-capability/formal/README.md`, `spec/affine-capability/requirements.md` | T05, T06, T07, T08 | P0 | Required theorem artifacts check reproducibly, unresolved assumptions are visible, and no unproved theorem is described as established. Compiler work below starts only after this gate. |

Formal `.md` files are explanatory companions. The selected Lean modules and planned Lake
target are named in the [formal artifact contract](affine-capability/formal/README.md).
T01 must bootstrap and pin that package and validate its checking commands; those artifacts
do not yet exist. T05–T07 are proof milestones to decompose into named lemmas, rather than
fixed one-session promises.

### Core state, primitives, and control flow

| ID | Task | Target File(s) | Depends On | Priority | Acceptance Criteria |
|---|---|---|---|---|---|
| T10 | Add typed core identifiers, IR, and deterministic dumps | `src/capability/ids.h`, `src/capability/ir.{h,cpp}`, `src/capability/dump.{h,cpp}`, `src/capability/BUILD.bazel` | T09 | P0 | Every proposed primitive has an explicit operand/result form, source span, and carrier/region references; malformed IDs fail validation. |
| T11 | Implement region state, forest queries, and checked edge updates | `src/capability/state.{h,cpp}`, `src/capability/forest.{h,cpp}`, `src/capability/forest_test.cpp` | T10 | P0 | Ownership is acyclic and single-parent; inline paths share allocation identity but distinct named fields have checked disjoint footprints. Equal/prefix paths overlap; unknown aliases and arbitrary indices remain conservative. Range partitions preserve ancestry. All transfers/replacements validate affected slots, old/new ownership paths, and surviving state. |
| T12 | Implement carriers, dependent loans, and suspended authority | `src/capability/carriers.{h,cpp}`, `src/capability/loans.{h,cpp}`, `src/capability/loans_test.cpp` | T11 | P0 | Duplication, reborrow, transfer, retirement, and end-loan preserve dependencies, including slot allocation/path and range partition provenance through stored and returned views; granting-chain exceptions never exempt competing loans. Track abstract dependency summaries for opaque generic components and suspended generator frames without erasing nested origins. |
| T13 | Implement slot initialization, moves, take-field, and init-field | `src/capability/initialization.{h,cpp}`, `src/capability/initialization_test.cpp` | T12 | P0 | Moves transfer obligations; empty fields prevent whole-value operations; owner-only extraction and reinitialization are enforced. |
| T14 | Implement exchange, remove-last, and consuming sum elimination | `src/capability/structural_ops.{h,cpp}`, `src/capability/structural_ops_test.cpp` | T13 | P0 | Noncopyable elements transfer once; sequence prefixes and sum payload states remain valid; preparation failures precede commit. |
| T15 | Implement structural/custom cleanup and replacement recipes | `src/capability/cleanup.{h,cpp}`, `src/capability/cleanup_test.cpp` | T14 | P0 | Reverse structural order, cleanup effects, private teardown, replacement temporaries, and completed-exit accounting are enforced without double cleanup. |
| T16 | Implement capability-dependent projection and deep freeze | `src/capability/projection.{h,cpp}`, `src/capability/freeze.{h,cpp}`, `src/capability/freeze_test.cpp` | T15 | P0 | Contents projection and checked slot borrowing preserve distinct target authority; slot views verify paths, initialization, bounds, and permitted replacement, preserving named-field separation and inherited range partitions. Range construction checks bounds, initialized storage, and text boundaries, preserving backing-allocation and granting dependencies; subranges alone do not establish disjoint authority. Implement split_at with bounds validation before either child exists, affine child loans, empty/nested partitions, and parent suspension until conflicting descendants end. Retained frozen ranges preserve checked backing identity and visible bounds, independently retaining storage without a source-handle loan or element copying. Owned descendants freeze recursively; inline shared projections retain backing storage; scoped loans and mandatory custom unique cleanup block freezing. |
| T17 | Derive and verify structural copy/materialization witnesses | `src/capability/copy_witness.{h,cpp}`, `src/capability/copy_witness_test.cpp` | T16 | P0 | Product/sum/sequence/owned-child recipes prove equivalence and fresh mutable reach; frozen handles retain independently; unsupported opaque or scoped components are rejected. Witness availability neither enables implicit construction nor makes owners duplicable; transfer needs no witness. |
| T18 | Add core CFG liveness and explicit retirement insertion | `src/capability/liveness.{h,cpp}`, `src/capability/retirement.{h,cpp}`, `src/capability/liveness_test.cpp` | T15 | P0 | Temporaries, stored components, captures, and cleanup uses participate; early retirement never silently advances observable disposal. |
| T19 | Normalize joins and verify loop invariants | `src/capability/control_flow.{h,cpp}`, `src/capability/control_flow_test.cpp` | T18, T16 | P0 | Incoming edges reconcile obligations and initialization; unavailable bindings never revive; bounded fixed points preserve possible origins and loop freshness. Joins and loop summaries preserve field/partition relationships without identifying unrelated split instances or inventing separation. Predecessor disposal occurs at the specified language cleanup point, with observable trace checks and rejection when surviving loans prevent normalization. |

### Proofs, effects, calls, and abstractions

| ID | Task | Target File(s) | Depends On | Priority | Acceptance Criteria |
|---|---|---|---|---|---|
| T20 | Implement proposition syntax and finite certificate validation | `src/proof/terms.{h,cpp}`, `src/proof/certificate.h`, `src/proof/kernel.{h,cpp}`, `src/proof/BUILD.bazel` | T09 | P0 | Stable IDs, scoped assumptions, logical constructors, nominal equality, and acyclicity are checked; resource exhaustion cannot return success. |
| T21 | Implement arbitrary-precision linear certificate checking | `src/proof/integer.{h,cpp}`, `src/proof/linear_certificate.{h,cpp}`, `src/proof/kernel_test.cpp`; `MODULE.bazel` if a new dependency is required | T20 | P0 | Coefficients normalize exactly; nonnegative combinations and encoded domain bounds are checked; malformed arithmetic and overflow assumptions are rejected. |
| T22 | Make bounded arithmetic automation emit checked certificates | `src/semantic/reason.{h,cpp}`, `src/semantic/linear_poly.{h,cpp}`, `src/proof/automation.{h,cpp}`, `src/semantic/reason_test.cpp` | T21 | P0 | Every proof used by the new mode carries a kernel-accepted certificate; unsupported reasoning returns unknown without weakening an obligation. |
| T23 | Track storage versions and complete fact dependencies | `src/capability/facts.{h,cpp}`, `src/capability/effects.{h,cpp}`, `src/capability/facts_test.cpp` | T11, T19, T21 | P0 | Writes, transitions, ancestors, and cleanup invalidate affected facts across old and possible new forests; snapshots never become current facts by relabeling. Disjoint field or partition authority does not imply fact preservation; allocation-wide invalidation remains permitted. |
| T24 | Define callable, representation, and native contract schemas | `src/capability/contracts.{h,cpp}`, `src/capability/representation_contract.h`, `src/capability/contract_test.cpp` | T10, T23 | P0 | Modes, abstract regions, writes, transitions, origins, outcomes, initialization, cleanup, and postconditions are representable without inspecting a callee body. Contracts preserve named-field footprints and split provenance; whole-receiver requirements remain whole-receiver loans. Callable origin binders are independent of spelling and instantiate separately per invocation. Represent type-parameter dependency variables and receiver-stored origin binders/result relationships for traits, closures, and generators independently of private layout. |
| T25 | Instantiate calls and preserve per-outcome states | `src/capability/calls.{h,cpp}`, `src/capability/outcomes.{h,cpp}`, `src/capability/calls_test.cpp` | T24, T12, T23, T19 | P0 | Arguments stay live in order; preconditions use the post-conversion input state; results acquire dependencies before argument retirement; later effects update every pending alternative. Instantiate stored origins separately from the temporary receiver loan, preserving carried-loan versus dependent-reborrow relationships. |
| T26 | Check bodies against declared contracts | `src/capability/check_contract.{h,cpp}`, `src/capability/check_contract_test.cpp` | T24, T25, T15, T19 | P0 | All returns establish their output states after cleanup; recursive calls use declared summaries; callbacks/cleanup are included and false `writes nothing` claims fail, including writes through captured edit targets. |
| T27 | Check closure environments, call modes, and sequential generators | `src/capability/closures.{h,cpp}`, `src/capability/closures_test.cpp`; **new** `src/capability/generators.{h,cpp}`, `src/capability/generators_test.cpp` | T17, T25, T26 | P0 | Infer lambda entry authority and check optional receiver constraints; consuming/editing/observing entries respect captures, with call mode separate from callback passing mode and effects; capture-owned result loans remain supported, while duplicated captured observations retain original dependencies; freeze validates a shared-callable body; copied environments substitute fresh identities without inventing proofs. Verify sequential-generator suspension states and resume/yield/completion/failure/disposal transitions; external-backed and frame-backed results obey distinct dependencies, and reentrant/conflicting resumes fail. |
| T28 | Check sealed capability constructors and operations | `src/capability/tokens.{h,cpp}`, `src/capability/tokens_test.cpp` | T25, T26, T17 | P0 | Private authority cannot be duplicated; hidden storage effects and retained loans remain exposed; tokens cannot acquire copy witnesses from their byte layout. |
| T29 | Assemble the verifier and model-conformance harness | `src/capability/verify.{h,cpp}`, `src/capability/verify_test.cpp`, `src/capability/model_test.cpp`, `spec/affine-capability/requirements.md` | T16, T17, T19, T26, T27, T28 | P0 | Each core feature is checked together, with deterministic diagnostics, conservative resource limits, and traces comparable to the formal transition model. |

### Source elaboration and compiler integration

| ID | Task | Target File(s) | Depends On | Priority | Acceptance Criteria |
|---|---|---|---|---|---|
| T30 | Add experimental mode, syntax, and AST preservation | `src/driver/cli.{h,cpp}`, `src/driver/driver.h`, `src/parser/token.h`, `src/parser/lexer.h`, `src/parser/parser.cpp`, `src/parser/ast.h`, `src/parser/ast_clone.cpp`, `src/parser/ast_traversal.h` | T08, T09, V0 | P0 | Capability/conversion syntax follows the chosen mapping, including `writes nothing` and nonempty write root sets in declarations and callable types; `writes {}` is rejected with guidance distinguishing prohibition from inference; optional leading callable receivers `self`, `mut self`, and `self: own[Self]` remain distinct from ordinary arguments; preserve capture-list presence and modes plus environment/mixed result origins; cloning and traversal retain these forms; ordinary mode retains the completed value-semantics behavior and diagnostics. |
| T31 | Resolve capability types and callable signatures | `src/semantic/type_table.{h,cpp}`, `src/semantic/checker_results.h`, `src/semantic/check_type_resolution.cpp`, **new** `src/semantic/capability_types.{h,cpp}` | T30, T24 | P0 | Experimental single-element views use `inspect[T]`/`edit[T]`; any cell compatibility spellings are exact aliases. Slot `edit[own[T]]` and child `edit[T]` remain distinct; slice/text types retain range shape and scoped parameter modes. `shared_slice[T]`/`shared_str` are distinct retained frozen range types; wrapping scoped descriptors must not erase loans. Representation type and authority stay separate; parameter/result modes and loan origins survive function values, methods, and explicit annotations. Callable environment receiver modes remain distinct from outer parameter borrowing or ownership; lambda modes are inferred and optional explicit constraints checked. Result `from self` binds the environment root, including in mixed input/environment sets. Optional callable parameter names bind result-origin clauses; consistent renaming and different implementation parameter names preserve meaning. Omission retains sufficient checked summaries or sound conservative defaults. |
| T32 | Integrate witnesses, generics, private representations, and dispatch | `src/semantic/check_bounds.cpp`, `src/semantic/check_generic_calls.cpp`, `src/semantic/check_instances.cpp`, `src/semantic/check_lowering_view.cpp`, **new** `src/semantic/capability_witnesses.{h,cpp}` | T31, T17, T27, T28 | P0 | Experimental `copy` constraints resolve to structural witnesses, never the legacy implicit-duplication classification; introduce no public duplication constraint. Generic substitution preserves abstract regions and obligations; owning generic parameters need no copy witness unless they explicitly copy/materialize; trait/indirect implementations cannot exceed promised effects or strengthen required authority. Check generic bodies once with dependency variables; substitute nested loan summaries and abstract stored-origin binders through trait dispatch without requiring concrete layouts or erasing obligations. |
| T33 | Elaborate expressions into ordered core bindings | **new** `src/semantic/capability_elaboration.{h,cpp}`; `src/capability/ir.h` | T31, T13, T16 | P0 | Names, places, aggregates, captures, scalar/shared loads, and temporaries have explicit carriers and access operations with source spans. Named-field projection narrows the checked footprint without first borrowing the entire parent. Elaborate split_at to the explicit checked partition operation with source-relative bounds and affine result carriers. Explicit and inferred value captures move affine values and duplicate only by checked rules; explicit observation/edit captures preserve loans and reborrows without escape-dependent mode changes. `*slot` preserves checked place provenance; exchange and replacement use slot authority without first extracting its owner. Range indexing observes by default; explicit observation/edit range requests return capabilities directly, with checked bounds and text boundaries, rather than references to temporary descriptors. |
| T34 | Elaborate conversions and calls from the complete conversion table | **new** `src/semantic/capability_calls.{h,cpp}`; `src/semantic/check_calls.cpp`, `src/semantic/check_ufcs.cpp` | T33, T17, T25 | P0 | Bare owned arguments transfer without a witness; declared loan parameters borrow/reborrow, including observation/edit range parameters. Mutable-range value bindings move; explicit transfers preserve loan identity and dependencies. Expected types cannot strengthen observation slicing into editing; checked duplication, explicit independent copy/materialization, and explicit freeze remain distinct. An owner passed bare to `shared_read` is rejected; no implicit copy-then-freeze or materialization is inserted. Callable invocations supply implicit environment receivers and borrow/reborrow or consume according to checked entry modes; owning an editing callback still permits reusable calls. Incompatible annotations fail before code emission. |
| T35 | Elaborate branches, loops, matches, failures, and cleanup | **new** `src/semantic/capability_control.{h,cpp}`; `src/capability/control_flow.cpp` | T34, T19, T15, T27 | P0 | All exits, argument failures, result transfers, predecessor disposal, and consuming patterns have explicit core operations and valid schedules. Elaborate generator creation/resume/yield and for-loop completion, failure, and early-exit disposal under the checked suspension invariants. |
| T36 | Route refinements and erased checks through core facts and certificates | `src/semantic/check_reasoning.cpp`, `src/semantic/predicates.{h,cpp}`, `src/semantic/narrowing.{h,cpp}`, `src/semantic/try_from_proof.cpp`, `src/semantic/checker_results.h` | T35, T22, T23, T26 | P0 | Name-based or pre-effect proofs cannot authorize an access; runtime tests remain executable, and only checked evidence justifies erasure. |
| T37 | Version exported contracts and metadata | `src/module_metadata.proto`, `src/driver/module_metadata.{h,cpp}`, `src/semantic/check_lowering_view.cpp` | T32, T24 | P0 | Metadata carries the semantics version and portable contract/witness descriptions, including callable entry modes, formal/environment origin binders and captured-dependency relations, slot/range-origin, named-field footprints, range-partition relationships, scoped-dependency, and retained frozen-projection summaries; session-local pointers/IDs are not serialized as authority; incompatible inputs fail closed. Export generic dependency variables, trait stored-origin relationships, and generator yield/resume/disposal contracts without serializing private frame layout as authority. |
| T38 | Require verified bodies at every execution/lowering entry | `src/driver/driver.cpp`, `src/driver/lowering_stage.cpp`, `src/semantic/check_lowering_view.cpp`, `src/comptime_vm/program.cpp`, `src/testing/stdlib_fixtures.h` | T29, T36, T37 | P0 | Normal execution, native compilation, compile-time preparation, and experimental fixtures cannot execute unchecked capability bodies; unfinished recursive bodies use safe preparation states. |

T30 must select a minimal, capability-compatible bootstrap prelude for integration fixtures,
with checked scalar/core declarations and explicitly contracted primitives. Expand it in T47.
Do not inject the legacy standard library and then skip checking it to bootstrap the mode.

T25 applies declared interfaces, including recursive ones, as local checking assumptions.
T26 discharges those assumptions by checking every body in a recursive dependency group;
T29/T38 prevent any group with an unverified implementation from reaching execution.

Omitted effect, origin, and outcome clauses request sound, conservative inference from
checked source bodies, including public definitions. T26 must infer and verify complete
summaries and check optional programmer constraints; recursive candidates must be verified
for the whole group. T37 exports the checked interfaces, without requiring callers to inspect
bodies. Native/opaque interfaces require sufficient declarations or sound defaults.
T53 must explain missing information or conservative approximations, why they prevent the
requested use, and the smallest relevant annotation or legal code change. Request author
input only when inference and sound defaults cannot establish the required contract;
never infer copying, freezing, or new authority merely to make a contract fit.

T38 initially rejects experimental execution paths that lack T42–T45 support. It must not
fall back to legacy lowering to keep those paths running.

### Runtime, HIR, and execution

| ID | Task | Target File(s) | Depends On | Priority | Acceptance Criteria |
|---|---|---|---|---|---|
| T39 | Specify capability extensions to the completed value-storage ABI | `spec/affine-capability/runtime-abi.md`, `spec/affine-capability/decisions.md`; landed value-storage interfaces identified at V0 | T09, V0 | P0 | Q3–Q5 are resolved against actual typed places, layouts, parameter storage, and result destinations; region/provenance mappings and owned/frozen/loan/projection extensions reuse the aggregate ABI, alignment, and initialization contracts. |
| T40 | Extend storage owners with frozen retention and projection lifetimes | **new** `src/runtime/frozen_storage.{h,cpp}`, `src/runtime/frozen_storage_test.cpp`; `src/runtime/layout.{h,cpp}`, `src/runtime/allocator.{h,cpp}` | T39 | P1 | Inline owners are retained or safely relocated before persistent projections escape; outstanding loans and address stability are respected; retained ranges/subranges preserve visible bounds and backing identity after source release, including small ranges retaining whole allocations; final release waits for all handles/projections and scoped lifetime dependencies and never runs mutable cleanup or thaws storage. |
| T41 | Implement copy recipes, transfer commits, and failure cleanup on typed storage | **new** `src/runtime/capability_storage.{h,cpp}`, `src/runtime/capability_storage_test.cpp`; `src/intrinsics.h`, `src/runtime/allocator.cpp`; `src/hir/value_storage.h` | T40 | P1 | Witness-authorized recipes construct into fresh destinations and allocate only required backing storage; initialized-part cleanup reuses storage state; exchange/pop commit without callbacks or recoverable failure between removal and installation. |
| T42 | Adapt verified core to the shared value-storage pipeline | **new** `src/hir/lower_capability.{h,cpp}`, `src/hir/capability_check.{h,cpp}`; `src/hir/nodes.h`, `src/hir/lower.cpp`, `src/hir/drop_schedule.cpp`, `src/hir/reference_check.cpp`, `src/hir/value_storage.h`, `src/hir/value_lowering.{h,cpp}`, `src/hir/value_verify.{h,cpp}` | T38, T39, T41 | P1 | Checked copy/retain/release/transfer and cleanup reach shared destinations with required provenance; structural witnesses remain distinct from legacy `copy`; capability cleanup is the sole logical schedule, with no second legacy disposal pass; both verification gates apply. Map sequential-generator states and loans to stable suspension storage; preserve frame identity across handle moves and use checked state-specific disposal, including early loop exit. |
| T43 | Extend bytecode emission for prepared capability operations | `src/bytecode_compiler/compile.cpp`, `src/bytecode_compiler/compile_test.cpp`; `src/bytecode/opcodes.h`, `src/bytecode/vm.cpp`, `src/bytecode/vm_test.cpp` | T42 | P1 | VM emission consumes the shared verified storage decisions and existing aggregate ABI; private by-value parameters, caller result storage, projections, and failure paths preserve distinct copy/transfer semantics without aggregate shell allocations. |
| T44 | Extend LLVM emission and native linkage for capability operations | `src/llvm_codegen/codegen.cpp`, `src/llvm_codegen/codegen_test.cpp`, `src/llvm_codegen/aot_test.cpp`, `src/llvm_codegen/runtime.cpp`; `src/driver/aot.cpp` | T42 | P1 | LLVM/JIT/AOT consume the same prepared storage decisions and aggregate ABI as VM; native wrappers retain typed result owners through decoding; alignment, cleanup, and retained-interior projections agree across backends. |
| T45 | Extend compile-time storage preparation, marshalling, and caches | `src/comptime_vm/backend.cpp`, `src/comptime_vm/program.cpp`, `src/comptime_vm/values.cpp`; `src/semantic/check_lowering_view.cpp`; **new** `src/comptime_vm/capability_test.cpp` | T43, T38 | P1 | Static calls pass both verification gates and reuse typed host/result storage; owners survive decoding and cleanup; cached lowering carries capability provenance and agrees with final verified storage preparation. |
| T46 | Preserve capability semantics through RVO, NRVO, and storage optimization | `src/hir/inline.cpp`, `src/hir/tail_calls.cpp`, `src/hir/frame_budget.cpp`, `src/hir/value_lowering.{h,cpp}`, `src/hir/value_verify.{h,cpp}`, `src/hir/return_storage.{h,cpp}`, `src/hir/value_lowering_test.cpp`, `src/hir/return_storage_test.cpp`; **new** `src/hir/capability_check_test.cpp` | T43, T44 | P1 | Mandatory RVO works at low optimization with inlining disabled; shared NRVO remains enabled by default with safe inline-move fallback; both preserve copy independence, origin lifetimes, initialization, effects, and cleanup; final frame budgets and tail-call provenance checks remain enforced. |

### Standard library and compatibility

| ID | Task | Target File(s) | Depends On | Priority | Acceptance Criteria |
|---|---|---|---|---|---|
| T47 | Define the trusted memory boundary and experimental prelude | `src/std/mem.cn`, `src/std/intrinsics.cn`, `src/std/prelude.cn`; `src/intrinsics.h`; **new** `src/semantic/capability_intrinsics.{h,cpp}`; `src/driver/driver.cpp` | T38, T41, T43, T44 | P1 | Trusted operations have explicit authority/effect/failure contracts. The split_at builtin maps to the verified core partition operation and the selected bounds-failure policy; it cannot infer authority from addresses or bounds alone. The retained frozen-projection intrinsic requires existing persistent frozen authority, checked backing provenance, and valid initialized ranges/text boundaries; it establishes retention before exposing results. Raw pointers or scoped views cannot supply that authority. Experimental source selection is complete and does not exempt the library from checking. |
| T48 | Migrate list storage and noncopyable sequence operations | `src/std/list.cn`, `src/std/traits.index.cn`; **new fixtures** under `src/testdata/affine_std_test/` | T47 | P1 | Initialized prefix, reserve/growth, push transfer, pop, replacement, indexing, views, and reverse cleanup are checked; noncopyable resources are supported. Element borrowing returns `inspect[T]`/`edit[T]` with checked slot origins and needs no copy witness; audit indexing trait relationships accordingly. Range indexing preserves its view shape and observation default; mutable slicing is explicit. Range APIs preserve reborrow/transfer dependencies, validate bounds and text boundaries where applicable, and do not infer disjoint editing from subrange bounds. Exercise split_at on mutable ranges, including nested/empty partitions and surviving returned views; verify that direct edits of a sibling named field remain usable during iteration. Replace cell `get`/`set` with permitted loads/observations and checked replacement/cleanup. |
| T49 | Provide frozen sharing, `copy`, and `copy_owned` APIs | `src/std/shared.cn`, `src/std/string.cn`, `src/std/traits.conversion.cn`; **new fixtures** under `src/testdata/affine_std_test/` | T47 | P1 | `copy` preserves the source value type; `copy_owned` materializes represented contents in a fresh `own[T]`. Both preserve their sources and require the relevant witnesses. Neither is a `clone` alias. Sharing cannot expose mutable authority or mandatory unique destructors; freezing is explicit; inline and child projections outlive released parents. `shared_slice[T]`/`shared_str` expose ordinary library `.slice(range)` methods over the checked retained-projection intrinsic, including retained subranges and UTF-8 checks. Range indexing stays scoped; handle duplication needs no element-copy witness. No universal `retain_range` builtin or new expression syntax is introduced. |
| T50 | Migrate consuming sums, iterators, generators, callbacks, and generic helpers | `src/std/option.cn`, `src/std/result.cn`, `src/std/iter.cn`, `src/std/algo.cn`, `src/std/traits.cn`, `src/std/deriving.cn`; **new fixtures** under `src/testdata/affine_std_test/` | T48, T49, T32, T42 | P1 | Resource payloads move once; iterator/callback contracts expose retained loans and writes; generic helpers use owning or borrowing modes as intended, requiring structural `copy` constraints only for explicit independent copying/materialization. Audit legacy `copy` bounds by body operations; remove unnecessary bounds for transfer or replace implicit copies with explicit checked operations or loans. Preserve named-field and split-result footprints through iterator/callback contracts rather than requiring whole-receiver loans unnecessarily. Migrate sequential generators and for loops over borrowed data; two external-backed iterator results remain live through trait dispatch when effects permit, and early exit disposes suspended state. |
| T51 | Migrate sequential external-resource APIs to sealed contracts | `src/std/io.cn`, `src/std/console.cn`, `src/std/fs/path.cn`, `src/std/platform.cn`, `src/std/time.cn`; `src/runtime/io.cpp`, `src/runtime/platform.cpp` | T47, T28 | P1 | Native resources are not automatically copyable/freezable; transitions, cleanup, failures, and external effects have explicit interfaces. |
| T52 | Implement the legacy/machine/concurrency boundary policy | `src/semantic/check_expr.cpp`, `src/semantic/check_ufcs.cpp`, `src/driver/driver.cpp`, `src/std/sync.cn`; `spec/affine-capability/compatibility.md` | T38, T47 | P1 | Unsupported crossings produce useful errors; approved adapters are tested; async, asynchronous cancellation, and thread safety are not inferred from sequential checking or frozen sharing alone. Do not classify checked sequential yield or synchronous suspended-generator disposal as excluded cancellation; reject unsupported async/concurrent crossings explicitly. |

T47 may select a separate experimental prelude or versioned modules, depending on Q6. If new
module files are necessary, record their exact paths in `decisions.md` before adding them.
Do not duplicate the entire library permanently. T51 covers the supported sequential API
subset; APIs whose implementation requires asynchronous cancellation or concurrent mutation
follow T52. Sequential generator disposal belongs to T27/T42/T50.

### Validation, migration, and adoption

| ID | Task | Target File(s) | Depends On | Priority | Acceptance Criteria |
|---|---|---|---|---|---|
| T53 | Add actionable capability diagnostics and snapshots | **new** `src/semantic/capability_diagnostics.{h,cpp}`, `src/semantic/capability_diagnostics_test.cpp`; **new fixtures** under `src/testdata/affine_diagnostics/` | T38 | P1 | Diagnostics identify the denied operation, loan origin, surviving carrier, invalidating effect/version, or missing cleanup/witness and give a legal remedy. |
| T54 | Complete proposal-to-core and proposal-to-source conformance tests | **new** `src/semantic/capability_check_test.cpp`; `src/capability/verify_test.cpp`; **new fixtures** under `src/testdata/affine_check_test/`; `spec/affine-capability/requirements.md` | T29, T38 | P1 | Every proposal matrix row and prose counterexample has executable acceptance/rejection coverage with stable requirement IDs. |
| T55 | Add VM/LLVM/compile-time differential execution coverage | **new** `src/capability_execution_test.cpp`; **new fixtures** under `src/testdata/affine_execution_test/`; `src/BUILD.bazel` | T43, T44, T45, T48, T49, T50, T51, T54 | P1 | Shared cases use the verified pipeline and compare values, observable cleanup, effects, and retained lifetimes across supported execution modes. Compare sequential generator yield/resume, handle moves, early loop exit, and state-specific disposal, plus branch-edge destructor traces. Execute field-disjoint iteration and nested/empty range splits, including returned child views, bounds failure, and backing lifetime checks. |
| T56 | Add bounded model comparison and adversarial proof/state tests | `src/capability/model_test.cpp`, **new** `src/proof/adversarial_test.cpp`; `spec/affine-capability/validation.md` | T29, T21 | P1 | Generated small heaps/traces and malformed certificates exercise invariants, alpha-renaming, forged partition evidence, branch/loop partition identity confusion, rejected transitions, and resource-limit failure without accepting unsafe states. |
| T57 | Validate reclamation and modeled failures with sanitizers | `src/runtime/capability_storage_test.cpp`, `src/runtime/frozen_storage_test.cpp`; `src/capability_execution_test.cpp`; `.bazelrc` | T55, T41 | P1 | Normal/error exits, partial construction, projection retention, and commit failures have exact cleanup counts and no invalid accesses/double frees under supported ASan/UBSan configurations. Cover suspended generator disposal and external/frame-backed yielded lifetimes, with exact cleanup counts on early exits and modeled failures. |
| T58 | Record compile-time, copy, and retention performance budgets | **new** `src/capability_benchmark.cpp`; `src/BUILD.bazel`; `spec/affine-capability/performance.md` | T55, T46 | P1 | Representative and adversarial workloads have baselines, agreed limits, allocation/copy counts, and documented tradeoffs; timeouts never change acceptance rules. |
| T59 | Publish migration examples and candidate normative documentation | `spec/cinder-grammar.ebnf`; affected chapters under `spec/specification/`; `README.md`; `spec/affine-capability/migration.md` | T50, T51, T52, T53 | P1 | Documentation distinguishes legacy, experimental, and proposed default behavior; examples run; source edits needed for ownership, copy cost, sharing, and cleanup are explicit. |
| T60 | Establish adoption readiness and resolve compatibility blockers | `spec/affine-capability/adoption.md`, `spec/affine-capability/requirements.md`, `spec/affine-capability/compatibility.md`, `spec/affine-capability/validation.md` | T55, T56, T57, T58, T59 | P1 | All required evidence is complete, trusted assumptions are listed, Q7/Q8 are resolved for the advertised scope, and release/rollback criteria are concrete. |
| T61 | Enable the approved default and migrate release entry points | `src/driver/cli.cpp`, `src/driver/driver.cpp`, `.justfile`, `README.md`, `src/cli_test.cpp`, `src/std_test.cpp` | T60 | P1 | The selected release mode, stdlib, metadata, examples, package/install flows, and default CLI behavior agree; the agreed compatibility window remains explicit. |
| T62 | Remove superseded paths after the compatibility window | `src/semantic/ownership_check.cpp`, `src/semantic/ownership_cfg.cpp`, `src/hir/drop_schedule.cpp`, affected `src/hir/BUILD.bazel` files; `spec/affine-capability/adoption.md` | T61 | P1 | Only paths proven redundant for all supported modes are removed; out-of-core features retain a defined implementation or are explicitly unavailable. |
| T63 | Reconcile final references, evidence, and repository graph | `spec/affine-capability-core.md`, this plan, `spec/todo.md`, `spec/specification/05-implementation/`, `graft/` | T62 | P1 | Actual implementation status and limitations match evidence; `graft build` refreshes the graph after the code changes; no proposal is silently relabeled as proved or implemented. |

## Implementation Notes / Builder Guidance

### 1. Preserve the separation between availability and duplicability

An inspection is duplicable, but its binding can become unavailable. Every lookup checks both
binding availability and its live carrier. A move transfers nested dependencies and cleanup
obligations atomically; it does not create a second authority token. Scalar snapshots carry
no loan dependency after the checked load, while an existing shared handle receives its own
retention obligation. A structural summary of an aggregate is not an extra runtime copy.

An active loan may depend on another loan even after the granting handle's last direct use.
`end_loan` must inspect both live carriers and dependent loans. Moving a suspended edit keeps
its identity, granting chain, and suspension. Ending one reborrow restores only permissions
not blocked by any other remaining loan.

### 2. Check forest transitions before changing the forest

For ancestor inspection versus descendant mutation, overlap is transitive ownership overlap.
Checked distinct named-field paths refine that overlap within one allocation; equal and
prefix paths still overlap. Separately allocated owned siblings and checked split partitions
can be disjoint, but unknown abstract regions cannot be declared disjoint merely because
they have different names. Stored aliases retain resolved target provenance. Whole-parent
transitions cover every affected field and descendant.

Replacing an owned child checks the containing slot, incoming owner, outgoing owner, and all
surviving lifetime dependencies against the pre-transition forest. Then validate the new
forest. Do not detach an edge first and thereby hide the conflicting loan. A child-targeted
edit does not authorize replacing its owner's slot in the parent.

First-class slot loans use `inspect[T]`/`edit[T]` instead of a separate cell type. T02–T04
must model and validate containing-allocation/path provenance, permitted replacement, and
dependent lifetimes before implementation. T12/T16 preserve that evidence through returned
or stored views; T31/T33 resolve nested slot types and elaborate `*slot` as a checked place.
`edit[own[T]]` of an element permits exchange of its stored owner, while `edit[T]` of the
child does not. Slot selection never authorizes an empty borrowed element; different inline
indices alone do not establish disjoint edits. Inherited split partitions can separate them. Live views block relocation, removal, destruction, and other
invalidating collection actions. T37 exports portable origin summaries, not raw addresses.
The [slot translation](affine-capability/translations.md#worked-example-a-borrowed-collection-slot)
exercises generic replacement with no copy bound and exact cleanup ownership.

Scoped ranges use the same dependency model, with checked bounds and range shape. T02–T04
must validate observation/edit subrange reborrows, affine range transfer, and suspension
through returned or stored results. T31/T33/T34 preserve observation-default slicing,
explicit mutable slicing, and range-parameter reborrows without borrowing temporary
descriptors. T12/T16 preserve backing provenance and reject separation inferred only from
nonoverlapping subrange bounds. The checked split_at primitive establishes affine child
partitions, including nested and empty splits, after bounds validation. Returned/stored
descendants preserve partition ancestry and keep conflicting parent authority suspended. Text slicing additionally checks UTF-8 boundaries.
T37 must export range origin dependencies and T48 must validate the migrated collection
APIs. See the [range translation](affine-capability/translations.md#worked-example-range-views-and-reborrowing).
Retained frozen ranges use `shared_slice[T]` and `shared_str`, created by ordinary library
`.slice(range)` methods over an explicitly contracted retained-projection intrinsic. T02–T04
and T16 must validate independent retention, frozen backing identity, visible subrange bounds,
and source preservation without element copying or scoped-loan promotion. T31/T37 preserve
the distinct types and portable provenance/retention summaries; T40/T42 preserve retention and
release through runtime lowering. T47 specifies the intrinsic boundary, requiring existing
persistent frozen authority and established range/text validity; raw pointers alone do not
qualify. T49 implements and checks the public collection/text wrappers and teaching diagnostics
for returning a loan through a local source handle. T54/T55 cover parent release, retained
subranges, UTF-8 boundaries, and final release without thawing or mutable cleanup. A tiny range
may retain its whole backing allocation. This introduces no universal `retain_range` builtin
and settles neither Q4's concrete ABI nor Q5's failure policy. See the
[retained-range translation](affine-capability/translations.md#worked-example-retained-frozen-ranges).

Callable types accept optional parameter names and checked result-origin bounds such as
`fn(part: slice[int32]) -> slice[int32] from part`. T08/T30/T31 preserve the unnamed forms,
scope the contract binders, and treat consistent renaming as equivalent; implementation
parameter names need not match. T24–T27 instantiate origins separately per invocation and
reject capture-backed or earlier-call dependencies outside an input-only bound. T25/T34
transfer all dependencies through forwarding results before argument retirement. T31/T37
preserve sufficient checked callable summaries through values and metadata; omission cannot
assert an input-only promise or erase dependencies. T53 explains incompatible captures or
missing interface information and the smallest legal change. T54/T55 cover accepted
input-derived views and computation-only captures, rejected unrelated capture-backed results,
different-source invocations, and forwarding/storing callables. Environment-derived results
also use the accepted capture-origin forms below. See the
[callback translation](affine-capability/translations.md#worked-example-callback-result-origins).

Write constraints use `writes nothing` for no writes to pre-existing storage, or nonempty
root sets such as `writes {count}`. T08/T30 reject `writes {}` and suggest the explicit
no-write phrase or omission for inference. T24/T31 scope `self` to a callable type's
contract environment; T25–T27 follow captured capabilities to their target regions and
instantiate enclosing bounds such as `writes {choose}` without exposing private captures.
T37 preserves those abstract footprints in metadata. T53–T55 distinguish omitted constraints
from checked prohibitions and cover explicit/captured counters, helper/cleanup writes, and
input-only results that do not retain the counter loan. Write bounds do not grant authority
or determine environment call modes. `writes nothing` permits fresh allocation and declared
transitions; complete transition, output, and cleanup summaries remain required. See the
[counter callback translation](affine-capability/translations.md#worked-example-callback-write-bounds).

Callable types accept optional leading environment receivers `fn(self, ...)`,
`fn(mut self, ...)`, and `fn(self: own[Self], ...)`. T08/T30 preserve them separately from
ordinary callable parameters, including implicit invocation syntax. T27/T31 infer source
lambda receiver authority and check optional constraints; omission retains sufficient
checked or sound conservative mode information rather than promising observation. T25/T34
separate a function's ownership or borrowing of a callback from the environment authority
needed for each invocation. T27 checks captured edit reborrows and captured owner moves;
T31/T37 preserve modes through stored/forwarded callables and portable summaries without
exposing capture layout. T53 explains the capture use requiring stronger access and the
smallest legal interface or ownership change. T54/T55 cover repeated editing calls with
live input-derived results, captured loans that outlive individual calls, owned editing
callbacks remaining reusable, rejected editing through observation and consumption through
loans, and independence of modes from writes. See the
[call-mode translation](affine-capability/translations.md#worked-example-callable-receivers).

Capture lists use `[x]` for value capture, `[&x]` for observation, `[&mut x]` for editing
or reborrowing, and `[]` for no captures. T08/T30 preserve the distinction between an
explicit closed list and no list; T31/T33 infer required variables for an omitted list
and apply value-capture rules in both cases. Affine owners and edit handles move;
duplicable values retain their checked dependencies. Expected callable constraints and
escape analysis must not silently choose borrowing. T27/T33 check explicit capture loans,
escaping lifetimes, and cleanup obligations.

T24/T31 bind result `from self` to the callable environment and support mixed bounds such
as `from {part, self}`. T25–T27 preserve the difference between borrowing captured ownership
and returning a duplicate of an already captured observation. The former retains supporting
ownership dependencies; the latter can retain only its original target/grant/lifetime
relationships when sufficient checked precision is available. T31/T37 preserve those
relations through stored/forwarded callables and portable metadata without exposing private
capture layout. Less precise interfaces may retain additional dependencies conservatively,
never discard actual ones. T53 explains a conflicting source use or the relationship a
boundary needs to establish. T54/T55 cover all capture forms and implicit defaults, source
availability after capture, captured-owner views, external-source observation captures,
duplicated captured views that need not retain the closure, conservative interfaces, mixed
origins, and rejection of views whose backing is destroyed by invocation cleanup. See the
[capture translation](affine-capability/translations.md#worked-example-capture-origins).

Shared edges do not extend exclusive ownership footprints. They still create independent
retention and scoped-origin obligations. A source shared handle needed by an inspection
cannot be released simply because another unrelated shared handle keeps the allocation alive.

### 3. Keep copy witnesses separate from duplication and ordinary trait methods

Move-by-default is the surface policy; every selected operation is explicit in the core.
Ordinary transfer requires no copy witness and preserves the incoming storage identity.
Scalars, retained frozen handles, and other duplicable values follow their checked duplication
rules, including lifetime dependencies. A structural copy witness authorizes explicit
independent construction; it does not make an owner duplicable or trigger implicit copying.

Use `copy` as the public structural constraint in the experimental mode. Retire the legacy
concept's implicit-duplication meaning there; keep permitted duplication as a separate
compiler-tracked property without introducing public `copyable` or `duplicate` constraints.
T32 resolves the new constraint to checked witness evidence. T50 audits legacy generic
bounds according to the operations they require; a shared spelling is no compatibility
guarantee. Legacy mode keeps its current meaning until adoption. In particular, an owner
can satisfy `copy` and still move by default, while an inspection can duplicate without
satisfying `copy`.

Expose core `copy_value` as source-level `copy(x)` and use `copy_owned(h)` for fresh-owner
materialization in both surface and core. T34/T49 must preserve the source-observation
semantics of both operations rather than applying ordinary move-by-default to the source.
`copy` on a frozen handle retains its target; `copy_owned` through that handle requires a
witness for represented `T`. A scoped handle can support observation of a materializable
referent without having its own independent value-copy witness. Legacy `clone` methods
remain ordinary calls, not implicit implementations of either operation.

The initial witness derives from representation structure and checked component operations.
It establishes `equiv_T` and that every mutable region reachable from the result is fresh
relative to the entire input heap. Freshness of only the root, or disjointness from only the
source, is insufficient. Copying must preserve source contents, versions, relationships,
and authority, including on modeled failures.

Derive recursive recipes by nominal recursive structure with guarded runtime traversal of
finite values; do not infinitely unfold recursive types while checking. Preserve the exact
observable scalar representation, active variants, order, length, closure code entries, and
every private represented field. Custom equality, cache omission, and arbitrary clone methods
do not establish a witness. Scoped components and protocol authority block derivation of an
independent copy witness, even when separate capability duplication rules permit duplication.

For a frozen represented owner field, `copy_owned` constructs a fresh owned child. A field declared
as a shared handle retains its existing frozen target. Copying an aggregate containing proof
data cannot silently retarget propositions to new regions or versions. Reject the recipe
unless the needed derivation is checked.

### 4. Treat cleanup as a first-class checked operation

Track `empty`, `initialized`, and private `cleaning` slot states independently of region state.
Whole-value operations require complete initialization. `take_field` is owner-only and limited
to permitted structural products; custom whole-value cleanup prevents ordinary partial moves.

Structural disposal uses reverse field/capture/index order. A custom unique recipe has private
teardown authority and must discharge every obligation itself or through explicit helpers.
Do not retain the current unconditional custom-drop-plus-field-drop behavior in the new mode.
Freeze requires a separate structural frozen release recipe and rejects mandatory custom
unique destruction. Reference-count bookkeeping grants no Cinder edit authority.

Replacement evaluates its incoming value before disposing the old one. Keep the replacement's
carriers alive through disposal. Reject an incoming value that borrows storage about to be
destroyed. `exchange` returns the outgoing value instead of destroying it; `remove_last`
transfers the final element without running its destructor. Prepare fallible result storage
before their indivisible commit. No callback or recoverable failure runs inside that commit.

At joins, explicitly clean still-initialized predecessor values that will be unavailable, or
initialize a compatible common result. Predecessor disposal is a specified cleanup point,
including observable destructors before lexical scope exit on the nonmoving branch. It is
not last-use retirement or an optimization. Apply cleanup effects before merging facts. Do not hide
obligations behind an unavailable binding or use runtime drop flags as a substitute for the
core's required normalization. A loop back edge must establish the same kind of invariant.

### 5. Order argument conversion, contracts, and outcome selection precisely

The ordered call sequence is:

1. Evaluate and convert arguments left to right, retaining all completed argument carriers.
   Apply transfers, checked duplication, and loans as selected by the parameter mode.
   Explicit copy/materialization or freeze requests may allocate or fail according to their
   checked operations; do not insert those operations just because a parameter type differs.
2. If conversion propagates an error, clean evaluated temporaries on the caller's failure
   path and preserve effects already performed. The uninvoked callee contributes no effects.
3. Instantiate regions and verify preconditions against the resulting input state.
4. Apply the callee's write/transition bound and each possible output-state description.
5. Install results and retained/stored loan dependencies, with checked postconditions over
   output identities and versions, before retiring argument carriers.
6. Apply caller-side cleanup and all subsequent operations to every remaining outcome state.
   Matching an outcome selects the current alternative; it does not restore the state at
   the instant the call returned.

An unannotated inspection-to-inspection conversion duplicates the same loan. Explicit `&`
creates a dependent observation loan; explicit `&mut` creates an edit loan or edit reborrow.
Never strengthen `&` from an expected edit type or silently reinterpret an explicit edit
request as observation. An edit passed to an edit helper normally creates a
reborrow; `&>` transfers the original handle and invalidates its source. The original edit
becomes usable again only after all retained descendants of the reborrow are gone.

An unannotated owner passed to an owning parameter transfers and invalidates its source;
`&>` may state that transfer explicitly. This works for uncopyable resources and generic
values without a copy bound. An explicit independent copy/materialization preserves the
source under its witness obligations. Passing an owner bare to `shared_read` is rejected;
`&!` consumes and freezes the original, while `&!copy_owned(x)` explicitly materializes
and freezes a copy. Neither inspection nor edit can be promoted to persistent sharing or ownership.
Every conversion-table row and every incompatible annotation needs a source-level test.

### 6. Couple effect checking to storage versions and outcomes

The invalidation set includes the pre-call may-write footprint, changed transition regions,
and owning ancestors in the input and every possible output forest. Include detached old
parents, newly attached descendants, callbacks, hidden token targets, closure captures, and
cleanup effects. Transferring a handle does not by itself mutate its region contents.

Infer omitted source-level write bounds from checked bodies; omission alone never establishes
an empty bound. Without a narrower checked summary, use the conservative permitted mutable
footprint. Request the missing information for interfaces that cannot bound this footprint.
An explicit `writes nothing` bound still permits declared transitions, which may invalidate
facts or availability. Mutating a fresh independent copy does not invalidate the original
storage's facts.

Body verification checks postconditions after callee cleanup on every corresponding return.
Error variants are ordinary outcomes and imply no rollback. Before discrimination, protect
all possibly retained loans and expose only common authority. Merging or discarding results
must normalize their obligations; never simply discard inconvenient alternatives.

Bound alternative-state growth soundly. Compatible states may merge by conservative summaries;
incompatible states require an explicit invariant or fail with a checking-limit diagnostic.
Do not prune a possible error outcome to meet a budget.

### 7. Keep proof checking small and independent

Kernel constructors cover assumptions, equality rules, congruence, conjunction, implication,
nominal witnesses, and checked linear certificates. Validate acyclicity and assumption scope
before traversing a submitted proof. Use exact integers for normalization and coefficients.
Arbitrary-precision means no fixed-width wraparound, not unlimited resource consumption.

Storage-dependent terms carry stable region/version IDs and dependency sets through every
derivation. Loan-scoped evidence expires with its loan. Immutable snapshot facts may remain,
but authorize no later access to changed storage. Equality transport preserves authority and
region identity. A proof of a capability proposition cannot duplicate the capability token.

Existing solver automation may remain an untrusted certificate producer. Incompleteness is
acceptable. Kernel rejection or resource exhaustion cannot be converted to proof success.
Keep required runtime tests after evidence erasure, and ensure disabling ordinary contract
assertions cannot disable capability or proof checks required for memory safety.

### 8. Freeze changes access, not nominal representation

Owned edges reached through frozen storage denote retained frozen children, never extractable
owners. An inspection of an owned child remains scoped even if the target happens to be
frozen. Loading a field already declared as a shared handle can independently retain it.
Persistent projections into inline fields retain the whole backing allocation and do not
run an inline-field destructor on release.

Freezing a closure checks both its captures and its callable entry. Code checked with edit or
consuming environment access cannot be reinterpreted as a read-only entry. A shared-callable
body must remain valid under shared capture projections.

The initial model has acyclic exclusive ownership. The runtime ABI decision must also state
what retained frozen graphs can be constructed and whether their construction preserves
acyclicity. Do not promise cycle collection or infer it from the absence of mutable cycles.

### 9. Integrate with the existing compiler without bypasses

Prefer focused new elaboration files to more capability-specific branches throughout the
large `check*.cpp` and `lower.cpp` implementations. Reuse nominal type resolution, source
spans, module identity, diagnostics, and generic substitution as adapters. Do not turn the
current ownership CFG into an independently maintained second authority for new-mode drops.

Use staged preparation states for recursive and compile-time functions: an interface may be
available before its body is ready, but an executable body must be verified. Include the
semantics version and relevant contracts/witnesses in incremental caches and fingerprints.
Compile-time host marshalling cannot fabricate retained handles or let host memory serve as
an escaping scoped origin without a checked lifetime.

Metadata IDs must be portable declaration identities or binders, not AST addresses. Check
contract compatibility at imported, indirect, generic, and trait-dispatched calls. If the
current driver only emits metadata, do not invent a separate-compilation loader solely for
this plan; version the emitted schema and enforce boundaries on actual loading paths.

Backend test helpers that call `check_program` and lower directly must acquire a verified
capability program. Direct core-unit tests may deliberately build invalid IR, but invalid
IR must never be executed as a positive backend conformance test.

### 10. Extend the completed value-storage pipeline

T39 specifies capability extensions to the V0 baseline. Reuse its concrete layouts, typed
places, private by-value parameter storage, caller-owned result destinations, host/native
wrappers, and storage verifier. Semantic regions track storage identity and authority without
equating them to allocation calls or physical addresses. Inline fields share the containing
region; genuine collection/closure backing allocations retain their distinct storage owners.
Physical slot reuse never reuses a semantic identity.

T42 translates witness-authorized structural copies into checked construction recipes over
the existing destination operations. The legacy `copy` classification alone cannot authorize
these recipes, and a witness must not silently change that classification in ordinary mode.
Byte copying implements a transfer or a permitted leaf copy, not arbitrary deep copying.
Construct inline roots directly in fresh destinations; allocate backing storage only where
the represented ownership structure requires it. Preserve source availability, contents, and
fresh mutable reach even when eliding temporary storage. Keep initialization state and logical
ownership distinct from the bytes used to implement them.

Capability cleanup supplies the logical schedule, including predecessor-edge disposal,
state-specific synchronous generator disposal, reverse structural order, and custom recipes
that discharge their own obligations. Shared storage lowering realizes that
schedule through initialized-place cleanup and partial-construction state. It must not append
legacy field drops, recompute semantic joins, or independently decide that an owner is dead.
Retain origin and cleanup metadata needed by storage verification and optimization checks.

Reuse alignment, packed access, zero-sized value, projection-offset, and address-stability
contracts. Frozen inline projections may require preserving or relocating an owner into
retained storage before exposure. Check outstanding loans and address observability before
relocation, and keep the owner alive until the last retained projection is released. A pointer
into a stack frame cannot become persistent merely by incrementing a count stored elsewhere.
This capability-specific retention allocation does not justify boxing ordinary aggregates.

Mandatory RVO remains ordinary destination-directed construction, including nested results,
branches, call chains, and error propagation, with optimization and inlining disabled.
Shared NRVO remains enabled by default for eligible locals; capability origins and observable
cleanup may narrow eligibility, with an ordinary inline move as fallback. Neither RVO nor
NRVO permits reusing a still-observable source that must remain independently available after
a structural copy. Replacement such as `x = make(&x)` prepares a separate result before
disposing the old value. Frame-range reuse and tail calls must respect surviving loans,
pending cleanup, result ownership, and final frame budgets.

T46 tests these conditions against the shared prepared storage form and emitted code on both
backends. Disabling an unsafe optional transformation is valid; dropping mandatory RVO or
reintroducing aggregate shell allocations is not the fallback. Extend the shared pipeline
where capability constraints require additional provenance rather than adding a parallel ABI.

## Validation Matrix and Test Strategy

T01 assigns an ID to every row of the source proposal's example tables and every additional
prose requirement. Keep a ledger with: source heading/row, expected result, core fixture,
source fixture, runtime fixture where meaningful, implementing task, and evidence status.
The following groups define minimum coverage; they do not replace row-by-row traceability.

| Family | Required distinctions | Primary tasks |
|---|---|---|
| Forest conflict | Parent/child/grandchild overlap, disjoint allocated siblings and named fields, equal/prefix overlap, whole-receiver conflicts, stored aliases, pre-detach checks, unknown aliasing, shared-origin lifetime | T11–T12, T54 |
| Carriers | Duplicated observations, captured/stored loans, moved containers, in-flight arguments, returned views, temporary origins, retired bindings, scalar snapshots, branch availability | T12, T18–T19, T25, T54 |
| Projection/freeze | Frozen owned child cannot yield ownership/edit; retained child/inline projection survives parent release; scoped access cannot promote; mutating closures and custom unique cleanup cannot freeze | T16, T27, T40, T49 |
| Slot views | Cell-to-inspect/edit mapping; checked slot versus child provenance; element and range result shapes; generic exchange without copy bounds; returned/stored dependencies; relocation/removal/destruction conflicts; owner-load and empty-slot rejection; named-field and inherited partition separation; no separation from indices alone; replacement validity and cleanup | T12–T16, T31, T33, T37, T48, T54 |
| Range views | Observation-default and explicit mutable slicing; direct range capabilities; bounds, initialization, and UTF-8 boundaries; observation/edit parameter reborrows; mutable binding moves and explicit transfer; returned/stored subrange dependencies; owner-slot element authority; checked split_at bounds and empty/nested partitions; returned split descendants and parent suspension; no separation from bounds alone or implicit persistent frozen ranges | T12, T16, T31, T33–T34, T37, T48, T54 |
| Conversion | Every conversion-table row, bare owner transfer with and without a copy witness, use after transfer, scalar/frozen duplication, rejection of implicit freeze/materialization, explicit-annotation mismatches, same-loan duplication versus fresh reborrow, repeated helpers, suspended-edit transfer, retained loans on error | T34, T54 |
| Retained frozen ranges | `shared_slice[T]`/`shared_str` and ordinary library `.slice()` over a contracted intrinsic; return/store after source release; retained subranges stay within visible bounds; frozen backing identity and whole-allocation retention; no element copying or element-copy witness for handle duplication; scoped indexing remains dependent; reject scoped/raw-pointer promotion and invalid bounds/text boundaries; final frozen release exactly once | T16, T31, T37, T40, T47, T49, T54–T55 |
| Copy/materialization | Structural `copy` constraint resolution and rejection of legacy duplication as witness evidence; owner satisfying `copy` still moves, duplicable inspection fails `copy`; explicit `copy`/`copy_owned` requests; same-type handle copy versus represented-content materialization; scoped-handle copy rejection versus supported referent materialization; witness availability never enables implicit copying or owner duplication; exact contents and fresh mutable descendants, unrelated old mutable child rejection, independent frozen retention, source preservation, scoped/token rejection, frozen-source materialization, proof identity substitution | T17, T27–T28, T32, T34, T41, T49–T50 |
| Generic dependencies | Check once with opaque dependency variables; substitute `list[str]`, `option[&T]`, nested containers and iterator results; preserve origins through move/extraction/replacement/drop; reject backing destruction while any nested view survives | T02–T05, T12, T24–T25, T32, T37, T54 |
| Receiver-stored origins | Two live external-backed results through trait dispatch and metadata; distinguish receiver/frame-owned loans from carried external loans and dependent reborrows; reject conflicting mutation, resume, or disposal without exposing private fields | T04–T05, T08, T24–T27, T32, T37, T50, T54–T55 |
| Sequential generators | Loans across yield; intervening caller actions; external-backed versus frame-backed results; stable frame identity on handle moves; rejection of reentrant/conflicting resume and dead backing; state-specific cleanup on completion, failure, and early loop exit/drop | T02–T08, T27, T35, T42–T45, T50, T54–T57 |
| Initialization/cleanup | Partial products, reinitialization, forbidden partial custom destruction, replacement borrowing old contents, branch-edge disposal with observable destructor timing before the join and surviving-loan rejection, partial-construction failure, specified order, exactly-once accounting | T13–T15, T19, T57 |
| Exchange/pop/match | Noncopyable resources, initialized prefix, empty pop, slot versus child authority, outgoing loans, prepared-result failure, consuming sum payload exactly once | T14, T41, T48, T50 |
| Callback origins | Named/unnamed callable parameters; consistent binder renaming and different implementation names; inferred implementation/forwarding contracts; per-invocation origins across different inputs; preserved summaries through stored values and metadata; compatible computation-only captures versus rejected capture-backed or earlier-input results; omission never erases dependencies | T24–T27, T30–T31, T34, T37, T53–T55 |
| Captures and origins | Explicit value/observation/edit/no-capture lists; omitted list infers value captures; affine moves versus checked duplication with dependencies; no implicit copying or escape-dependent borrowing; closed-list enforcement; escaping borrowed lifetimes; `from self` and mixed bounds; captured-owner views retain supporting ownership; duplicated captured observations preserve original dependencies without unnecessary environment loans; conservative boundary summaries; reject results whose backing invocation cleanup destroys | T24–T27, T30–T31, T33–T34, T37, T53–T55 |
| Callable receivers | Inferred and explicit observing/editing/consuming entries; omitted receiver retains sufficient mode information; implicit call-site receivers; callback passing mode separate from entry mode; reusable owned/borrowed editing callbacks; earlier input-derived results do not retain environment loans; retained capture loans; reject editing through observation and consuming through loans; write bounds independent of modes; portable mode summaries and teacher diagnostics | T25–T27, T30–T31, T34, T37, T53–T55 |
| Effects/facts | Ancestor invalidation, disjoint preservation, `writes nothing` versus omission, rejected `writes {}` syntax, named argument and callable-environment write bounds, hidden callback/token writes, earlier argument proof invalidated later, cleanup invalidating postconditions | T23–T26, T36 |
| Outcomes | Error states retaining loans/replacing owners, no implicit rollback, no success-only authority before match, later effects on all alternatives, conversion failure before invocation | T25, T35, T54–T55 |
| Proof kernel | All constructors, scoped assumption discharge, huge coefficients, negative certificate weights, malformed/cyclic proofs, overflow domains, stale versions, region equality, resource limits | T20–T22, T56 |
| Abstractions | Generic nominal identity, private fields, trait/indirect subtyping, recursive summaries, copy/freeze closure contracts, sealed constructors, native trust boundaries | T24–T28, T32, T37, T47 |
| Execution/adoption | Both backends, compile-time preparation, metadata mismatch, optimization on/off, unsupported machine/concurrency crossings, CLI and package selection | T38, T43–T46, T52, T55, T61 |
| Value-storage foundation | Inline layouts and descriptors, independent parameter storage, caller-owned results, mandatory RVO without inlining, NRVO enabled/disabled and fallback, replacement aliasing, partial initialization, bounded frames, safe tail calls, no ordinary aggregate shell allocations | V0, T39–T46, T55–T58 |

Use three complementary levels:

1. **Core verifier tests:** build small core programs directly and assert accepted states or
   precise rejection reasons. Compare bounded generated traces with the formal model. Do not
   share the verifier's conflict routine with the reference model used as the test oracle.
2. **Source conformance tests:** compile complete programs through the mode's real semantic
   path, including library contracts, generics, diagnostics, and compile-time calls. A missing
   syntax implementation must not masquerade as the intended ownership rejection.
3. **Execution tests:** run accepted cases through VM and LLVM/JIT/AOT; use compile-time VM
   execution for the subset with permitted compile-time effects. Compare values, externally
   visible cleanup traces, copy independence, and live-allocation accounting, not just exit
   status. Do not demand identical optimization-dependent allocation counts between backends.

For lifetime tests use the reclaiming allocator, not only arena mode, since an arena can mask
use-after-release. Add failure injection at each modeled allocation/preparation boundary;
confirm source preservation, cleanup order, and the absence of half-committed exchange/pop.
Fatal process failures have separately documented expectations and do not count as completed
recoverable exits. Test long recursive values without assuming recursive host calls are safe
for unbounded depth; explicit traversal stacks or documented limits may be necessary.

Retain the value-semantics T-13 regression suite in ordinary mode and add capability-mode
counterparts for the supported subset. T55–T58 compare against V0 evidence: exact cleanup
traces and source independence, zero ordinary aggregate/descriptor shell allocations,
mandatory RVO with inlining disabled, and equivalent behavior with NRVO enabled/disabled.
Inspect shared storage plans and emitted code to establish eliminated buffers/transfers;
allocation counts alone do not prove RVO. Account separately for necessary structural-copy
backing allocations and frozen retention blocks. T52's exclusions do not remove ordinary-mode
coverage for captures, coroutine results, cancellation, or concurrency from the foundation.
Sequential generators also require capability-mode coverage; ordinary-mode coroutine tests
do not substitute for checking their suspension invariants and disposal contracts.

Existing targeted commands, verified from the build declarations, include:

```sh
bazelisk test --config=debug //src/parser:parser_test
bazelisk test --config=debug //src/semantic:ownership_check_test //src/semantic:reason_test
bazelisk test --config=debug //src/hir:lower_test //src/hir:inline_test
bazelisk test --config=debug //src/bytecode_compiler:compile_test //src/bytecode:vm_test
bazelisk test --config=debug //src/llvm_codegen:codegen_test //src/llvm_codegen:aot_test
bazelisk test --config=debug //src/runtime:allocator_test //src/runtime:layout_test
bazelisk test --config=debug //src:cli_test //src:std_test //src:codegen_stress_test
just test
```

Add focused targets such as `//src/capability:verify_test`, `//src/proof:kernel_test`,
`//src/semantic:capability_check_test`, and `//src:capability_execution_test` in the matching
tasks; those targets do not exist at planning time. Run targeted suites per change and the
full suite at integration/adoption gates. T57 must add or confirm supported ASan/UBSan build
configurations rather than assume the existing `tsan` configuration supplies those checks.
V0 records the landed value-storage, return-storage, frame-budget, and hosted/compile-time
test targets as additional required regression commands; use their actual names rather than
assuming the interface names proposed in the value-semantics plan became build targets.

## Milestones and Adoption Gates

| Gate | Required evidence | Permitted next step |
|---|---|---|
| V0: value-semantics baseline | Landed revision, mapped interfaces/test targets, and completion evidence for value-semantics T-01–T-14 | Begin source/runtime/HIR integration after its formal/core prerequisites; independent formal/core work does not wait for V0 |
| G0: formal readiness | T09; explicit assumptions and reproducible theorem evidence | Implement the compiler core and kernel |
| G1: core completeness | T29 plus kernel coverage; all primitive/contract families represented | Integrate source elaboration, still opt-in |
| G2: checked source | T38 and T54; every route to execution requires verification | Execute the supported experimental subset once its backend is ready |
| G3: execution parity | V0, T43–T46 and T55–T57; lifetime/failure evidence, preserved value-storage guarantees, RVO/NRVO checks, and baseline regressions on both backends | Expand experimental library use and evaluate performance |
| G4: adoption readiness | T60; all requirement IDs covered, budgets accepted, compatibility policy complete | Enable the selected release default in T61 |
| G5: migration complete | T62–T63; compatibility-window conditions met | Remove only redundant legacy machinery and publish final status |

The shortest useful demonstration is a verified buffer/list program that constructs an owner,
edits it, observes its length, copies it independently, freezes one owner, retains a child or
inline projection, releases the parent, and disposes all remaining resources correctly.
Add noncopyable `pop` plus a consuming match and one error-returning mutator before calling
the prototype representative. Formal readiness additionally requires the four translations
listed under required model decisions. A passing demonstration is not a substitute for any gate.

General adoption must not silently disable existing async or machine features. If T52 cannot
provide a sound supported boundary, keep the experimental mode opt-in and record that T60 is
not complete. A separately approved language release may advertise a restricted subset, but
that decision must be explicit in compatibility and release documentation.

Before T61, define rollback as selecting the prior language/runtime/stdlib/metadata combination
and rebuilding affected artifacts. Never reinterpret capability metadata under legacy rules
or mix runtime ABIs to recover a release. Existing source migrations remain reviewable; no
bulk rewrite should infer resource transfer where independent copying was intended.

## Completion Checklist

- [ ] Required formal properties are established with stated assumptions and reproducible artifacts.
- [ ] Generic dependency substitution, trait stored-origin relationships, branch cleanup points,
      and sequential generator transitions are settled and translated before safety proofs.
- [ ] Sequential generators preserve loans across yield and dispose suspended state exactly once.
- [ ] V0 records the completed value-semantics baseline, actual interfaces, and regression evidence.
- [ ] Every proposal primitive, conversion, table row, and prose counterexample is traced to tests.
- [ ] All supported runtime and compile-time execution paths consume verified bodies.
- [ ] Copies preserve source state and equivalent contents with independently owned mutable reach.
- [ ] Loans retain their carriers, granting chains, and lifetime obligations across calls and errors.
- [ ] Frozen storage and retained projections never regain mutable authority.
- [ ] Cleanup accounts for initialized resources once on every completed exit in the specified order.
- [ ] Effects, transitions, cleanup, and pending outcomes preserve only justified versioned facts.
- [ ] Kernel verification terminates or reports checking failure; automation cannot bypass it.
- [ ] VM, LLVM, runtime APIs, and compile-time marshalling agree on the chosen ABI and behavior.
- [ ] Capability operations extend the shared value-storage pipeline and pass both verification gates.
- [ ] Mandatory RVO, shared NRVO with safe fallback, inline allocation bounds, frame budgets, and
      tail-call lifetime checks retain the foundation's guarantees.
- [ ] The library, trusted boundary, and unsupported-feature policy match the advertised safety claim.
- [ ] Performance, migration, release, and rollback evidence satisfies the adoption gate.
- [ ] Normative documentation and graph context describe the implemented state accurately.

This document adds a plan only. It does not implement the core, complete formal proofs, run
compiler regression tests, or change the language's current normative behavior.
