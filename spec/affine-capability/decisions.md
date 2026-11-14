# Affine Capability Decisions

This ledger records implementation decisions for the
[affine capability plan](../affine-capability-implementation-plan.md).
An accepted decision does not establish a theorem or complete an implementation task.

## Q1 — Formalization toolchain and proof artifacts

Status: accepted on 2026-11-14. Owning task: T01. Dependent tasks: T02–T09.

### Decision

Use Lean 4 with Lake for the sequential core's formalization. Version-controlled `.lean`
definitions and theorem proofs under `spec/affine-capability/formal/` are the authoritative
proof artifacts. Markdown documents explain those artifacts and link requirement IDs to
fully qualified theorem names. Model tests supplement the proofs.

Pin one exact Lean release in `lean-toolchain` and dependency revisions in Lake's manifest.
Toolchain upgrades require a fresh check of all required theorems and their assumptions.
The [formal artifact contract](formal/README.md) defines the planned modules, build target,
checking workflow, and evidence needed at T09. Package bootstrap and exact version selection
remain T01 work; no Lean package or proof is supplied by this decision.

### Rationale and alternative

The initial core is sequential and exposes its regions, loans, carriers, initialization,
cleanup, and effects explicitly. A direct operational semantics with inductive judgments
and state invariants is an appropriate starting point. Lean supports both those judgments
and executable functions whose correspondence to the judgments can be proved. Its kernel
checks proof terms produced by elaboration and automation.

Rocq with Iris is the principal alternative, with relevant ownership and borrowing precedent
in RustBelt. Lean is selected for this direct sequential development. Concurrency and
verification of unsafe library implementations are outside the initial core; this choice
does not claim that they would require the same proof architecture.

### Trust and scope

Lean is the metatheory tool. The planned Cinder `src/proof/` kernel is a separate finite
certificate checker. Define its certificate language and a reference checker in Lean and
prove acceptance soundness under explicit contexts, storage dependencies, and assumptions.
Lean is not a required dependency of the eventual Cinder executable.

Required proofs may depend on Lean's standard foundational axioms, with the actual
dependencies reported. They must not depend on `sorryAx`, project-defined axioms, or axioms
introduced by native evaluation. Native/runtime boundaries appear as explicit contract
hypotheses in theorem statements, with their scope recorded in the requirement ledger.
Do not hide a required safety property in an unproved hypothesis.

The resulting proofs concern the formal core and reference checker. They do not automatically
verify the C++ checker, value-storage pipeline, native implementation, or backends. Those
correspondence obligations remain separate. Cleanup accounting concerns completed exits;
it does not promise termination or eventual cleanup for diverging programs.

### Sources

