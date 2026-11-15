# Design C: safety and ergonomics plan

Status: design revision in progress. The semantic decisions below were agreed on
2026-11-15; implementation tasks and proofs remain incomplete. A was previously accepted;
that choice is now being reconsidered. This document extends C in the
[comparison](ergonomics-comparison.md) on the [standalone B baseline](b-plan.md).
The first milestone covers the complete design, implemented step by step, rather than
introducing language features over successive releases. There are no existing users to
migrate. Reconcile the revised choice with the decision ledger and T01/T08 of the
[affine roadmap](../affine-capability-implementation-plan.md) before implementation adoption.

Public contract syntax and compatibility details remain the next design discussion.
The specific cross-thread synchronization API is also open; its required scope is recorded below.

## Goal and effort model

C should support convenient application code and predictable systems code in one language.
Memory safety is required at every effort level. Additional programmer effort can remove
runtime checks, retention, and allocation, or establish stronger protocol guarantees.
It must not be necessary to abandon safety to escape ownership complexity.

| Concern | Convenient path | More explicit path |
|---|---|---|
| Ownership | Inferred moves, borrows, summaries, and conditional cleanup | Pinned contracts and explicit transfers |
| Sharing | Unique ownership by default; explicitly selected sharing | Unique owners and scoped views |
| Aliasing | Static borrowing; explicit checked separation | Statically checked access and disjoint partitions |
| Bounds | Checked indexing | Checked proof witnesses and branded indices |
| Interfaces | Inferred checked summaries | Stable explicit guarantees |
| Resources | Scoped deterministic cleanup | Checked authority and protocol transitions |
| I/O | Broad authority supplied to script entry points | Explicit, transitively enforced capability boundaries |

Choose representations through types and APIs. A failed static check must not silently
insert deep copies, shared ownership, freezing, or runtime borrowing. Shared values and
owned values can coexist in one program; there is no global weaker-safety language mode.
Static typing with inference is the starting point. This plan does not by itself provide
Python's dynamic object model or its ecosystem.

The goal is sane defaults and zero-cost abstractions wherever possible. There is no
language-wide managed runtime. Reference counting and runtime access checks belong to
explicitly selected abstractions; ordinary ownership and borrowing are statically checked.

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

### Agreed borrowing, movement, and cleanup rules

- Infer the fields a method needs and acquire only that access before invocation; check
  its body against those requirements. Imprecise inference conservatively uses all access
  and effects permitted by declared contracts. Reject calls unsafe under that fallback.
- Direct simultaneous mutable borrowing requires statically proved separation. An explicit
  checked operation can validate runtime indices once and return disjoint views or an
  overlap/bounds error before creating either view. Successful views need no repeated alias checks.
- List growth is rejected while an element view is live, even with spare capacity.
  Unrelated fields remain accessible; replacement, removal, and destruction must preserve loans.
- Moving a reference transfers its restrictions without moving its target. Moving an inline
  aggregate with borrowed inline fields is rejected. Moving a heap-owning handle is allowed
  when its allocation stays stable and restrictions follow the new owner. No implicit boxing.
- Borrows end after their last possible use, including stored references, captures, and cleanup.
  Ending a loan does not run observable cleanup early.
- Functions may store caller-provided borrows when lifetime and conflicting-access restrictions
  are tracked. Inferred contracts expose store flows, including those on error paths.
- Pending outcomes protect all possible loans. Matching selects the applicable state without
  rolling back mutations. An error containing no borrow releases that outcome's loan obligation.
- Conditional moves are allowed. Later use requires definite availability; scope exit may use
  drop flags to clean up only values still owned there.
- Custom cleanup requires a complete object: extracting a field requires a valid replacement.
  Without custom cleanup, partial moves are allowed and remaining fields are cleaned up.
- Automatic cleanup suffices for safety but does not prove an explicit protocol step occurred.
  It cannot propagate recoverable errors or suspend. Explicit fallible close/flush operations
  report completion failures; asynchronous shutdown is an explicit awaited operation.
