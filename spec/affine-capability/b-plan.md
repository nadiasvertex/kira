# Design B: scoped value semantics plan

Status: proposed alternative design and implementation plan. No task or proof below is
complete. This extracts B from the [ergonomics comparison](ergonomics-comparison.md) and
defines the implementation and validation obligations for that option. It is not a claim
that the current checker implements B. Reconcile any adoption with T01/T08 of the
[affine roadmap](../affine-capability-implementation-plan.md) and the accepted decisions.
The strengthened [C plan](c-plan.md) builds on this baseline with additional precision
and a separately specified managed mutable path.

## Goal and boundaries

B uses ordinary owned values, scoped borrows, field paths, and inferred flow summaries.
It aims to reduce exposed ownership machinery while preserving memory/lifetime checking.
Its deliberate tradeoff is conservative authority and effects: passing a whole receiver
for editing requires the whole receiver, and stored-loan summaries union all outcomes.

B supports explicit field threading and prepare/commit application APIs where narrower
contracts would be needed in A/C. The [complete applications](applications/README.md)
show these costs. Low annotation counts alone do not establish scripting ergonomics.
B is an independent design option and a potential implementation milestone for C, not
evidence that C's additional features are unnecessary or already safe.

This plan does not include inferred partial receiver authority, abstract storage groups,
user-pinned write sets, outcome-specific mutations of caller storage, checked sealed-token
creation, or managed shared mutation. Async/cancellation, concurrent mutation, raw-pointer
authority, arbitrary interior mutability, and safe type-erased scoped values need separately
defined extensions or explicit rejection. Existing compiler support does not waive those
boundaries.

## Rules

The numbering preserves the original comparison's B rules, which C extends.

1. **Values and modes.** By-value use moves affine values; values with checked implicit
   duplication rules duplicate. Borrows are `&T` and `&mut T`. There is no exposed
   `own[T]` owner-handle type. Distinguish implicit duplication from the structural witness
   for explicit `copy`: a witness for independent copying must not implicitly duplicate
   an owner. Resolve the earlier shorthand “`copy` types duplicate” against A's accepted
   copy distinction before implementation, without inventing a second conflicting meaning.
2. **Places.** A place has a root (local, parameter, temporary, result, or abstract backing)
   and field, dereference, index, or partition steps. Prefix overlap applies after resolving
   target aliases and backing provenance. Different roots do not establish separation.
   An index covers the whole sequence unless checked partition provenance narrows it;
   `split_at` children are disjoint. Reborrows keep granting dependencies and suspend
   conflicting parent access even if the parent handle moves.
3. **Origins.** Each value carries all loans it depends on, including shared/exclusive
   target places. Moving or duplicating a value preserves those dependencies. Aggregates,
   collections, variants, and closures carry the dependencies of their live contents.
   Substituting generic arguments or packing a value must not erase its dependencies.
4. **Scoped types.** A type is scoped if its values can carry loans, including borrows,
   range views, borrowing closures, and scoped type arguments. Statics, frozen retained
   sharing, tasks outside a checked `crew`, and type-erased boxes reject scoped values in
   this baseline. Define any supported `crew` boundary before allowing such crossings;
   its existence in the current language is not proof of compatibility.
5. **Liveness.** A loan remains live while any live carrier requires it, including stored
   values, captures, intermediate arguments, returned values, and pending cleanup. End
   a loan only after its last dependent carrier and cleanup obligation retire. Do not
   insert an early observable destructor merely to make a borrow check succeed.
6. **Call summaries.** Infer source-body summaries. Trait methods, native declarations,
   and callable types need contracts or conservative defaults. Summaries describe which
   inputs' loans or carried loans can flow to results, which can be stored through arguments'
   exclusive authority, and callable entries (once, reusable editing, reusable observing).
   Preserve direct versus carried origins, such as an iterator returning externally backed
   elements rather than borrowing its own frame. Unknown flow conservatively allows every
   compatible flow, but never authorizes a lifetime violation. Union possible stored-loan
   states over all outcomes; there is no variant-specific mutation summary.
