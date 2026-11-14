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
operation names, the `copy` constraint name, single-element view consolidation, and scoped
range-view construction and passing, and retained frozen ranges with library `.slice()`
accepted on 2026-11-14. Q2 remains partially resolved;
further mappings and contract spellings are pending.
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
duplicate owner tokens. A slot loan preserves the initial core's whole-allocation conflicts.

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

Different subrange bounds do not establish disjoint edits under the initial whole-allocation
model. A mutable range of owners lends owner slots, preserving the distinction between
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

### Remaining scope and obligations

Q2 still needs callable and capture spellings and remaining contract forms. The worked
`from`, `writes`, `outcome`, `entry`,
`transfers`, and `preserves` clauses remain syntax candidates with the accepted inference
policy. The legacy implicit-duplication
classification and the structural independent-copy witness must remain distinct despite
the mode-specific reuse of the `copy` constraint name.

T08 must validate the remaining choices with representative translations. Structural copy,
separation, freeze, lifetime, and cleanup proofs remain required even though copying is
explicit. This decision completes no implementation task and changes no current compiler
or normative-language behavior.

## Remaining gates

Q2 remains partially resolved; Q3–Q8 remain unresolved. V0 has no recorded baseline evidence
yet. The accepted decisions do not authorize general language adoption.