- Permanent freezing consumes unique ownership and excludes outstanding mutable access.
  Owned mutable descendants freeze with it; independently shared mutable objects do not
  silently freeze. Freezing introduces no implicit deep copy.
- Read-only loans prevent mutation through every alias to their referenced storage.
  Interior mutation requires an explicit abstraction with checked access rules. Reading a
  shared handle does not itself grant access to its contents.
- Uncertain preservation invalidates a fact. Rechecking is required before using it to omit
  checks. Sorting can preserve bounds while invalidating content-dependent facts.
- Generic bodies are checked against declared constraints for every satisfying type.
  Concrete types may improve optimization, but cannot change validity through private details.

## Explicit mutable sharing

Explicitly selected mutable shared ownership uses single-threaded reference counting,
weak handles, and runtime-checked access. Strong cycles retain objects until broken;
weak back-references permit reclamation. There is no automatic cycle collector.
The existing application examples spell this abstraction `managed[T]`; that is provisional
API vocabulary, not a managed runtime or the default representation of values.
Final public spellings remain open.

Duplicating a handle retains the same object without deep copying. Readers exclude editing;
an editor excludes other access, including reentrant callbacks. Conflicts return an error
before access is granted or a callback runs. Successful callback access runs the callback once
and retains the object and reservation through callback cleanup. No loan-dependent reference,
view, closure, or stored value can escape callback-scoped access. Independently retained
handles may be returned if they do not contain that loan.

Explicit access guards also support synchronous borrowing iterators and generators that
retain access across yield. Guards keep both the object and reservation alive; dependent
borrows prevent premature release. All aliases share the same access state. Retention alone
never permits invalidating an element view through relocation. Shared objects cannot contain
escaping stack borrows. They are neither implicitly frozen nor transferable across threads.

Finalization starts only after access guards are released. Once it starts, weak upgrades fail
and no new access guard can be acquired; resurrection is prohibited. Cleanup runs exactly once.
Ordinary allocation failure terminates; explicit fallible APIs return errors and safely clean
up partial construction without exposing uninitialized values. Reference-count overflow
terminates before wrapping. Termination is not a promise of recoverable-error cleanup.

An explicit fallible extraction operation recovers unique ownership only with one strong
owner and no active access guards or dependent borrows. Success permanently disables existing
weak upgrades; failure returns the original shared handle unchanged. There is no implicit
conversion to unique ownership or permanent freezing.

## Suspension, cancellation, and threads

These features belong to the complete first milestone, with their own verification obligations.
Synchronous generators may retain static loans across suspension. Suspended frames preserve
restrictions; destruction runs cleanup and releases remaining loans. Yielded views prevent
destruction of any storage they still reference.

Runtime access guards cannot cross `await`. Release them before awaiting and reacquire afterward,
rechecking invalidated facts. Statically checked borrows may cross `await` when the compiler
proves lifetime, storage stability, exclusion of conflicting access, and cancellation safety.
Tasks moving between threads must additionally satisfy transfer rules for all retained values
and borrows.

Cancellation is cooperative at defined cancellation points. It runs cleanup exactly once for
remaining owned values and releases outstanding reservations, while preserving obligations to
any surviving borrows. Cleanup cannot suspend; explicit asynchronous shutdown must be awaited.