- [Lean elaboration and kernel checking](https://lean-lang.org/doc/reference/latest/Elaboration-and-Compilation/)
- [Lean axioms and dependency auditing](https://lean-lang.org/doc/reference/latest/Axioms)
- [Lake builds and project checking](https://lean-lang.org/doc/reference/latest/Build-Tools-and-Distribution/Lake/)
- [RustBelt ownership and borrowing formalization](https://plv.mpi-sws.org/rustbelt/)

## Q2 — Ownership default and explicit copying

Status: ownership-default, contract-inference, basic surface-spelling, `copy`/`copy_owned`
operation names, the `copy` constraint name, single-element view consolidation, scoped
range-view construction and passing, retained frozen ranges with library `.slice()`,
callable parameter names/result-origin bounds, write bounds including `writes nothing` and
the callable environment root `self`, callable receiver spellings with inferred lambda
call modes, capture lists with value defaults, and `from self` capture origins accepted on
2026-11-14. Q2 remains partially resolved; further mappings and contract spellings are pending.
Owning task: T08. Dependent tasks: T30–T36 and the affected library migrations.

### Decision

Surface value passing moves owned values by default. An ordinary owning argument transfers
its value, authority, and cleanup obligations, leaving its source unavailable. The same
transfer policy applies to ordinary by-value bindings, assignment, returns, and ownership
captures. Fresh expression results may initialize their destinations directly.

Scalars, retained frozen handles, and other values with checked duplication rules may
duplicate under those rules. Duplication must preserve retention and loan dependencies.
Having a structural copy witness does not make an owner duplicable and does not authorize
implicit independent copying. Passing ownership, including through an unconstrained generic
parameter, requires no structural copy witness.

Declared inspection and edit parameters select borrowing or reborrowing. In particular, an
existing edit passed to an edit helper reborrows by default so sequential helpers can reuse
the caller's handle. A returned or stored reborrow continues to constrain the original
handle until all dependent carriers retire. Explicit transfer of that edit handle remains
available as `&>e`, and invalidates its source without discarding dependencies.

Independent copy construction and materialization require explicit requests and structural
witnesses. Freezing an owner is explicit as well: a bare owner cannot satisfy a persistent
sharing parameter by implicit freezing or copy-then-freeze. `publish(&!x)` freezes
and consumes the original; `publish(&!copy_owned(x))` explicitly materializes and freezes a copy,
preserving `x` when both operations are valid. An existing frozen handle can satisfy the
sharing parameter through independent retention.

The formal core records transfer, duplication, independent copy construction, loans, and
freeze explicitly. Defaults belong to surface elaboration and do not create alternative
implicit core transitions. The [design's conversion table](../affine-capability-core.md#capability-requests-and-conversions)
and the [implementation plan](../affine-capability-implementation-plan.md) carry this policy.

### Rationale and alternative

Move-by-default handles resources, closures, and sealed authority without requiring copies
that may be impossible. It keeps allocating, potentially fallible independent construction
visible and aligns owned-value passing with current Cinder's ownership model. Parameter
capabilities provide reuse through scoped loans; explicit copies provide independent values.

Copy-by-default would preserve the caller's source for copyable inputs, but would insert
potentially expensive construction and require witnesses for ordinary generic value passing.
It would still need a separate transfer route for resources. Explicit copy construction
retains the independence guarantees without making it the default operation.

### Contract inference and programmer constraints

Infer sound, conservative effect, result-origin, and outcome contracts from checked source
bodies by default. Public definitions do not require handwritten contracts merely because
they are public. Export the complete checked summaries so callers never need to inspect a
body. Conservative alternatives are valid when they account for all capabilities, retained
loans, initialization, and cleanup; ambiguity does not automatically require an annotation.

Allow programmer-written constraints to pin desired guarantees. Check inferred behavior
against every supplied constraint. Unconstrained inferred summaries may change with their
implementation, requiring dependent code to be rechecked. Recursive groups can infer or
use conservative candidate summaries, with every body verified before acceptance. Opaque
or native boundaries require sufficient explicit contracts or sound interface defaults.

When inference cannot establish a sufficient contract, explain what information is missing,
why the current conservative summary prevents the requested operation, and where the author
can supply the smallest relevant constraint. Show a legal code change when the body cannot
satisfy that constraint. This is part of the compiler-as-a-teacher mission: annotations are
checked promises, not escape hatches from safety rules. Never infer copying, freezing, or
new authority merely to resolve missing contract information.

### Accepted surface spellings

Use `own[T]`, `inspect[T]`, `edit[T]`, and `shared_read[T]` for explicit capability types.
Preserve `&T` as shorthand for `inspect[T]` and `&mut T` as shorthand for `edit[T]`.
Ordinary by-value `T` remains distinct from the explicit owner-handle type `own[T]`.

Preserve `self` as an inspection receiver and `mut self` as an edit receiver. An explicitly
consuming receiver writes `self: own[Self]`. Ordinary receiver calls follow the declared mode.

Use `&x` for explicit observation and `&mut x` for explicit editing, including the corresponding
dependent reborrows. An expected edit parameter cannot strengthen `&x` into editing, and an
explicit edit request is not silently rewritten as observation for an inspection parameter.
Bare arguments still follow the parameter mode. Use `&>x` for explicit transfer and `&!x`
for consuming freeze. Existing edit handles reborrow to edit parameters unless transferred
explicitly. These choices preserve Cinder's visible observation/edit distinction.

Use existing `let` and `var` for local construction, with optional annotations. No new builder
keyword or implicit freeze is introduced. Capability authority and local binding mutability
are both checked: `var` cannot make frozen storage writable, and `let` does not freeze an owner.

Use `shared_read[T]` for retained frozen sharing during the experiment. Legacy `shared[T]`
and `shared expr` are not aliases or unchecked conversions. Legacy sharing stays available
in legacy mode; replacement and removal follow the compatibility/adoption gates. Reusing
the shorter name for the new semantics is a possible adoption-time decision, not an implicit
compatibility mapping. Sequential frozen sharing does not establish thread-transfer safety.

### Independent-copy operation names

Use `copy(x)` for source-preserving structural value copying, returning the same surface
value type. Use `copy_owned(h)` to observe represented `T` through a live capability and
construct equivalent contents in a fresh `own[T]` root. Both are explicit, witness-authorized
operations; neither consumes its source or is selected implicitly by a parameter type.

Copying a `shared_read[T]` handle retains the same frozen target and requires the handle
value's witness. `copy_owned` through that handle instead needs represented `T`'s witness
and constructs fresh mutable reach. The same materialization is available through a valid
inspection or edit of copyable represented data; copying cannot remove scoped loans stored
inside that representation. `copy` on the scoped handle itself has no witness in the initial
core even when materializing its referent is supported.

The core uses `copy_value` for the operation exposed as `copy`, and `copy_owned` for
fresh-owner materialization. `clone` is not an alias for either. Legacy and user-defined
`clone` methods remain ordinary calls with their own contracts and do not establish a
structural witness by name. `copy`/`copy_owned` avoid the ambiguity of a second copy-like
operation whose name does not identify its authority or result-type distinction.

The [surface mapping](surface.md) gives the accepted spellings and examples. These decisions
do not amend the normative grammar or implement the new mode.

### Structural copy constraint

Use `copy` as the public structural constraint requiring a checked independent-copy witness.
The constraint and the operation `copy(x)` describe the same capability at the type and value
levels. `copy_owned(h)` requires the witness for represented `T`, rather than the source
handle type. An owner satisfying `copy` still moves by default; the constraint never changes
ordinary value passing into implicit construction or duplicates its ownership token.

Retire the legacy `copy` concept's implicit-duplication meaning in the experimental mode.
The compiler continues to track permitted duplication, including retention and loan rules,
as a separate property. No public `copyable` or `duplicate` constraint is introduced. This
avoids giving nearly identical names to different guarantees and follows the preference for
verbs. Inspection duplication, for example, does not establish the structural `copy` witness.

Legacy mode retains its existing concept until adoption. Audit existing generic bounds by
their actual operations during migration; an old `copy` bound is not evidence for the new
witness. No compiler implementation or normative grammar changes are made by this decision.

### Single-element views and borrowed slots

Collapse `cell[T]` into `inspect[T]` and `mut cell[T]` into `edit[T]`. Single-element views
use the existing capability types and inferred loan origins. Preserve `slice[T]`,
`mut slice[T]`, and `str` as range views carrying their shape and authority.

A capability may lend an initialized slot: `edit[own[T]]` targets the owner slot in a
containing collection, whereas `edit[T]` targets the child's represented contents. The slot
loan carries its containing allocation, checked path, granting chain, and lifetime
dependencies. Child authority alone cannot replace the owner's slot in its parent.
`*slot` designates the borrowed place; `exchange(*slot, incoming)` transfers the previous
value out and the replacement in without requiring `copy` or leaving an empty element.
Assignment through the slot performs checked replacement and cleanup. Loads never silently
duplicate owner tokens. A slot loan preserves its checked place footprint and any inherited
range partition; unequal indices alone do not establish separation.

Element-borrowing APIs expose `inspect[T]` or `edit[T]` with checked origins and replacement
authority. Different collection implementations can share that interface while retaining
the element type and loan dependencies. Heterogeneous element types need a sum or a
separately checked trait-object abstraction; cell types do not provide element-type erasure.

Legacy cell types and methods remain in legacy mode until migration. Any transitional aliases
in the new mode must have the exact capability semantics. Library migration reviews element
and range indexing separately, preserving range result shape, and replaces cell `get`/`set`
with permitted loads/observations and checked replacement. The
[worked slot translation](translations.md#worked-example-a-borrowed-collection-slot)
records the generic helper, ownership flow, and rejection cases. Formal and implementation
validation of slot provenance, replacement validity, and dependent loans remains required.

### Scoped range views

Accept observation by default for `items[start .. end]` and explicit observation with
`&items[start .. end]`, both yielding `slice[T]`. `&mut items[start .. end]` requests
`mut slice[T]`. The result is the range capability, not a reference to a temporary descriptor;
expected types cannot silently strengthen observation into editing. Text observation slicing
yields `str` with checked UTF-8 boundaries. All ranges require valid bounds, initialized
storage, checked authority, and origin dependencies; slicing neither copies elements nor
owns the backing storage.

Existing mutable views reborrow for bare arguments: observation for `slice[T]` parameters,
editing for `mut slice[T]` parameters. Ordinary binding moves a mutable view, while `&>view`
explicitly transfers its authority and invalidates its source binding. Observation duplication,
reborrow, and transfer preserve the corresponding loan dependencies. Returned and stored
subranges keep their source borrowed; conflicting authority remains suspended as required.

Different subrange bounds alone do not establish disjoint edits. Use the checked `split_at`
primitive for independent child edit ranges. A mutable range of owners lends owner slots,
preserving the distinction between
`edit[own[T]]` and child `edit[T]`. Frozen backing permits scoped observation, but does not
implicitly make the range independently retained. The
[range translation](translations.md#worked-example-range-views-and-reborrowing)
records the accepted construction, passing, and rejection cases. Formal validation and
implementation remain pending.

### Retained frozen ranges and library slicing

Accept `shared_slice[T]` and `shared_str` for independently retained frozen collection and
UTF-8 text ranges. Keep `slice[T]` and `str` scoped; wrapping either scoped descriptor in
`shared_read[...]` is not a loan-to-retention conversion. Frozen-source range indexing still
creates a scoped view. An ordinary library method `.slice(start .. end)` explicitly creates
a retained range; retained ranges support the same method for independently retained
subranges. It validates bounds, initialized storage, and text boundaries as applicable,
preserves the source, and copies no elements. Small ranges may retain whole backing
allocations. Handle duplication independently retains the same frozen storage and requires
no structural witness for copying elements; frozen projection and release rules still apply.

Implement the library API over an explicitly contracted intrinsic for retained frozen
projection. There is no universal source-level `retain_range` builtin or new call syntax.
The intrinsic requires existing persistent frozen authority, checked provenance, and range
validity; it establishes independent backing retention before exposing the result. Library
wrappers perform the public validation and compose this primitive. Neither an ordinary
scoped view nor a raw pointer can acquire retention by appearing read-only. Returning the
result does not keep a loan on the local source handle, but does preserve the identity of the
frozen backing storage. Copying elements, freezing, thawing, and owner materialization are
separate operations.

The [retained-range translation](translations.md#worked-example-retained-frozen-ranges)
records returned ranges, subranges, text boundaries, and the scoped-escape diagnostic.
Formal and implementation validation remain pending. This choice fixes surface types and
semantic retention, not Q4's concrete ABI, the intrinsic's name, or Q5's failure policy.

### Callable parameter names and result origins

Accept optional parameter names in `fn(...)` types and the result-origin form
`fn(part: slice[int32]) -> slice[int32] from part`. Multiple permitted origins use
`from {left, right}`. Names bind arguments within the callable contract; consistent renaming
preserves its meaning and implementation parameter names need not match. Existing unnamed
parameter types remain available when the contract does not need to refer to them.

Instantiate the origin bound separately for each call. An input-only bound excludes scoped
dependencies on captured storage, unrelated sources, and earlier calls' arguments. Captures
used only for computation remain permitted. Preserve all backing, grant-chain, and lifetime
dependencies through the result and forwarding helpers; the bound grants no new authority.

Infer source callable summaries and check them against optional expected constraints.
Passing or storing a callable must preserve sufficient checked contracts. Omission does
not assert an input-only origin or discard dependencies; opaque boundaries require enough
explicit information or sound conservative defaults. Report the missing relationship or
actual incompatible source when a callback cannot satisfy the requested contract.

The [callback translation](translations.md#worked-example-callback-result-origins) records
inferred implementation and forwarding contracts, compatible scalar captures, and rejection
of a result borrowed from a capture. This accepts the result-origin spelling provisionally
for the experiment; capture-derived results also use the environment-origin forms below.
Formal validation and implementation remain pending.

### Write bounds and the no-write spelling

Accept `writes nothing` as the explicit no-write constraint for declarations and callable
types. Omission requests inference or a sound conservative bound; a nonempty root set such
as `writes {count}` bounds permitted writes. Reject `writes {}` and suggest either
`writes nothing` or omission according to intent. Do not introduce `writes none` as an alias.
The explicit words distinguish a checked prohibition from the absence of a constraint and
fit the existing phrase-like contract forms such as `transfers ... to ...`.

Accept `self` as the callable environment root inside callable-type contracts. A callback
with `from part` and `writes {self}` may modify a captured counter while returning a view
derived only from its input. The environment footprint follows captured authority to its
targets, including edit loans; an enclosing function can bound these writes with
`writes {choose}` for its callback parameter. Actual region dependencies remain in complete
checked summaries without exposing capture layout. Result origins, write authority, and
environment call modes remain distinct; no write clause grants authority or chooses a mode.

All bounds cover helpers and cleanup. `writes nothing` forbids writes to pre-existing
storage; it permits fresh allocation and does not establish purity, termination, unchanged
ownership, or absence of transitions. Omitted clauses infer those components under the
existing policy. The [counter callback translation](translations.md#worked-example-callback-write-bounds)
records explicit and captured counters. These are experimental design choices; parser,
contract verification, and runtime validation remain pending.

### Callable receivers and inferred call modes

Accept optional leading callable receivers `fn(self, ...)`, `fn(mut self, ...)`, and
`fn(self: own[Self], ...)` for observing reusable, editing reusable, and consuming entries.
The receiver is implicit when invoking the callable. `Self` names the callable environment
type without choosing its allocation representation; contract `self` names that environment.

Infer the required receiver from source lambda bodies and check optional explicit constraints.
Capturing an edit and reborrowing it on invocation requires editing access to the environment;
moving a captured owner out requires a consuming entry. Omitted receivers retain checked
inferred or sound conservative call-mode information, rather than promising observation.

Keep the callable's parameter passing mode separate from its entry receiver. In
`&mut fn(mut self, part: slice[int32]) -> slice[int32] from part writes {self}`, the outer
edit borrows the callable, while `mut self` states the authority needed to invoke it.
Taking ownership of an editing callable still permits repeated calls before disposal.
Observation cannot invoke an editing entry, and a borrowed callable cannot satisfy a
consuming entry by an implicit owner transfer or copy.

The [call-mode translation](translations.md#worked-example-callable-receivers) records
sequential calls of a captured-counter callback while its earlier input-derived result
remains live. Result origins and captured loans retain their independent obligations.
Call modes do not imply write bounds: an observing entry can write through an explicit
edit argument, and `writes nothing` need not imply observation. Formal validation and
implementation remain pending.

### Capture spellings and environment result origins

Accept `[x]`, `[&x]`, `[&mut x]`, and `[]` as value capture, observation capture, edit
capture/reborrow, and no captures. An explicit list limits access to its listed enclosing
locals. Without a list, infer required variables and use the same value-capture rules:
move affine values and duplicate only values with checked duplication rules. Preserve all
loan dependencies and cleanup obligations. Independent copying is explicit, and borrowing
is explicit rather than selected by escape analysis or an expected callback type.

Accept `from self` for dependencies supplied through the callable environment and
`from {part, self}` for input/environment combinations. Infer source contracts and check
optional constraints; the environment root does not reveal private capture layout.

Preserve the difference between borrowing storage owned by a capture and duplicating an
already captured observation. The former must keep supporting ownership dependencies alive;
the latter can retain its original dependencies without retaining the closure when the
checked summary preserves that relationship. Less precise interfaces may conservatively
retain additional dependencies, never erase actual dependencies. `from self` is an origin
bound, not an unconditional loan on the environment or permission to escape dead storage.
A borrowed capture that outlives its source or a result borrowed from storage destroyed by
invocation cleanup remains invalid. The [capture translation](translations.md#worked-example-capture-origins)
records the three storage relationships and mixed origins. Formal validation and
implementation remain pending.

### Remaining scope and obligations

Q2 still needs remaining contract forms and legacy compatibility mappings.
Beyond the accepted callable result-origin and write-bound forms, the worked
`from`, `outcome`, `entry`,
`transfers`, and `preserves` clauses remain syntax candidates with the accepted inference
policy. The legacy implicit-duplication
classification and the structural independent-copy witness must remain distinct despite
the mode-specific reuse of the `copy` constraint name.

T08 must validate the remaining choices with representative translations. Structural copy,
separation, freeze, lifetime, and cleanup proofs remain required even though copying is
explicit. This decision completes no implementation task and changes no current compiler
or normative-language behavior.

## Named-field borrowing and mutable range splitting

Status: accepted for the initial experimental core; formalization and implementation pending.

Keep allocation identity for lifetime, ownership, and reclamation, and refine loan/access
footprints by checked named-field paths. Distinct fields of the same structural product may
be borrowed independently, including nested fields and their exclusively owned descendants.
Equal and prefix paths overlap. Whole-parent moves, replacement, freezing, destruction, and
whole-receiver calls still conflict with affected field loans. Aliases carried in separate
fields retain their actual target provenance; field names do not separate those targets.
Private projections must satisfy abstraction contracts. Fact invalidation may remain
allocation-wide; permission to edit a sibling does not imply preservation of sibling facts.

Accept built-in `split_at(view, index)` for `mut slice[T]`, returning a pair of mutable ranges
by reborrowing the source. Validate the index within the visible source bounds before
creating either child. The core primitive establishes partition evidence and preserves the
backing identity, granting chain, and lifetime dependencies. Both children are affine;
nested splits preserve ancestry, and returned/stored descendants keep conflicting parent
authority suspended. Empty partitions are valid and authorize no element access. Arbitrary
slicing and unequal indices do not independently establish disjoint edit authority.

These decisions belong in T02–T04 and the T08 translations before T05–T07 proofs, then in
T11–T16, contracts/metadata, and T48–T50 migration. They do not establish the required safety
lemmas or change the current compiler. See the [worked translations](translations.md#worked-example-named-field-borrowing-and-range-splitting).

## Deferred ergonomic extensions

Release-only custom cleanup as a route to freezing is deferred. The initial rule still
requires recursively freezable contents and a checked structural frozen-release recipe;
mandatory custom unique cleanup blocks freezing. A future extension must establish recursive
obligation accounting, permitted effects, and observable cleanup timing without invoking
arbitrary mutable destructors on frozen storage. T48/T49 must record unsupported container
cases rather than presume this extension exists.

Nonfreezable scalar interior mutability is deferred. Before acceptance it needs explicit
alias authority, write effects and version invalidation, restrictions on borrowing contents,
and a closure-sharing model distinct from permanent frozen sharing. No new type name or
exception to inspection/freeze rules is accepted here.

The accepted absence of a public duplication bound remains unchanged. Generic independent
copying continues to use explicit `copy` operations and the structural witness constraint.

## Remaining gates

Q2 remains partially resolved; Q3–Q8 remain unresolved. V0 has no recorded baseline evidence
yet. The accepted decisions do not authorize general language adoption.