7. **Authority and effects.** A call acquires the authority demanded by its signature.
   A whole `&mut self` remains a whole exclusive receiver loan even if the body writes only
   one field. Effects conservatively cover exclusive targets, carried mutable loans in
   by-value arguments and captures, and their reachable authority, including transitive
   calls and cleanup. Facts outside that affected storage survive only when no alias or
   structural transition can invalidate them. There is no clause to bound a write more
   narrowly. For disjoint field updates, pass the field itself.
8. **Store check.** Every stored loan must remain valid for the destination's required
   lifetime. For a destination reached through a parameter, use the caller's target storage,
   not the shorter-lived callee parameter binding. Reject a loan of a callee local stored
   into caller-lived storage. Loans from other inputs can be stored only when the lifetime
   relation is established; record the store flow in the summary and update the caller's
   destination carrier. Overwrite, extraction, movement, and cleanup preserve or retire
   those dependencies as appropriate. A store check alone without caller propagation
   does not close the dangling-view bug.
9. **Sharing and copying.** Keep A's permanent freeze, `shared_read`, `shared_str`, and
   `shared_slice` guarantees with a value-based surface. Explicit `copy(x)` constructs
   independently owned mutable reach when supported, or retains a duplicable frozen handle
   under its contract. No implicit deep copy, freeze, thaw, or scoped-to-retained promotion
   is permitted. Copying cannot shed loans embedded in a value. Frozen projection, retention,
   cleanup witnesses, and thread-transfer properties need checked rules, not name aliases.
10. **Cleanup.** Keep a drop flag for a value moved on only some paths, and clean it at its
    specified scope exit if still owned. Track partial initialization and replacement so
    each initialized resource is cleaned exactly once. Loans stay valid through cleanup;
    branch joins cannot hide stored dependencies or obligations. Observable cleanup timing
    must be specified rather than selected opportunistically by last-use analysis.

## What B removes and what it retains

| A machinery | B treatment |
|---|---|
| Region identities, ownership forest, `tree(r)` overlap | No exposed region syntax; resolved places still need backing identity, alias checks, and pre-transition lifetime checks |
| `own[T]`, `&>`, owner-slot versus child-content surface, `copy_owned` | Ordinary owned values; `&mut xs[i]` has type `&mut T`; same-type explicit copying |
| Granting-chain and suspension representation | Representation can change; granting dependencies and suspended-parent restrictions remain |
| Narrow write footprints and per-region fact versions | Conservative effects from whole exclusive authority; no user write bounds |
| Per-outcome caller-storage states | Union over outcomes; state carried directly in result payloads still follows ordinary carrier rules |
| Sealed capabilities checked inside their module | Private constructors and affine indexed values; primitive-authority implementations remain trusted |
| Carriers, result/store origins, callable modes | Retained and required across module and generic boundaries |
| Field paths, checked partitions, freeze, explicit copy, cleanup | Retained with a value-based surface and complete safety obligations |

Places do not make storage identity, relocation, or destruction harmless. Before detaching,
replacing, moving, freezing, or destroying backing, check outstanding loans against the
old storage state. After a transition, facts about old storage must not silently refer to
new storage at the same syntactic path. Unknown aliasing is conservative.

## Deliberate precision and trust limits

- **Whole-receiver conflicts.** A method editing `self.total` conflicts with a live borrow
  of `self.items` if its signature takes all of `&mut self`. The repair is a field helper,
  as shown in the report application. Merely inferring that it writes only total does
  not change B's authority requirement.
- **Conservative facts.** A whole exclusive call can invalidate shape/bounds facts even
  when it happens to edit only elements. Existing checked indexing remains available;
  no stale witness may justify check erasure.
- **Union store flows.** A direct registration call may keep its source borrowed after an
  error because success could have stored it. A prepare/commit API moves the decision into
  a result payload and performs the store explicitly only on success. No error implies
  rollback, and discarding a result does not discard a loan already stored elsewhere.
- **Private typestate.** Affine indexed values and private constructors restrict outside
  construction and reuse. They do not prove uniqueness or validity inside the declaring
  module. If such a token authorizes a primitive memory-sensitive operation, the implementation
  belongs to the explicit trusted base. Ordinary private syntax cannot silently create
  arbitrary safe primitive permissions. B does not provide C's checked-authority guarantee.