Ordinary shared handles are thread-confined, avoiding atomic reference-counting costs.
Unique owners may transfer when their contents permit it. Cross-thread mutable sharing requires
an explicitly selected synchronization abstraction with checked access. Its concrete API,
transfer constraints, and cancellation/guard interactions remain to be specified; the
non-suspending runtime-guard rule applies. No concurrency proof is implied by sequential safety.

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
| C01 | — | Formalize the agreed semantics, feature manifest, proof obligations, access acquisition, storage identity, reborrows, transitions, pending outcomes, and trusted boundaries. Give acceptance/rejection rules for each adversarial case below. |
| C02 | C01 | Implement finite place/provenance summaries and store-flow checking in an isolated model; reject local escape and retain loans stored through mutable parameters. |
| C03 | C02 | Infer access and effect summaries across recursive groups, indirect calls, captures, and cleanup. Use terminating widening/budgets and sound conservative fallback; check each body against its resulting summary. |
| C04 | C03 | Add group abstraction, structural invalidation, and fact dependencies, including generic/nested storage. Demonstrate that narrow method access is acquired before invocation. |
| C05 | C04 | Add per-outcome states with safe pending-result handling and checked postconditions. Validate errors that retain loans or replace storage. |
| C06 | C03 | Add authority-backed capabilities, generative brands, and checked native minting boundaries. Reject safe in-module forgery as well as outside forgery. |
| C07 | C04–C06, CS, CT | Implement/version complete interfaces, optional pinned guarantees, release compatibility comparison, and transitive restricted-I/O checking. |
| C08 | C01, C02 | Specify and prototype explicit shared ownership, weak handles, access guards, failure, extraction, and cleanup. Validate reentrancy, escape rejection, cycles, and final release. |
| CS | C02, C05, C08 | Define and verify generator suspension, yielded loans, static borrows across await, cooperative cancellation, and synchronous cleanup; reject runtime guards across await. |
| CT | C03, C06, C08, CS | Specify and verify explicit synchronization APIs, thread confinement, owner/task transfer, and concurrent access. |
| C09 | C04–C08, CS, CT | Elaborate the application corpus for all three options, execute accepted programs, and record programmer work and runtime costs. Revise ergonomics before production integration. |
| C10 | C09 | Consolidate safety and composition evidence developed from C01 onward; map evidence to G0/GO/GX/GP and runtime/backend gates. Only supported, validated combinations proceed to adoption. |

Implementation proceeds in dependency order within one complete milestone. Experimental
models and executions do not bypass verification gates on production paths. Explicit interior
mutation, async/cancellation, and concurrency require their own rules and composition evidence.
Map the existing G0/GO/GX/GP gates to the revised scope and define additional async/concurrency
and sharing obligations before enabling those paths. No gate is recorded as passed.

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
- Reentrant shared access conflicts; returned/nested views and stored callbacks cannot escape access.
- Allocation/access failure and final release preserve exactly-once cleanup; weak upgrade after
  final release fails; strong cycles retain and weak back edges permit reclamation.
- A restricted module reaches unrestricted I/O through a higher-order, native, or cleanup path.
- A fresh checkout checks against a released interface and detects each incompatible summary change.

Additional acceptance/rejection cases for the agreed scope:

- Runtime overlap/bounds failures create neither view; successful partitions remain disjoint.
- List growth with a live element view fails; access to unrelated fields succeeds.
- Moving stable heap owners preserves loans; relocating borrowed inline storage fails.
- Last-use release includes captures, stored loans, and cleanup; conditional moves clean up once.
- Custom-cleanup objects reject partial moves without replacement; ordinary aggregates permit them.
- Frozen and read-only storage cannot be mutated through aliases; generic validity is contract-based.
- Reference-count overflow terminates before wrapping; fallible allocation cleans partial construction.
- Extraction disables weak upgrades on success and preserves the shared handle on failure.
- Borrowing iterators retain guards; yielded views prevent premature frame/storage destruction.
- Static loans across await meet lifetime/cancellation rules; runtime guards across await fail.
- Cooperative cancellation cleans remaining owners exactly once without suspending cleanup.
- Thread-confined handles reject transfer; approved unique transfers and synchronized sharing
  preserve access rules under concurrent execution.

Use executable reference checks, source fixtures, and VM/LLVM execution as appropriate.
Test lifetime cases with actual reclamation and allocation-failure injection. Tests provide
evidence; they do not replace the soundness and composition arguments.

The decision criterion is a frontier across safety guarantees, programmer work, runtime
cost, and implementation complexity. C is a candidate, not an established Pareto optimum.
