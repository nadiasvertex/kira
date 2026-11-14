# Design C: safety and ergonomics plan

Status: proposed design and implementation plan. No task or proof below is complete.
This extends C in the [comparison](ergonomics-comparison.md) on the
[standalone B baseline](b-plan.md); it does not change the
accepted A core, current compiler behavior, or the decision ledger. Reconcile adoption
with T01/T08 of the [affine roadmap](../affine-capability-implementation-plan.md).

## Goal and effort model

C should support convenient application code and predictable systems code in one language.
Memory safety is required at every effort level. Additional programmer effort can remove
runtime checks, retention, and allocation, or establish stronger protocol guarantees.
It must not be necessary to abandon safety to escape ownership complexity.

| Concern | Convenient path | More explicit path |
|---|---|---|
| Ownership | Inferred moves, borrows, summaries, and conditional cleanup | Pinned contracts and explicit transfers |
| Sharing | Retained frozen values; managed mutable handles where needed | Unique owners and scoped views |
| Aliasing | Runtime-checked managed access | Statically checked access and disjoint partitions |
| Bounds | Checked indexing | Checked proof witnesses and branded indices |
| Interfaces | Inferred checked summaries | Stable explicit guarantees |
| Resources | Scoped deterministic cleanup | Checked authority and protocol transitions |
| I/O | Broad authority supplied to script entry points | Explicit, transitively enforced capability boundaries |

Choose representations through types and APIs. A failed static check must not silently
insert deep copies, managed sharing, freezing, or runtime borrowing. Managed values and
owned values can coexist in one program; there is no global weaker-safety language mode.
Static typing with inference is the starting point. This plan does not by itself provide
Python's dynamic object model or its ecosystem.

## Required static semantics

### Access requirements are separate from effects

Infer and export the authority a callable needs, including shared reads and exclusive
access, in addition to possible writes. A non-writing mutable borrow can still require
exclusive access. Result origins, store flows, call mode, cleanup, and ownership transitions
remain separate summary components. Include effects reached through callbacks, captures,
stored loans, and cleanup.

For a method that only increments `self.total`, the required receiver access can be
restricted to `self.total`. At invocation the caller acquires that authority, rather than
first constructing an exclusive loan of the whole receiver. The callee must be checked
under that same restriction. Merely learning a narrow write set after acquiring `&mut self`
is insufficient. A method that also reads `self.items` needs shared access there and must
conflict with a live exclusive element loan. Unknown/opaque requirements default to the
whole authority admitted by the signature, never to an empty set.

Contracts imported from metadata and trait/function-value types must carry this distinction.
Implementations may require less authority and provide stronger results than their contract;
they cannot acquire stronger authority by inspecting a private body at the call site.

### Places resolve to storage and preserve reborrow dependencies

Paths describe static projections; prefix overlap applies only after resolving aliases,
backing storage, and inherited partitions. Different roots are not evidence of separation.
Track the target and lifetime dependencies of dereferences and reborrows through moves,
aggregate storage, captures, results, and replacement. A parent loan remains reserved while
a child reborrow suspends its use; moving the parent does not remove that restriction.

Preserve allocation/backing identity internally where necessary. C removes mandatory region
syntax, not the obligation to model identity. Relocation, replacement, removal, destruction,
freeze, and ownership transfer must check affected loans before changing reachability.
Recursive calls need fresh dynamic instances of finite static descriptions, not identities
derived solely from source variable names. Define and validate their correspondence.

### Groups abstract storage and invalidation

Groups expose logical storage to contracts without exposing private fields. Their checked
meaning includes owned descendants and structural dependencies. For a list, `elements`
depends on the backing designated by `shape.data`; replacing that pointer, shortening the
initialized prefix, or destroying the list can invalidate element loans. Disjoint group
names alone cannot override those dependencies.

Safe field composition derives group coverage. Raw-pointer implementations require an
explicit trusted boundary whose contract covers pointed-to storage, aliasing, lifetime,
relocation, initialization, and cleanup. A group declaration is not evidence that those
properties hold. Nested and generic groups need substitution and conservative widening
when precise separation cannot be established.

### Facts depend on identity and current state

Infer observer dependencies where possible. Preserve a fact only if no intervening write or
transition invalidates its actual dependencies. Writes, relocation, destruction, replacement,
and cleanup all participate. Storage epochs or an equivalent internal representation may
be needed; eliminating a particular version representation is not a design requirement.