- **Application sharing.** Independently retained shared mutable callbacks need an extension.
  The baseline passes unique state explicitly; it does not silently add reference counting,
  runtime borrowing, or cycle collection when static ownership rejects a program.

## Interfaces and diagnostics

Export complete result/store-flow and call-mode summaries for separate compilation. Callers
use imported contracts, not private body inspection. Check implementations against trait,
native, and function-value contracts; conservatively reject unsupported flow relationships.
Version metadata so stale or mismatched ownership interfaces cannot be executed as valid.

Inferred public origins or store flows can widen and break callers even without C's inferred
write sets. Record released interfaces and compare those observable components against the
release baseline; local build history is insufficient. Explicit origin/call-mode constraints
can pin relationships once their surface is specified. This does not introduce narrow
write contracts or partial receiver inference into B.

Diagnostics should identify the original loan, the carrier retaining it, the conflicting
operation, and any transitive store/callback path. For intended B limitations, suggest the
actual repair (field helper, explicit state, or prepare/commit) instead of an unchecked copy
or an annotation that cannot change B's semantics.

## Dependency-ordered implementation work

| Task | Depends on | Deliverable and acceptance condition |
|---|---|---|
| B01 | — | Formalize rules 1–10, copy/duplication distinction, cleanup timing, destination lifetimes, alias resolution, and machine/task boundaries. Define exact supported and rejected forms. |
| B02 | B01 | Prototype field/partition places with backing identity and reborrow dependencies; reject aliasing through different roots and access via moved suspended parents. |
| B03 | B02 | Implement carrier propagation and the store check; reject callee-local escape and retain argument loans stored into caller storage through mutable parameters. |
| B04 | B03 | Infer result/store-flow and call-mode summaries through recursion, higher-order calls, captures, and generics. Use bounded inference with sound conservative fallback and verify bodies against their summaries. |
| B05 | B04 | Implement whole-authority effects, structural invalidation, and union stored-loan states; accept result-payload flow without introducing per-outcome caller-storage summaries. |
| B06 | B05 | Integrate partial initialization, replacement, drop flags, and freeze/copy/retained-range contracts; preserve dependencies and exactly-once cleanup on all exits. |
| B07 | B04 | Export/version complete B interfaces, check released-contract compatibility and indirect/native contracts, and implement explanatory flow diagnostics. |
| B08 | B06, B07 | Port the complete A/B/C application corpus and library baseline; record rewrites, annotations, copies, checks, and known conservative rejections before selecting B or C. |
| B09 | B08 | Establish the model/implementation safety argument and map verification, runtime, backend, and adoption evidence to the affine roadmap's gates. No production execution bypasses verification. |

These tasks are prototypes and proposed integration work, not completed gates. Adding
generators requires loan-carrying suspension/frame and disposal rules with GX-equivalent
evidence. Async cancellation, concurrency, machine interoperation, and compile-time execution
need explicit supported boundaries and validation before broad adoption. B does not inherit
A's proofs merely by retaining some of its concepts.

## Validation and decision gate

Run the [three application workflows](applications/README.md), keeping support assumptions
and differences visible. B should accept the field-helper report, reusable callbacks with
explicit state, and prepare/commit registry. It should reject the whole-receiver conflict
and the direct registry mutation after an error under its union summary. A syntax/type error
in an unsupported feature is not evidence of the intended ownership rejection.

Minimum adversarial coverage includes local escape, caller-side stored-loan propagation,
aliases with distinct roots, moved suspended parents, temporary-backed views, generic
containers and closure captures, helper/cleanup effects, relocation/removal/destruction,
pending results, and branch cleanup. Test with reclamation and failure injection; compare
VM/LLVM and compile-time execution only for the advertised supported subset. Native contracts
and token implementations in the trusted base need a recorded audit boundary.

The ledger separates proposed acceptance, executed source conformance, runtime behavior,
cleanup evidence, and performance measurements. No counts or acceptance results in this
plan are measured implementation evidence. Choose B only after evaluating whether its
smaller exposed contract model compensates for application restructuring and conservative
rejections; compare that outcome with C's runtime and implementation costs.