A positional in-bounds witness can survive sorting. An assertion that the index still
identifies the maximum element cannot: it also depends on element contents. A brand naming
an arena does not alone prove that an entry is still allocated after deletion or reuse.
Erasing bounds or generation checks requires a checked witness of the relevant property.
Monotonic postconditions, such as preserved bounds after growth, are optional proved contracts.

### Outcomes protect pending alternatives

Infer per-variant origins, store flows, initialization, and authority states. Before a result
is matched, protect every possibly retained loan and permit only operations valid in every
remaining alternative. Intervening calls and cleanup update all alternatives. Matching later
must not restore stale facts, old ownership, or authority invalidated since the original call.
Discarding or joining outcomes conservatively merges obligations. An error does not imply
rollback; writes on an error path remain writes.

Do not equate outcome inference with automatic theorem discovery. Capacity guarantees,
initialized-prefix guarantees, and refined parsing results require checked primitive
contracts or proved postconditions. Scope-exit drop flags account for maybe-moved values
without introducing early observable cleanup to make a borrow pass.

### Capability implementations preserve authority

Ordinary protocol typestate may use private constructors and affine value-indexed types.
A token authorizing unchecked memory access, lock-protected access, or other primitive
permissions must own or derive from checked authority. Its safe constructor cannot mint
duplicate authority, including inside its declaring module. Operations preserve, transfer,
or consume that backing; any native minting operation is explicitly trusted.

Affine does not mean must-consume: abandoning a token follows its cleanup contract and does
not prove an explicit `close` occurred. Pinning a boolean state parameter does not by itself
establish resource identity, uniqueness, or a generative brand.

## Managed mutable sharing

Add an explicit proposed `managed[T]` library/runtime abstraction for application code.
The initial candidate is single-threaded strong reference counting with weak handles and
runtime-checked scoped access. A strong cycle retains its objects; weak back edges break
cycles. This is a defined limitation to measure in the application gate, not a claim of
automatic collection. Do not advertise arbitrary-cycle reclamation until an alternative
collector or ownership discipline is specified and validated.

The proposed API used in the application examples is:

```text
managed.new(T) -> managed[T]
managed[T].try_read(fn(&T) -> R) -> result[R, borrow_error]
managed[T].try_edit(fn(&mut T) -> R) -> result[R, borrow_error]
```

These are proposed contracts, not existing standard-library declarations. Handle duplication
retains the same object; it does not deep-copy `T`. Active readers exclude editing; an editor
excludes all other access, including reentrant callbacks. A conflict returns `@err` before
the callback executes. The callback runs once on success. Retention and the runtime access
reservation remain live until callback cleanup completes, including an error return.
Neither a reference, view, borrowing closure, nor other value carrying the access loan may
escape in `R` or be stored into a longer-lived destination. Returning another independently
retained handle is permitted if it does not contain that loan.

The wrapper remains alive throughout access, and all aliasing handles share the same access
state. Retention alone never authorizes an element view across buffer relocation. Managed
objects cannot contain escaping stack borrows, are not implicitly frozen, and are not
implicitly thread-transferable. Initial managed access cannot suspend across yield/await.
Custom cleanup, partial construction, final release, weak upgrade, and reference-count
overflow need specified failure and cleanup behavior. Destruction cannot expose a live
access path to a finalized object. Concurrency needs a separate checked synchronization API.

More explicit code replaces managed state with unique ownership and passes state to callbacks,
or uses statically verified partitions. Crossings remain visible and checked: no implicit
conversion of a managed alias into a unique owner, or of managed mutability into permanent
freeze. Extracting an owner requires proof/checking that no retaining or accessing aliases
remain, under a separately specified operation.

## Stable public contracts and restricted I/O

Persist complete elaborated interfaces: access requirements, writes, transitions, origins,
store flows, call modes, group interpretation, outcomes, and proved guarantees. Compare a
candidate release against a recorded released interface, not merely a previous local build.
Detect stronger preconditions/authority needs, wider effects or dependencies, weaker
postconditions, and other changes that reject or invalidate previously valid callers.
When subtyping cannot establish compatibility, require an explicit interface/version change.

Allow explicit constraints to pin guarantees. Inference remains the convenient default;
pinning is not required for every function. Recheck dependencies against the interface
actually used, version metadata, and reject incompatible artifacts rather than interpreting
them under different ownership rules. Handle group renaming or splitting through semantic
mapping or an explicit breaking change.

An unrestricted script receives broad I/O authority from its entry environment. A restricted
module and its transitive calls can exercise only authority admitted by their contracts,
including closures, imports, globals, native calls, and cleanup. Importing an unrestricted
function cannot bypass the boundary. Define ambient script authority as explicit elaborated
authority, not an invisible exception to effect checking. Capability restriction is an
authority guarantee; it is not by itself an OS sandbox for untrusted native code.

## Dependency-ordered implementation work

| Task | Depends on | Deliverable and acceptance condition |
|---|---|---|
| C01 | — | Specify access acquisition, storage identity, reborrows, transitions, pending outcomes, and trusted boundaries. Give acceptance/rejection rules for each adversarial case below. |
| C02 | C01 | Implement finite place/provenance summaries and store-flow checking in an isolated model; reject local escape and retain loans stored through mutable parameters. |
| C03 | C02 | Infer access and effect summaries across recursive groups, indirect calls, captures, and cleanup. Use terminating widening/budgets and sound conservative fallback; check each body against its resulting summary. |
| C04 | C03 | Add group abstraction, structural invalidation, and fact dependencies, including generic/nested storage. Demonstrate that narrow method access is acquired before invocation. |
| C05 | C04 | Add per-outcome states with safe pending-result handling and checked postconditions. Validate errors that retain loans or replace storage. |
| C06 | C03 | Add authority-backed capabilities, generative brands, and checked native minting boundaries. Reject safe in-module forgery as well as outside forgery. |
| C07 | C03 | Implement/version complete interfaces, optional pinned guarantees, release compatibility comparison, and transitive restricted-I/O checking. |
| C08 | C01, C02 | Specify and prototype managed retention, weak handles, access reservations, failure, and cleanup. Validate reentrancy, escape rejection, cycles, and final release. |
| C09 | C04–C08 | Elaborate the application corpus for all three options, execute accepted programs, and record programmer work and runtime costs. Revise ergonomics before production integration. |
| C10 | C09 | Establish static-model and managed-boundary safety arguments; map evidence to G0/GO/GX/GP and runtime/backend gates. Only supported, validated combinations proceed to adoption. |

C02–C09 are prototypes, not permission to bypass the affine roadmap's verification gates
on production execution paths. Managed sharing extends the core's excluded interior
mutation and therefore requires its own rules and composition evidence before adoption.
Async/cancellation and concurrent managed access remain explicitly unsupported until their
extensions meet corresponding gates. No gate here is recorded as passed.

## Application and adversarial validation

The [complete application corpus](applications/README.md) starts with aggregation, deferred
callbacks, and fallible borrowed registration. Compare successful behavior and failure paths,
not signatures alone. Record declaration annotations, required structural rewrites, explicit
copies, retention/access/bounds checks, necessary allocations, cleanup traces, and diagnostic
quality. Distinguish source measurements, executed checks, and performance measurements;
do not report speculative counts or unimplemented acceptance as results.

Required negative/edge cases include:

- Different syntactic roots alias one target; moving a suspended parent loan preserves the conflict.
- A method reads a field with a live exclusive loan, even if it only writes another field.
- A helper, callback, or destructor adds a hidden read, write, or storage transition.
- Backing replacement, shrinking, or destruction invalidates element views and stale brands.
- An outcome is held, discarded, overwritten, or matched after intervening mutation or cleanup.
- An error stores a borrow or replaces an owner; matching the error does not roll back effects.
- A safe constructor tries to duplicate lock/index/peripheral authority inside its module.
- Reentrant managed access conflicts; returned/nested views and stored callbacks cannot escape access.
- Allocation/access failure and final release preserve exactly-once cleanup; weak upgrade after
  final release fails; strong cycles retain and weak back edges permit reclamation.
- A restricted module reaches unrestricted I/O through a higher-order, native, or cleanup path.
- A fresh checkout checks against a released interface and detects each incompatible summary change.

Use executable reference checks, source fixtures, and VM/LLVM execution as appropriate.
Test lifetime cases with actual reclamation and allocation-failure injection. Tests provide
evidence; they do not replace the soundness and composition arguments.

The decision criterion is a frontier across safety guarantees, programmer work, runtime
cost, and implementation complexity. C is a candidate, not an established Pareto optimum.
