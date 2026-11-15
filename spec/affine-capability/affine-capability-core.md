# Affine capability core

Status: design proposal. This document defines a candidate core, not implemented Cinder syntax
or a replacement for the current normative specification. Safety claims below are obligations
for a future formalization, not established metatheorems.

## Intent and choices

Surface value passing moves owned values by default. An ordinary by-value parameter receives
the argument's value and cleanup obligations; transferring an affine value makes its source
unavailable. Scalars and other values with checked duplication rules, including retained
frozen handles, may duplicate. A structural copy witness does not make an owner duplicable
or enable implicit independent copying. A function can request a capability such as `own[T]`,
`shared_read[T]`, `inspect[T]`, or `edit[T]`; inspection and edit parameters borrow or reborrow
according to the conversion table below.

Independent copying and freezing are explicit requests. Surface `copy(x)` elaborates to
core `copy_value` and constructs an equivalent value of the same surface type with independent
mutable storage. `copy_owned(h)` materializes represented contents in a fresh owned root and
returns `own[T]` for represented `T`. Both preserve their sources and require the relevant
structural copy witnesses, exposed by the `copy` constraint. This constraint permits explicit
construction and does not change implicit-duplication rules. Copying a frozen handle retains
its existing target; `copy_owned` instead constructs a fresh owner of the target's represented
contents.
Immutable strings and frozen subobjects may share backing storage when lifetime management
is preserved. Freezing consumes an owner
and permanently removes mutable authority over its region and owned descendants.

Move-by-default is a surface elaboration policy. The formal core records transfer, duplication,
copy construction, loans, and freeze explicitly; checking never chooses an implicit deep copy
because a witness exists. See the [Q2 ownership decision](affine-capability/decisions.md#q2--ownership-default-and-explicit-copying).

Conversion annotations are `&x` for observation, `&mut x` for editing, `&>x` for transfer of
an owner or edit handle, and `&!x` for freezing. Unadorned `x` uses the parameter capability
and conversion rules below. `&x` never becomes an edit loan from the expected parameter type.
The [surface mapping](affine-capability/surface.md) records accepted experimental spellings,
reference shorthand, receiver modes, local bindings, and the legacy-sharing boundary.

Local inspection is different: a scoped `inspect` loan permits reading while temporarily
suspending mutation. It does not produce a persistent `read` value. This distinction is
deliberate: interpreting every local read as permanent freezing would prevent ordinary local
construction and update. Semantic region and loan notation remains schematic; accepted
surface spellings are distinguished from pending choices in the surface mapping.

Shareability means permission to duplicate a handle, not permission to outlive its storage.
Persistent shared handles retain their storage; scoped references retain a lifetime obligation.
Thread transfer additionally requires that the representation and external resources support it.

## Core objects

Use abstract region identities `r`, loan identities `l`, types `T`, and immutable value
identities `v`. Each region identifies an allocation; checked named-field paths and range
partitions refine access footprints without creating new allocation identities. Regions
owned exclusively form an ownership forest. Cyclic mutable ownership,
interior mutation, raw pointers, and concurrent mutation are outside this core.

```text
value types A ::= scalar | product(A...) | sum(A...) | sequence(A) | shared_read(r,T)
                | own(r,T) | inspect(l,r,T) | edit(l,r,T)
                | closure(environment, signature) | proof(P)

region state S ::= unique | frozen | dead

operations ::= new | project | load | store | move | take_field | init_field
             | exchange | remove_last | match_move | split_at
             | freeze | copy_value | copy_owned
             | begin_inspect | begin_edit | retire | end_loan | call | drop
```

An `own(r,T)` carries exclusive authority to read, write, transfer, freeze, or destroy the region. It is affine. `shared_read(r,T)` denotes permanently immutable storage and is duplicable.
`inspect` is a duplicable scoped observation loan; `edit` is an affine exclusive loan.
Neither kind of loan owns storage. Loads of nonduplicable contents produce observations or
require an explicit move; reading must not silently duplicate ownership.

Ordinary by-value uses in bindings, assignment, arguments, returns, and ownership captures
transfer affine values; duplicable values follow their checked duplication rules. Parameter
capability requests can instead select borrowing or reborrowing. Fresh expression results
can initialize their destinations directly without an independent copy. Literal immutable
values need no mutable allocation first. Explicit local builders may start with `own` authority.

## Checking judgment

```text
Sigma ; Gamma ; Delta ; Loans ; Carriers ; Facts |- e : A @ c
    => Sigma' ; Gamma' ; Delta' ; Loans' ; Carriers' ; Facts'
```

`Sigma` records region states, representation types, exclusive ownership edges, current
storage versions, and initialization maps for allocated values. `Carriers` also records
initialization and pending cleanup for local and temporary value slots.
`Gamma` contains duplicable values and immutable identities. `Delta` contains affine handles
and abstract capability tokens. Both value contexts describe currently available bindings;
each runtime value binding identifies its carrier. Duplicability does not make a binding
permanently available.
`Loans` records active loans, their modes, target regions, granting authorities, lifetime
dependencies, and suspension state. `Carriers` records live value slots and their loan
dependencies, including anonymous temporaries and stored values. The result occupies a fresh
carrier `c` in `Carriers'`, even before it is bound to a source variable. `Facts` contains
propositions about immutable identities or particular versions of mutable storage.

Sequential checking threads the output state into the next expression. Contexts are not
silently copied when checking subexpressions. Moving an affine handle removes its old binding
and transfers its authority to the result. Dropping it invokes its defined cleanup rather than
merely forgetting a live resource.

## Scoped availability and carriers

A carrier is an identified value slot: a local binding, intermediate result, argument slot,
return slot, aggregate field, or closure capture. `deps(c)` is the set of loans required by
the value in that slot, closed over the loans' granting and lifetime dependencies. An
inspection or edit of loan `l` requires `l`; containing values carry the dependencies of
their stored components. Owning a container does not erase loans stored inside it. For
abstract types, the checked representation contract must expose those dependencies even
when the fields themselves are private.

The carrier invariant covers every live runtime value, including values held by the
evaluation continuation while another subexpression runs. Every loan in `deps(c)` must be
active, with live storage and satisfied lifetime dependencies. A variable lookup requires
an available binding and a live carrier; membership in `Gamma` alone never licenses use
of an expired inspection. Structural carrier descriptions may summarize nested fields,
but do not constitute extra runtime copies or duplicate an affine component.

| Value operation | Carrier transition |
|---|---|
| Duplicate an inspection | Keep the source and create a fresh carrier with the same loan dependencies |
| Duplicate a loan-bearing aggregate or closure | Require duplicable components; register the new components and preserve their dependencies |
| Move an affine value | Transfer its components and dependencies to the destination; invalidate the source binding or slot |
| Bind a temporary, construct an aggregate, capture, pass, or return a value | Transfer or duplicate its carriers according to its capability; never discard dependencies |
| Load a scalar by value | Produce a loan-free result after checking the source access |
| Load an existing shared handle | Produce an independently retained result; do not inherit the source's inspection loan |
| Retire a value | Complete any required cleanup while dependencies remain active, then invalidate its slot and remove its carrier entries |

Duplicating an inspection creates another carrier of the same loan, not a fresh lifetime.
Duplicating an edit is forbidden. Reborrowing instead creates a fresh dependent loan under
the forest conflict rules. Retiring one inspection copy does not expire the other copies.
A scalar snapshot or independently retained shared result may outlive its source loan;
facts about mutable storage still obey the version rules below.

`retire(c)` is an explicit core lifetime operation, inserted only at a point where the
continuation cannot use that value again, including through aliases or cleanup. It removes
the corresponding value binding from `Gamma` or `Delta`. It is not unchecked weakening:
owned resources and retained shared handles still require their defined cleanup, and an
origin needed by another loan cannot be destroyed. `drop` performs the relevant cleanup
and retirement; retiring a scoped handle merely relinquishes that carrier, not its target
storage. The initial core retires a containing value as a whole unless a checked operation
explicitly accounts for its component slots; it cannot forget a captured loan while leaving
the containing closure callable.

`end_loan(l)` requires no live carrier whose dependencies include `l` and no active loan
depending on `l`. It removes `l` from `Loans`, withdraws loan-scoped evidence from `Facts`,
and restores only permissions not suspended by other loans. Immutable snapshot facts may
remain, but carry no permission to access the former target. Loan identities are fresh and
never reused; later inspection of the same region creates a different loan. An expired
binding cannot be revived by reusing its source name, retaining a type annotation, or
checking a later expression under an earlier copy of `Gamma`.

The elaborator makes evaluation order explicit as sequential core bindings. Each evaluated
argument remains in a live carrier while later arguments are evaluated and throughout its
use by the callee. On return, a borrowed result receives its carrier and origin dependencies
before argument carriers are retired. Contracts must account for loans retained in results
or stored elsewhere; a call cannot hide a carrier from its caller's state. A temporary
owner or shared handle used as a loan origin likewise stays live until all dependent loans
end. Extending such a temporary's lifetime requires an explicit binding and cleanup scope
in the elaboration; otherwise an escaping loan is rejected.

Local last-use analysis justifies retirement and loan-ending operations; it does not
silently delete loans during expression checking. When analysis cannot establish that all
carriers are dead, checking retains the loan or rejects the conflicting operation. At a
control-flow join, a binding is available only if available on every reachable incoming
path, and dependency summaries conservatively retain all possible incoming loan origins.
A binding retired on one path cannot become available again at the join. Loop invariants
include carrier availability and loan dependencies as well as affine authority.

## Primitive rules

| Operation | Preconditions | Result |
|---|---|---|
| `new(T)` | Valid initializer | Fresh unique region and `own(r,T)` |
| `project(h,p,mode)` | Valid field/element path; mode permitted by source authority | Capability-dependent view or retained shared handle, as below |
| `load(h)` | Live read authority; permitted read access under the forest conflict rules | Scalar copy, duplicate stored shared handle, or observation under the projection rules |
| `store(h,x)` | Initialized destination; live `own` or `edit` authority; permitted replacement and cleanup | Dispose old contents, install replacement, update storage versions |
| `exchange(h,p,x)` | Initialized destination at path `p`; live `own` or `edit` authority; valid replacement and permitted transfers | Transfer old contents to the result and replacement into the destination; preserve initialization and update versions |
| `remove_last(h)` | Live `own` or `edit` authority over a valid structural sequence; permitted removal | Return `@none` if empty, otherwise transfer the last element into `@some` and shorten the initialized prefix |
| `match_move(v)` | Complete structural sum under direct ownership; permitted consuming elimination | Consume the sum, transfer its active payload to branch carriers, and discharge remaining sum storage obligations |
| `move(h)` | Affine handle available; owner transfer passes the forest conflict rules | Transfer handle; source unavailable |
| `take_field(h,p)` | Initialized structural product field; direct owner authority; permitted field removal | Move field into a result carrier and mark the source field empty |
| `init_field(h,p,x)` | Empty structural product field; direct owner authority; compatible replacement and permitted edge update | Transfer the new value into the field and mark it initialized |
| `begin_inspect(h,l)` | Live read authority over the checked target place; permitted observation loan or reborrow | Scoped observation; preserve the target path and suspend conflicting owner actions |
| `begin_edit(h,l)` | Live exclusive authority over the checked target place; permitted edit loan or reborrow | Scoped edit; preserve the target path and suspend conflicting originating authority |
| `split_at(h,k)` | Live mutable range; checked split position; valid initialized backing and permitted reborrow | Two affine edit ranges partitioning the visible range; preserve backing and granting dependencies, suspend conflicting parent authority |
| `retire(c)` | No future use; cleanup and origin-lifetime obligations satisfied | Invalidate the value slot and remove its carrier entries |
| `end_loan(l)` | No live carrier or dependent loan of `l` | Restore authority only where no remaining loan suspends it |
| `freeze(h)` | Fully initialized unique owner; permitted forest transition; contents and cleanup are recursively freezable | Consume owner, mark region frozen, return `shared_read` |
| `copy_value(v)` | Live read authority; structural copy witness for the value type | Equivalent independent value under the checked-copy rules |
| `copy_owned(h)` | Live read authority; structural copy witness for represented `T` | Equivalent contents in fresh owned storage, with fresh mutable descendants |
| `call(f,args)` | Argument capabilities, loans, and preconditions satisfy the instantiated contract | Apply storage effects, capability transitions, result dependencies, and checked postconditions |
| `drop(h)` | Valid cleanup recipe for the initialized value; forest conflicts and lifetime dependencies permit disposal | Discharge cleanup once, retire the carrier, and reclaim or release storage as appropriate |

An edit loan may create nested observation or edit loans only by suspending its own conflicting
authority until those loans end. This is reborrowing, using the same rules as an owner.

There is no `shared_read -> own` conversion. Even discovering that a frozen handle has one remaining
reference does not thaw it. An independent mutable copy requires `copy_owned` and a fresh region.
Rebinding a variable to fresh storage does not mutate the old frozen value.

Freezing is deep: recursively consume and freeze exclusively owned reachable regions; already
frozen children are allowed. Borrowed mutable children, outstanding mutable aliases, and
opaque resources without a checked freezing contract prevent freezing. A read-only outer
record must not conceal a mutable cell or an operation that mutates its reachable storage.
Reading an external resource does not imply the external world is immutable: such resources
need explicit operation capabilities and cannot automatically become persistent `read` data.

## Ownership forest and loan conflicts

Write `r -> s` when unique allocation `r` contains the exclusive owner of allocation `s`.
Every owned allocation has at most one owning parent, and these edges are acyclic. Owners
held directly by local values or capability tokens are roots. Inline fields use their
containing allocation identity. Shared handles and scoped loans are not ownership edges;
freezing converts exclusive edges into retained frozen edges under the projection rules.

Define the footprint of whole-region authority rooted at `r` and its overlap relation in the current
heap state as follows:

```text
tree(r)       = { r } union { s | r -> ... -> s }
overlap(r, s) = tree(r) intersects tree(s)
```

For whole-region authority, overlap means that the roots are equal or one owns the other
transitively. A loan targeting all of `r` protects `tree(r)`, not merely the bytes allocated at
`r`. Checked place footprints refine this relation as specified below. A parent inspection
prevents editing an owned descendant, and a child loan
constrains operations on every owning ancestor. The checker must prove nonoverlap before
treating abstract regions as independent; different symbolic names alone are insufficient.

An access is checked against every active loan whose footprint overlaps its requested
footprint according to this table:

| Requested access at `r` | Overlapping `inspect` loan | Overlapping `edit` loan |
|---|---|---|
| Read or begin observation | Allowed | Conflict |
| Write or begin edit | Conflict | Conflict |
| Transfer owner, freeze, destroy, or detach owned storage | Conflict | Conflict |

The table describes competing authority. An operation through an edit loan is authorized
by that loan rather than conflicting with itself. A reborrow may also use the authority
of its granting loan and that loan's granting ancestors: the checker records this chain
explicitly and suspends their conflicting permissions for the reborrow's duration.
An inspection can grant only observation; an edit can grant observation or editing.
For unique storage, the reborrow's target must lie within the granting loan's footprint.
Observation of frozen storage may instead follow retained frozen edges under the projection
rules, preserving the granting loan's lifetime dependency and granting no edit authority.
All other loans,
including sibling and dependent loans, still participate in the conflict check. In
particular, a parent edit cannot be used on an overlapping footprint while a child reborrow
is active. Suspended loans remain reservations against unrelated authority; suspension
does not make their storage available to another owner or borrower.

The granting-chain exception authorizes only operations allowed by the source capability.
An edit loan does not thereby gain permission to transfer or freeze its target's owner.
Moving an edit handle transfers the existing loan and its dependencies without ending it
or restoring its grantor; it cannot bypass a suspension imposed by a dependent reborrow.
Ending a loan requires all its carriers and dependent loans to be dead. Authority resumes
only to the extent permitted by every other remaining loan.

An access to a checked place `(r,p)` uses that place's footprint: its inline storage and
the exclusive descendants owned through that place. The empty path covers the entire
allocation and its owned tree. A path overlaps itself and every prefix or descendant path.
Paths that first diverge at distinct named fields of the same structural product are
disjoint, including their exclusively owned descendants. This applies recursively to nested
products and closure fields when projection is permitted by the abstraction contract.
Different names for abstract regions, sum variants, unequal indices, or arbitrary subrange
bounds do not establish separation. Indexed access conservatively covers its containing
sequence footprint unless it inherits a narrower checked partition from `split_at`.
Named-field refinement cannot bypass that conservative indexing rule.

Place provenance includes the resolved target and granting chain, not just a syntactic path:
two fields carrying loans of the same external storage do not make those targets disjoint.
Likewise, following a stored shared handle preserves its frozen target and lifetime rules.
Private representation fields are available only through checked abstraction contracts.

A projection can select a named field or separately allocated owned child without first
borrowing the whole parent. It checks source authority over the selected footprint and the
lifetime of the ownership path, granting no other access to the parent. Sibling loans can
coexist when their footprints are disjoint; an enclosing edit remains usable on unaffected
siblings but cannot perform overlapping accesses. A method requiring an edit of all of
`self` still conflicts with a live field loan. Moving, freezing, destroying, or replacing
the whole parent covers every field and descendant, regardless of field separation.

Changing an ownership edge is also an access, not bookkeeping that bypasses loans.
Insertion or replacement requires write authority for the containing owner slot;
transferring an incoming owner and removing or destroying an outgoing owner must each
satisfy their own conflict checks. Check these conditions in the pre-transition forest,
then require the resulting forest and all surviving loans to remain well formed. No
transition may detach, reparent, destroy, or freeze a live loan's target or invalidate
its ownership path. This prevents removing a child edge first to make a later conflict
check overlook a loan. A permitted update extends an enclosing loan's footprint to any
newly owned descendants; it does not leave those descendants available to outside authority.

Shared edges do not extend an exclusive footprint into the frozen allocation they retain.
Lifetime dependencies are checked separately from overlap: releasing a particular shared
handle that supports a scoped loan is forbidden while that dependency remains live.
This also applies when replacement or destruction of a containing object releases that
handle; passing the forest conflict check alone does not discharge lifetime obligations.
An independently retained shared projection has its own lifetime and does not keep a
loan on its former parent. Dropping that parent may release its handle, but cannot reclaim
storage still retained by the projection. A stored scoped loan likewise keeps its original
target and dependencies; reaching it through another allocation does not change them.

## Capability-dependent projection

Freezing preserves the nominal representation type `T`: there is no recursive `Frozen[T]`
type transformation. Instead, access to a field, element, active sum payload, or closure
capture depends on the capability used to reach it. A declared field type describes its
representation; it does not independently grant the authority to use that representation.
The heap-typing invariant must relate these representations to their region states. In
particular, an `own(s,U)` field in frozen storage is a retained edge to frozen `s`, not an
available ownership token. Its former exclusive authority has been consumed by freezing.

`project(h,p,mode)` selects a valid path `p` through `h` and requests observation, editing,
or persistent sharing. Array paths require bounds evidence and sum paths require evidence
of the active variant. Projection does not move a field or implicitly copy its contents.
For a field represented as `own(s,U)`, the rules are:

| Source capability | Requested mode | Result and obligation |
|---|---|---|
| `own(r,T)` | Observation | `inspect(l,s,U)`; loan prevents conflicting actions through the parent |
| `own(r,T)` | Editing | `edit(l,s,U)`; suspend conflicting parent authority |
| `inspect(l,r,T)` | Observation | `inspect(l2,s,U)`; child loan depends on `l` and cannot outlive it |
| `edit(l,r,T)` | Observation or editing | Corresponding child reborrow; suspend conflicting authority of `l` until it ends |
| `shared_read(r,T)` | Observation | Scoped inspection of frozen `s`; retain the source lifetime obligation |
| `shared_read(r,T)` | Persistent sharing | `shared_read(s,U)`; frozen `s` is independently retained before returning |

All rows require live source authority and no conflicting loans. Inspection never grants
editing or persistent sharing of an owned child, even if analysis discovers that its storage
is frozen. Neither inspection nor shared access exposes the stored `own` token. Editing
permits updates through a loan; projection alone does not extract ownership. `take_field`
requires direct owner authority. The checked `exchange` and `remove_last` operations below
may transfer stored values through an edit while preserving the container's validity; they
do not transfer or freeze the owner of the edited container itself.

A scalar may be loaded by value through any live read authority. A field already represented
as `shared_read(s,U)` may be loaded by duplicating that handle through any live read authority;
the duplicate retains `s` independently of the containing object. This duplicates an existing
persistent capability rather than promoting an inspection into one. Stored scoped loans keep
their original lifetime obligations; accessing them never duplicates an edit token or extends
a loan's lifetime. The initial freezing rule rejects representations containing scoped loans;
supporting their promotion to retained shared edges would require a separate checked conversion.

Inline aggregate projections carry the containing allocation identity and their path; they
do not invent a fresh region. Their observation or edit views follow the same authority
restrictions. A persistent shared projection into frozen inline storage retains the backing
allocation, so releasing the outer handle cannot invalidate the projected value. An allocation
is reclaimed only after its retained projections and other retaining handles are released.
These path-bearing views are schematic extensions of the handle notation above.

Projection composes along a path and never strengthens its source authority, except that
loading an existing shared handle duplicates that handle's own authority. Loans through owned
children also constrain their owning ancestors: dropping, freezing, or replacing an ancestor
cannot invalidate a live projected loan. Projecting an inline named field preserves its
allocation identity while narrowing authority to its checked place footprint; sibling named
fields may have simultaneous loans under the separation rules above.

Abstract types expose only projections and operations justified by the declaring module's
checked capability contract; private fields do not bypass these rules. Closure captures obey
the same projection rules as fields. A closure reached through shared authority is callable
only through a read-only entry point checked with those capture projections. Freezing cannot
reinterpret code checked with writable or consuming capture access as read-only code. The
initial core rejects freezing a closure whose exposed call contract requires edit or ownership
of its environment, or whose captures cannot be frozen. A read-only closure may be frozen only
when its body remains valid under the shared projection rules.

### Borrowed slots and collection views

The experimental surface collapses single-element `cell[T]` and `mut cell[T]` views into
`inspect[T]` and `edit[T]`. These capabilities can target an initialized storage slot as
well as an allocation's represented contents. A path-bearing slot loan has schematic form
`inspect(l,r,p,T)` or `edit(l,r,p,T)`: `r` is the allocation containing the slot, `p` its
checked path, and `T` the type stored there. This extends the handle notation, not the set
of authority modes. The loan also records its granting chain and lifetime dependencies.

The target of `begin_inspect` or `begin_edit` may be a checked place `(h,p)` selected through
live granting authority. Resolve its allocation, bounds, initialization, and permitted
access before creating the loan. An address or a claimed type alone cannot supply this
authority. A collection exposes only slots whose permitted operations preserve its checked
representation and abstraction contracts. Inline element paths conservatively conflict
within their containing sequence or inherited split partition; selecting different indices
alone is not a proof of disjointness.

For an element represented as `own(s,U)`, an `edit[own[U]]` of its slot is grounded in the
containing allocation and permits checked replacement or exchange of that stored owner.
An `edit[U]` of the child instead targets `s` and cannot replace its parent's ownership
edge. Contents projection and slot borrowing remain distinct operations even though both
produce the existing capability types. No expected type may invent the missing authority.

Surface `*slot` designates the loan's checked place without moving its contents. Loads follow
the ordinary capability-dependent projection rules; an inspection of an owner slot never
loads an ownership token by value. `exchange(*slot, incoming)` selects the slot carried by
the edit and invokes checked exchange; assignment through it invokes checked `store` and
cleanup. Neither permits extraction that leaves borrowed storage empty. Exchange checks the
pre-transition ownership forest, replacement validity, loans, effects, and initialization
exactly as when its destination is selected directly from the container.

Returning, storing, reborrowing, or explicitly transferring a slot capability preserves its
place provenance and loan dependencies. A live slot loan prevents collection operations
that invalidate its target or granting chain, including relocation, removal, and destruction.
Retiring the originating handle alone cannot end a loan still carried by a result or field.

Range views retain the surface spellings `slice[T]`, `mut slice[T]`, and `str`. They carry
their range shape and scoped observation or edit authority under the same loan rules.
Range construction records its backing allocation, checked range, and granting dependencies;
it does not create fresh storage or independent region authority. Bounds and initialization
must be established, with UTF-8 boundary checks for text. Observation is the default surface
range operation; editing requires an explicit edit request and valid edit authority. Range
borrow dispatch produces a range capability rather than a loan to a temporary descriptor.

Observation range carriers duplicate only while preserving their loan dependencies. Mutable
range carriers are affine: ordinary value binding transfers them, while declared range
parameters select observation or edit reborrowing for bare arguments. Explicit transfer
preserves the existing loan identity and granting chain. A surviving returned or stored
subrange keeps its origin and conflicting-authority suspension obligations after the helper
or original carrier retires. Range bounds alone never establish disjoint inline edits;
only the checked partition operation below supplies range separation. Element-slot projection
from a mutable range of `own[T]` yields an edit of the stored owner slot, not ownership
extraction or child-only authority.

A scoped range over frozen backing preserves its source lifetime obligation. It cannot
implicitly become independently retained. Existing retained single-value projection rules
do not erase the dependencies of a scoped range.

Single-element capabilities hide collection representation while keeping `T` known; sums
or a separately checked trait-object abstraction provide heterogeneous element types.
Legacy cell semantics remain confined to legacy mode during migration.

### Checked mutable range splitting

`split_at(h,k)` reborrows one mutable range of length `n` into two affine edit ranges with
visible intervals `[0,k)` and `[k,n)`, relative to `h`. Establish `0 <= k <= n` before
creating either result, by checked evidence or a runtime bounds check. A failing check
creates neither child loan and leaves the source loan unchanged; argument evaluation has
its ordinary effects. The operation requires valid source authority, initialized elements,
and no conflicting outstanding loans over the range being split.

The primitive establishes a checked partition identity with two disjoint child footprints,
both dependent on the granting loan and the original backing lifetime. It creates no new
allocation or ownership token and copies no elements. The children cover exactly the
visible source range; arbitrary user assertions or independently computed bounds cannot
mint partition evidence. The primitive rule, including bounds validation, must be proved;
the caller need not supply a disjointness proof.

Each child's element slots and exclusively owned descendants lie within that child's
authority. This does not separate external targets reached through stored loans. Subranges,
returned or stored views, and nested splits preserve partition ancestry and all granting
dependencies. Splitting one child does not suspend its disjoint sibling. Transfers preserve
loan identity and partition evidence; duplicating an edit remains forbidden. Conflicting
parent authority stays suspended while any child or dependent loan survives. Ending one
child restores only authority not blocked by remaining loans; whole-range access resumes
only after all conflicting descendants end.

Control-flow joins and loop summaries preserve these separation relationships and possible
backing origins. A partition identity from one branch or iteration cannot be relabeled to
claim separation from an unrelated loan. If every incoming state does not justify a claimed
separation, the joined state must conservatively treat the footprints as possibly overlapping.

Splits at zero and at `n`, including splitting an empty range, are valid. Empty results carry
their bounds and lifetime dependencies but authorize no element access. Neither child may
grow its visible bounds or relocate, remove, freeze, or destroy the backing storage. Slot
replacement retains the ordinary representation, cleanup, and pre-transition forest checks.
The operation introduces no mutable text view or exception to UTF-8 validity rules.

## Retained frozen ranges

The surface types `shared_slice[T]` and `shared_str` are range-shaped retained frozen
projections, distinct from scoped `slice[T]` and `str`. Their descriptors identify checked
backing storage and a valid range; text additionally requires UTF-8 boundaries. They retain
the storage needed to keep that range live without inventing a fresh semantic region.
Concrete descriptor and control-block layout remain subject to the runtime ABI design.

The ordinary library method `.slice(range)` on a suitable persistent frozen source produces
an independently retained range through an explicitly contracted intrinsic. The intrinsic
requires live persistent frozen authority, checked backing provenance, valid bounds, and
initialized storage, plus text-boundary validity where applicable. Checked library code
establishes the public range checks and invokes the primitive with its preconditions
discharged. The primitive establishes retention before exposing the result; a bare address
and count are insufficient. The result preserves frozen storage identity but has no loan on
the source handle, so releasing that handle or returning from a helper does not invalidate
the result. Retained subranges use the same rules and stay within the source's visible range.

Creation preserves the source and copies no elements. Duplication retains the same range
under the frozen-handle rules and does not require an element-copy witness. Element
projection never exposes mutable or owning authority, including for represented owner
edges in frozen storage. Final release uses the frozen release recipe and waits for every
retaining handle/projection and scoped lifetime dependency; a small range can keep a whole
backing allocation alive. Scoped indexing of a retained range still produces a scoped loan.

This is a library API over checked retained projection, not a universal `retain_range`
builtin. Scoped observation or edit authority cannot invoke it to acquire persistent
retention, even when the target is known to be frozen. A stored existing frozen capability
may still be loaded under the normal projection rules. Neither this operation nor its
library wrapper implicitly freezes, thaws, copies elements, or materializes an owner.

## Checked value copying

Semantic copying, capability duplication, and ownership transfer are distinct operations.
`copy_value(v)` preserves the source and constructs an equivalent value with independent
mutable storage. Duplicating `shared_read` or `inspect` preserves the target identity and
obeys that capability's retention or loan rules. Transferring `own` moves its existing
authority. An affine owner can support copying its contents into fresh storage without
becoming a duplicable owner; copying never produces a second owner of the original region.

Explicit independent copying or materialization requires a structural copy witness for `T`.
Ordinary ownership transfer does not require one, including for generic parameters. The initial core
derives this witness from the type's representation and checked component operations; a
user function named `copy`, `copy_owned`, or `clone` does not supply one by declaration alone.

The experimental surface names this structural requirement `copy`. For `copy(x)`, it applies
to the source value type; for `copy_owned(h)`, it applies to represented `T`. An owner with a
witness still moves by default. Implicit duplication is a separate compiler-tracked property,
not a second public constraint named `copyable` or `duplicate`. In particular, an inspection
can duplicate under its loan rules without satisfying `copy` in this initial core.

This meaning replaces the legacy `copy` concept's implicit-duplication classification only
in the experimental mode. Legacy bounds cannot supply structural witnesses; migration must
check the operations each generic body actually needs. Ordinary mode retains its existing
semantics until adoption.

The witness specifies a value relation `equiv_T` and a construction recipe with these obligations on
every successful return, where `H_in` and `H_out` are the input and output heaps:

```text
equiv_T(source in H_in, result in H_out)
mutable_reach(result, H_out) subset_of fresh_regions(H_in, H_out)
```

`mutable_reach` includes all mutable storage reachable through the result's representation
and capabilities, including private fields and closure captures. Every such region must
be newly allocated for this copy and exclusively owned by the result's ownership forest.
It must be disjoint from all pre-existing mutable storage, not just from the source root.
Any backing storage shared with pre-existing values must be frozen and independently
retained. Neither a fresh outer allocation around an old mutable child nor a borrowed
pointer into that child establishes independence.

The source's contents, ownership relationships, capabilities, and storage versions are
preserved. Construction may allocate and initialize fresh storage, read through valid
observation authority, and duplicate retained frozen handles. It may not consume or freeze
the source, write pre-existing value storage, or perform undeclared external operations.
Shared retention bookkeeping is permitted; it does not change the frozen value's contents.
Copying requires a stable observation of the source for the construction's duration, so
conflicting edits and cleanup are rejected under the usual loan rules. On a modeled failure
exit, partial construction is cleaned up without changing the source or leaking authority.

`equiv_T` is a representation relation, not a user-supplied equality operator or an arbitrary
claim that two programs behave alike. Its initial structural cases are:

| Representation | Copy construction and equivalence obligation |
|---|---|
| Scalar | Preserve the exact scalar value, including any observable representation distinctions |
| Product, sum, or sequence | Preserve every field, the active variant, sequence length and order; recursively copy the represented components |
| Owned child `own(r,U)` | Observe its contents and recursively construct an equivalent child with a fresh owner; never copy its ownership token |
| `shared_read(r,U)` | Duplicate and retain the same frozen target and projection path |
| Closure | Preserve the code entry points and recursively copy the environment; its checked contract must remain valid for the fresh capture regions |
| `proof(P)` | Preserve its proposition and dependencies, or check a derivation for any required fresh identities; never manufacture authority |
| Scoped `inspect` or `edit` | No structural value-copy witness in this initial core |
| Opaque resource or abstract capability token | No automatic witness from its name or physical layout |

At owned positions, equivalence relates contents across distinct allocation identities; it
does not assert equality of the source and destination regions or their authority. The
fresh owner comes from allocation. For frozen source representations, the same contents
relation applies even though access to the source uses shared projection rules.

Structural recursion follows the finite represented value and its acyclic ownership forest.
An inductive type can have a finite recursive copy recipe; executing it may take time
proportional to the value and may fail to allocate. Validating the recipe uses structural
rules, not evaluation of arbitrary user code inside the proof kernel. Closure checking
requires the same code and a valid substituted contract, not a proof of arbitrary function
equivalence.

Abstract data types may expose a witness derived inside their declaring module from their
private representation. Every represented field participates; private fields are not exempt
from equivalence or independence. Custom representation changes, cache omission, and custom
notions of value equality require additional proof rules and are outside the initial core.
A protocol token or external resource does not become copyable merely because its private
bytes can be copied: fresh heap storage does not create fresh authority over an external
resource. Such values use their explicit operations or capability passing modes.

Scoped handles still follow their explicit capability rules: inspection duplication keeps
the same loan dependencies, and editing requires transfer or reborrowing. Aggregates or
closures containing scoped loans may be duplicable when their components permit it, but
are not eligible for independent value copying in this initial core. A caller can explicitly
observe a referent and copy its represented data when that data has a witness; the resulting
type and authority are for the new data, not a duplicated or extended loan. Likewise, proof
data mentioning concrete regions or versions cannot be silently retargeted to copied
storage. A copied representation that needs such evidence requires a checked derivation
for its fresh identities; otherwise its copy witness is rejected.

`copy_owned(h)` uses the same structural obligations to materialize the represented `T` in a
fresh owned root. When its source is frozen, it observes through the capability-dependent
projection rules and reconstructs owned fields with fresh regions, even if those source
fields are retained frozen edges. Fields declared `shared_read` retain frozen targets.
This creates new mutable authority from allocation, not by extracting or thawing source
authority. In contrast, copying a `shared_read` handle as a value only duplicates the
handle; it does not invoke this materialization operation.

Surface `copy(x)` selects core `copy_value`; `copy_owned(h)` selects the fresh-owner operation.
Both observe their sources without consuming them; edit sources require valid observation
reborrows during construction. These source-preserving operations are explicit requests,
not ordinary by-value argument transfers. A copied `shared_read[T]` handle needs a witness
for the handle value, while `copy_owned` through that handle needs a witness for represented
`T`. A scoped handle has no independent value-copy witness in the initial core, even when
its referent can be materialized with `copy_owned`.

Explicit `copy_value` and `copy_owned` requests use these obligations. Preserving an owner while
sharing its contents requires explicitly constructing an independent copy and then checking
freezing on that copy. Neither an owning parameter nor a sharing parameter implicitly selects
such construction. An arbitrary user-defined function, including a legacy `clone` method,
remains an ordinary explicit call with its own effect and result contract; its name does
not authorize a structural recipe.
The formalization must establish that the
structural recipe preserves `equiv_T` and mutable separation. The bounded arithmetic proof
kernel alone does not establish those heap properties.

## Capability requests and conversions

A prototype declares each requested parameter capability; otherwise its parameter is an
ordinary by-value parameter. Callers may write `&`, `&mut`, `&>`, or `&!` to request observation,
editing, transfer, or freeze. Unannotated owners transfer to owning parameters; `&>` can state
that transfer explicitly and is required to transfer an existing edit handle instead of reborrowing it
for an edit parameter. An incompatible explicit annotation is an error: `&` cannot satisfy
an edit parameter, and `&mut` requests an edit rather than silently selecting observation.
Conversion spellings and the user-facing `copy`/`copy_owned` names are accepted for the
experiment. There is no default `copy[T]` parameter mode.

| Requested capability | Available value | Conversion |
|---|---|---|
| Ordinary by-value `T` | Affine value of the matching type | Transfer its carrier contents and cleanup obligations; consume source; no copy witness required |
| Ordinary by-value `T` | Value with a checked duplication rule | Duplicate under that rule; retain frozen storage and preserve any loan dependencies; never independently deep-copy mutable storage |
| `own[T]` | `own(r,T)`, unannotated or with `&>` | Transfer; consume source; no copy witness required |
| `own[T]` | Explicit `copy_owned(h)` result | Transfer the fresh owner produced by checked materialization; preserve `h` |
| `shared_read[T]` | `own(r,T)`, unannotated or with `&>` | Rejected: sharing requires explicit freezing; transfer alone does not freeze |
| `shared_read[T]` | `own(r,T)` with `&!` | Freeze original; consume source |
| `shared_read[T]` | Existing `shared_read(r,T)` | Duplicate shared handle |
| `inspect[T]` | Owner or shared handle, unannotated or with `&` | Begin scoped observation loan |
| `inspect[T]` | Existing `inspect(l,r,T)`, unannotated | Duplicate the inspection with the same loan identity and dependencies |
| `inspect[T]` | Existing `inspect(l,r,T)` with `&` | Begin a fresh observation reborrow dependent on `l` |
| `inspect[T]` | Existing `edit(l,r,T)`, unannotated or with `&` | Begin a fresh observation reborrow; suspend conflicting access through `l` |
| `edit[T]` | Owner, unannotated or with `&mut` | Begin exclusive edit loan |
| `edit[T]` | Existing `edit(l,r,T)`, unannotated or with `&mut` | Begin a fresh edit reborrow; suspend conflicting access through `l` |
| `edit[T]` | Existing `edit(l,r,T)` with `&>` | Transfer the existing edit handle and its dependencies; invalidate the source binding |
| `edit[T]` | Any argument explicitly annotated `&` | Rejected: the explicit observation request cannot grant editing |
| `inspect[T]` | Any argument explicitly annotated `&mut` | Rejected: the explicit edit request does not match the observation parameter |

All conversions require their relevant conditions: explicit copying must be supported,
freezing must be valid, and loans must not conflict. `&x` begins a fresh observation loan
or dependent observation reborrow; `&mut x` begins a fresh edit loan or dependent edit
reborrow when the source permits it. `&>x` transfers only; `&!x` freezes owned storage.
For a sharing parameter, `publish(x)` is rejected when `x` is an owner,
`publish(&!x)` consumes and freezes the original,
and `publish(&!copy_owned(x))` explicitly materializes and freezes a copy while preserving `x`.
Neither freezing nor independent copy construction is inferred from the requested type.
A composite containing an uncopyable resource can transfer to an owning parameter without
an annotation or a copy witness, or be lent to a suitable `inspect` or `edit` parameter.

An inspection cannot satisfy an edit parameter, regardless of annotation. These conversions
do not promote scoped handles to ownership or persistent sharing, and do not provide a
structural value-copy witness for a scoped handle. Duplication of an unannotated inspection
uses its capability duplication rule and preserves the same loan dependencies.

Reborrowing preserves the caller's handle and creates a dependent argument loan under the
forest conflict rules. The caller regains only permissions no longer suspended after every
carrier of that reborrow has retired and all dependent loans and the reborrow itself have
ended. Returning from a call alone does not end it: a returned or stored derived loan keeps the dependency
active, including when the callee returns an error. Sequential helper calls can therefore
reuse an edit parameter when neither call retains a loan:

```text
twice(e: edit[Buffer]):
    push(e, 1)  // fresh edit reborrow ends after the call
    push(e, 2)  // e may be reborrowed again
```

Passing `&>e` to an edit parameter instead moves the existing handle without creating a new
loan. Its identity, granting and lifetime dependencies, and suspension state are preserved;
transfer cannot bypass a live reborrow's restrictions or grant ownership of the target.
The source binding remains unavailable after the call. If the callee returns the handle,
the caller may bind that returned carrier explicitly; return alone does not revive the source.

## Calls, lifetimes, and control flow

A function signature specifies argument modes, result capabilities, borrowed result origins,
abstract token transitions, and the storage-effect contract below. Its body is checked
against that contract once. Call checking uses the contract without inspecting the
implementation.

Parameter conversions follow the capability conversion table. Ordinary owning arguments
transfer without copying; preserving their source requires an explicit copy or a loan-taking
interface. Freeze is explicit, and duplication preserves the relevant retention or loan rules.
An explicit scoped observation parameter accepts `inspect` without freezing. Any returned
loan names its originating parameter region in the core signature; the surface language may
infer this information. Its dependency description distinguishes carrying the incoming loan
from creating a dependent reborrow, so call instantiation preserves the actual argument
loan's identity and granting chain. A loan of a function-local allocation cannot escape.

Loan carriers and dependent loans follow the scoped availability rules above, including
temporary argument and result slots. Lifetime inference is not a promise to accept every
semantically safe program.

At a branch join, an affine authority is available only if it survives in every reachable
branch. A region is writable afterward only if every branch preserves unique write authority.
Predecessor edges must first reconcile initialization and cleanup obligations as specified
below; taking the intersection of available bindings alone must not leak a resource.
If one branch freezes and the other retains an owner, explicitly freeze that other owner at
the join when valid, or reject a requested common representation. Never restore lost authority.
Loops require a stable capability invariant on every back edge; freezing an owner in one
iteration cannot be followed by writing it in the next. Early returns perform the cleanup
required by their exit states. Exceptions and cancellation need equivalent explicit exits
before they are admitted to this core.

An aggregate derives capability duplication from all its fields; independent value copying
instead requires the structural witness above. A closure is an aggregate environment
plus checked code: a consuming call takes its environment by ownership, a mutating reusable
call takes an edit loan, and a read-only reusable call takes observation authority. Capturing
a loan preserves its lifetime obligation; capturing ownership preserves affine usage.

The accepted experimental surface exposes these entries as `fn(self, ...)`,
`fn(mut self, ...)`, and `fn(self: own[Self], ...)`. Infer the required entry authority
from a source lambda body and check optional explicit constraints. The environment receiver
is implicit at invocation and separate from how a higher-order function receives the
callable: borrowing an editing entry permits reusable calls, and taking ownership of that
callable does not make each invocation consuming. Reborrowing a captured edit requires
editing access to the environment; moving a captured owner out requires consumption.
An observing entry may still write through an explicit edit argument. Omitted surface
receivers preserve sufficient checked or sound conservative call-mode information; they
do not establish an observing entry. Call-mode information must survive stored/forwarded
callables and portable summaries without exposing private capture layout.

Experimental capture lists use value capture `[x]`, observation `[&x]`, editing/reborrow
`[&mut x]`, and no captures `[]`. An explicit list limits the available enclosing locals;
without a list, capture required variables by value under the same rules. Affine values
move and other values duplicate only under checked capability rules; all stored loans and
cleanup obligations remain accounted for. Neither escape analysis nor an expected callable
constraint silently changes value capture into borrowing. Explicit borrowed captures must
satisfy their own lifetime obligations, including when the closure escapes.

## Initialization and cleanup

Every initialized resource-bearing slot has a cleanup obligation, including a duplicable
shared handle and each of its runtime copies. This obligation is distinct from permission
to use an affine value at most once. A move transfers the obligation; it does not discharge
it. Cleanup discharges it exactly once on a completed exit. A moved or already cleaned slot
must never be cleaned again. Scalar disposal is trivial, and scoped-handle disposal retires
its carrier without destroying its target.

Initialization maps distinguish initialized fields from empty fields, with an internal
cleaning state while a cleanup recipe runs. These are value-slot states, not new region
states. Whole-value loads, borrowing, calls, copying, freezing, and ordinary whole-value
moves require a fully initialized value. A partially initialized owned allocation retains
its owner solely for permitted field operations and cleanup; its representation cannot be
exposed as a complete `T`. Initialization of a slot publishes its carrier, ownership edge,
and cleanup obligation together. Moving or disposing it removes them consistently.

In the initial core, `take_field` supports fields of structural products whose containing
products have no custom whole-value cleanup. It requires direct ownership of the containing
allocation or local aggregate, an initialized selected field, and the forest and lifetime
checks for removing that field. It transfers the field's value, loan dependencies, and
cleanup obligation to the result and leaves an empty slot. Independently initialized
remaining fields can still be selected, subject to checked place-footprint loan conflicts.
`init_field` fills an empty slot without cleaning it first. Only after all fields are filled
may the parent be used as a whole again. Both operations update ownership edges and storage
versions. Extraction that leaves an empty field through `edit`, `inspect`, or `shared_read`
is forbidden. Checked exchange and sequence removal through `edit` preserve a complete
container under the rules below.

Arbitrary partial moves from sums, sequence elements, or values with custom whole-value
cleanup are rejected in this core. The structural exchange, removal, and consuming sum
elimination rules below are specific permitted operations, not general partial-move authority.
Whole variant replacement and whole-value transfer remain possible. During construction, a
private initialization map tracks fields completed so far. If a modeled failure occurs before `T`
is complete, structural cleanup disposes only those initialized components; a destructor
requiring a complete `T` is not invoked. A constructor must keep every acquired resource in
a tracked slot with valid partial-construction cleanup, even when its fields are interdependent.

Overwriting an initialized slot with `store` is replacement, not a byte write that forgets
the previous contents. Its elaboration is:

1. Evaluate the replacement into a live temporary carrier while the old value remains
   initialized and its destination authority remains available. Evaluation that consumes
   the destination is not an initialized-slot replacement. Apply all evaluation effects
   and check authority, loan conflicts, and ownership-edge changes before removing the old value.
2. Run cleanup for the old value, keeping the replacement carrier and its dependencies live.
   A replacement borrowing from the old value cannot survive this step and is rejected.
3. Install the replacement, transfer its carrier and cleanup obligation, and update versions.
   No user code may observe the destination's intermediate empty state.

Initializing an empty product field uses `init_field` instead; initializing an empty local
binding likewise installs its value without cleaning nonexistent prior contents.
Replacing an owned field through an edit with `store` disposes the old child; returning the
old child instead requires `exchange`. A self-move cannot pass as replacement of an initialized
slot; an explicit extraction followed by initialization must instead account for the empty
state and every intervening exit. If replacement evaluation fails on a modeled failure exit,
the old value remains initialized and any partial replacement is cleaned according to its
construction map.

### Checked exchange, removal, and sum elimination

The core provides these structural operations, with schematic signatures:

```text
exchange(edit slot: T, incoming: T) -> T
remove_last(edit xs: sequence[T]) -> option[T]
```

These are core carrier operations: `incoming` and the returned values transfer their stored
components and cleanup obligations, following move-by-default value passing. Neither
operation requires `T` to be copyable. An affine replacement transfers by default, with
`&>` available to state the transfer explicitly; an owner can supply the edit through the usual
loan conversion. Inspection and shared authority cannot authorize either operation.

The destination slot of `exchange(h,p,x)` is selected by a checked path `p` relative to the
granting owner or edit `h`. Selecting an owned field for exchange addresses its slot in the
containing allocation; it is not the projection that lends the child allocation's contents.
An edit targeting the child alone cannot exchange the owner stored in its parent. An empty
path exchanges the target's contents while preserving the target allocation and its owner.

`exchange` first evaluates the replacement into a live temporary while the destination stays
initialized. It checks destination authority, bounds or field validity, loan conflicts,
ownership-edge changes, and the validity of the replacement in its containing representation.
Then one core transition transfers the old value into the result carrier and the replacement
into the destination. The old value is not destroyed: its cleanup obligation belongs to the
result. The destination is initialized before and after the transition. No user code or
recoverable failure can occur between removal and installation; any fallible preparation
for result storage precedes this commit and follows the ordinary outcome contract. A
self-move, or a replacement borrowing the old value when that loan prevents its transfer,
is rejected.

A structural sequence has a length, capacity, backing storage, and this initialization
invariant:

```text
0 <= length <= capacity
exactly slots [0, length) contain initialized elements
slots [length, capacity) have no element cleanup obligations
```

This valid initialized prefix makes the sequence a complete value; unused capacity is not
a partially moved logical element. Whole-sequence borrowing, copying, and freezing require
this invariant and valid live elements. Element access is bounded by length, not capacity;
copying and freezing visit live elements, and cleanup disposes only those elements before
releasing the backing storage.

`remove_last` returns `@none` when length is zero without changing the sequence. Otherwise,
one core transition transfers the last element into `@some`, marks its former slot empty,
and decrements length. Result storage is prepared before commit under the same failure rule
as `exchange`. No element destructor runs during removal, and the sequence remains valid
because the empty slot is outside the new initialized prefix. The backing allocation and
its owner remain with the sequence.

Both operations check the pre-transition forest and the resulting state. The granting edit
and its granting chain authorize the update under the existing exception; competing loans
still participate. A live loan targeting the removed owned child prevents its transfer.
Carriers, loan dependencies, ownership edges, and cleanup obligations move together. An
owned child transfers from the container to the result's ownership description, without
duplicating its owner. Stored loans retain their original origins. Versions of the changed
container and affected ancestors are refreshed under the existing footprint rules; a moved
child's contents are not thereby copied or changed. This transfer creates no additional
field or range separation beyond the checked place and partition rules.

Abstract containers expose these operations only through a checked representation contract
in their declaring module. Operation bodies must preserve the representation invariant and
account for every cleanup obligation, including obligations required by custom container
cleanup. A method named `pop` or `exchange` grants no additional permission, and these rules
do not permit skipping a custom destructor or arbitrary partial extraction. The structural
transitions are checked by the ownership and initialization rules, not asserted as user
axioms in the arithmetic proof kernel.

`match_move` consumes a complete structural sum, such as an owned `option[T]`, and selects
its active variant. It requires an owning local value or direct ownership of its allocation,
with the usual transfer and lifetime checks. The active payload transfers to branch carriers;
unused payload components receive explicit cleanup. The sum carrier is invalidated and any
remaining backing storage is reclaimed once, without also destroying the transferred payload.
No partially initialized sum remains available. Sums with custom whole-value cleanup do not
automatically support this operation; they require a separate checked decomposition rule.
Matching an inspected or shared sum only observes its payload and cannot use `match_move`.

### Cleanup recipes and schedules

For unique storage, a structural cleanup recipe disposes initialized fields in reverse
declaration order, including fields of the active sum payload, sequence elements in reverse
index order, and closure captures in reverse capture order, then reclaims the allocation.
Moved fields are skipped. Cleanup must retire
loan carriers and end dependent loans before destroying their origins. If the specified
order cannot meet those obligations, the program needs an explicit valid disposal sequence
or is rejected; the checker does not silently reorder observable cleanup.

A custom unique cleanup recipe is checked as a consuming operation with explicit effects
and a fully initialized input. It receives private authority over that unique value while
the caller's carrier is unavailable. It must account for every field obligation and the
allocation itself, using checked helpers or structural disposal for fields; the compiler
does not also apply a second unconditional field cleanup afterward. It cannot resurrect
the value, publish an owner or loan of the allocation being destroyed, or leave a resource
obligation unaccounted for on return. Recursive field cleanup uses the fields' own recipes,
not another invocation of the enclosing value's destructor.
Internal disposal steps update the initialization map as they consume fields. This private
teardown permission does not enable ordinary partial moves out of a custom-cleanup value,
and the incomplete value cannot be passed to code requiring a complete `T`.

Cleanup in this initial core cannot throw, cancel, or return a recoverable error; such exits
require a separate unwinding design. A recipe may diverge, so affinity promises no general
eventual-cleanup theorem. Every completed exit, including an error return, must account for
its obligations. Cleanup effects participate in the call-footprint rules and can invalidate
facts even when the surrounding operation performs no explicit store.
Here a modeled failure exit is explicit core control flow that performs checked cleanup and
produces an ordinary error variant at the failing operation's boundary. It obeys the outcome
contract below; it does not bypass call checking or imply support for implicit exception
unwinding or process-abort recovery.

Scope exits use an explicit cleanup schedule, normally reverse binding introduction order;
temporaries have explicit cleanup points in the elaboration. Early return first transfers
the result and its dependencies to the return slot, then cleans remaining locals in that
order. A result borrowing a local prevents the required local cleanup and is rejected.
Last-use analysis can retire a no-effect loan carrier early. It cannot move observable
resource cleanup earlier merely because there are no further direct reads; that requires
an explicit disposal point or a checked justification that effects and lifetimes are preserved.

At branch joins, predecessor edges normalize to a common initialization shape and compatible
capability state. If a binding or field was moved on one path and will be unavailable after
the join, the other paths must explicitly dispose their still-initialized instances before
joining. Alternatively, all paths may initialize a common result slot with a compatible
value. The initial core does not leave hidden conditional cleanup obligations behind an
unavailable binding. If edge cleanup conflicts with a surviving loan, normalization fails.
Reconcile loan carriers, cleanup effects, and facts after these edge operations. Loop back
edges obey the same rule and must re-establish the initialization and cleanup invariant;
break and return edges discharge the obligations of scopes they leave.

Frozen storage uses a separate structural release recipe. Releasing a shared handle consumes
only that handle's retention obligation. Final reclamation is permitted only after every
retaining handle and projection has been released and no scoped lifetime dependency remains.
It releases the frozen allocation's retained outgoing edges once and reclaims its backing
storage; a child retained elsewhere stays alive. Inline projections retain their whole
backing allocation and do not trigger separate cleanup of an inline field.

This release recipe never reconstructs `own` or `edit`, invokes mutable field destructors,
or thaws the allocation. Reference bookkeeping and deallocation are privileged runtime
operations justified by liveness, not user write authority over frozen contents. Freezing
requires a recursively checked structural release recipe as well as freezable contents.
A type with mandatory custom unique cleanup is therefore not freezable in the initial
core; freezing must not silently discard its destructor or defer it until the last shared
release. Custom frozen finalizers or conversions that discharge such obligations before
freezing require additional checked rules and are outside this core.

Release-only custom cleanup and nonfreezable scalar interior-mutation cells are deferred.
A future cleanup extension must check recursive release obligations and preserve observable
cleanup semantics; a label alone cannot authorize freezing. A future interior-mutation
extension must specify alias authority, fact invalidation, callback effects, and closure
sharing without weakening permanent freeze. Neither extension is assumed by library migration.

## Call footprints and fact preservation

Each callable contract includes a finite summary of possible storage effects and any proved
preconditions and postconditions. Footprints name symbolic region roots or checked region
paths supplied by parameters, closure captures, or abstract capabilities. Instantiation
substitutes the actual region identities after argument evaluation and conversion. An
abstract capability may expose an opaque region identity for this purpose without exposing
its representation. The summary distinguishes:

| Contract component | Meaning |
|---|---|
| `writes W` | Upper bound on pre-existing mutable storage the call may modify, including owned descendants of each named root |
| `transitions X` | Allowed changes to ownership edges, region states, and capability availability, including freezing, destruction, and transfer |
| Output-state and result description | For each outcome, surviving and fresh region identities, ownership relationships, initialization, available capabilities, cleanup obligations, and all retained loan dependencies |
| `requires P` / `ensures Q` | Checked propositions over input versions, output versions, immutable values, and result identities; postconditions may be specific to an outcome |

These summaries bound behavior; they do not grant authority. Every write and transition
still needs the corresponding capability and must pass loan-conflict and lifetime checks.
Checked field paths and range partition provenance refine access footprints across calls.
A whole-receiver requirement still borrows the whole receiver; an implementation cannot
silently narrow it by inspecting the callee body at a call site. Contracts and their imported
summaries preserve any field or partition separation needed by the caller.
`W` includes writes performed transitively by callees, callbacks, and cleanup. Contracts for
indirect calls and higher-order parameters carry such summaries too; an implementation
cannot be substituted if it exceeds the declared effects or fails the promised result and
postcondition contract. Recursive bodies are checked against their declared summaries,
including recursive call sites, rather than discovered by unbounded unfolding.
The transition and output-state descriptions must account for every return, including error
variants; a conservative write footprint alone does not specify which owners, regions, or
loans survive a call.
Writes to an incoming region must be covered even if the body first attaches it beneath
another owner. Fresh internal allocations need no input-region entry in `W`, but any that
escape must appear in the result or output heap description.

Recoverable failures are ordinary sum-valued returns. A callable returning multiple outcome
variants supplies an output-state description and any proved postconditions for each variant.
For example, using schematic contract notation:

```text
update : edit(r,T) -> result[U,E]
  writes W
  transitions X
  returns @ok(u)    => state S_ok,    ensures Q_ok
  returns @error(e) => state S_error, ensures Q_error
```

`W` and `X` bound effects across every outcome, including cleanup and writes later undone.
Each return edge completes its required callee-side cleanup before establishing its output
state and postconditions. An error return does not implicitly restore input contents or
authority. Any rollback guarantee must be checked. If a call replaces an owned child and
then returns an error, its error state must describe the replacement and disposal of the
old child. If it stores a loan into caller-owned storage before returning an error, its
error state must retain that loan's dependencies even though the result payload is an error.

Before the outcome is discriminated, only guarantees common to every possible outcome are
available. The checker retains the alternative output states and conservatively protects all
possibly retained loans and lifetime origins; an undiscriminated result cannot hide a loan
from conflict checking or cleanup. Matching a variant selects its output state and justified
postconditions. These are alternatives in the checking state, not duplicated runtime owners.
Intervening operations and cleanup must be valid in every remaining alternative and update
each one; matching cannot restore authority or facts invalidated since the call. Discarding
the outcome or joining branches requires the existing capability and cleanup normalization,
so incompatible states cannot leave hidden cleanup obligations behind unavailable bindings.

For source definitions with checked bodies, omitted effect, result-origin, and outcome
clauses request sound, conservative contract inference, including for public definitions.
Programmer-written clauses are optional constraints that every implementation must satisfy.
The elaborated interface contains a complete checked summary either way; callers do not
inspect bodies. Inferred summaries can change with an implementation and require dependent
code to be rechecked. Explicit constraints can pin the guarantees an author wants to preserve.

Inference need not determine a single exact origin: a sound union of possible dependencies
is valid when it accounts for all outcomes and cleanup obligations. Recursive groups may use
bounded inference or conservative candidate summaries, but every body must establish its
summary before the group is accepted. If inference cannot establish a sufficient contract,
request only the missing information. Native or opaque boundaries need sufficient explicit
contracts or sound interface defaults; unavailable bodies cannot justify extra precision.

The accepted surface spelling for an explicit empty write bound is `writes nothing`.
Reject `writes {}`; omission requests inference, not an empty constraint. This spelling
does not alter the internal finite-set representation of the may-write footprint.

An omitted write annotation is not an unchecked `writes nothing` promise. When no narrower
checked summary is available, the conservative write bound covers all mutable storage reachable
through authority the call may exercise, including authority
inside captures, stored edit loans, and sealed tokens. Such reachability follows those
capabilities' targets as well as ownership edges; it is not just the ownership tree of the
outer argument. A call whose permitted mutable reach cannot be bounded by its interface is
rejected in this core. Mutable global or external state requires an explicit capability and
effect contract; it cannot be an undeclared source of mutation. External-state facts need
their own stability contract and are not covered by heap-version preservation alone.

Callable result-origin bounds bind formal arguments and are instantiated independently for
each invocation. An input-only bound excludes dependencies supplied only by captures,
unrelated storage, or arguments from earlier calls. It still permits captures used solely
for computation. Forwarding or storing callable values preserves sufficient checked
summaries; the result acquires all backing, granting, and lifetime dependencies before
argument carriers retire. Consistently renaming formal binders does not change a contract.
Omitted origin annotations do not establish an input-only bound or erase dependencies.
See the accepted [callable surface mapping](affine-capability/surface.md#callable-parameter-names-and-result-origins)
and [callback translation](affine-capability/translations.md#worked-example-callback-result-origins).

In a callable-type contract, the surface root `self` denotes the environment and authority
reachable through its captures. A bound such as `writes {self}` therefore includes targets
of captured edit capabilities, not only owned descendants or environment bytes. An enclosing
contract may refer to this footprint through its callable parameter, such as `writes {choose}`.
Instantiation preserves actual region identities without exposing private capture layout.
An input-derived result need not retain a counter loan merely because the callback wrote
the counter; any environment or other carrier retaining that edit still keeps its loan live.
The write bound neither grants authority nor chooses an environment call mode.

The origin bound `from self` permits dependencies supplied through the callable environment;
`from {part, self}` permits dependencies from the input, the environment, or both. It does
not imply that all results borrow environment storage. Borrowing a captured owner requires
the dependencies supporting that ownership to remain live. Returning a duplicate of a
captured observation preserves its original target, granting chain, and dependencies;
it need not retain the environment when the checked summary preserves that relation.
Opaque or less precise interfaces may conservatively retain additional dependencies, never
drop actual ones. Portable summaries must preserve enough of these relations to justify
their claimed precision without exposing capture layout. A consuming call cannot return
a loan of captured storage destroyed by invocation cleanup merely by declaring `from self`.

Diagnostics explain the inferred bound or missing relationship, its source operations, and
why it prevents the requested use. Show the smallest relevant annotation or code change and
where it belongs. A suggested constraint remains an obligation to check; it cannot assert
away a real conflicting loan, effect, or ownership transition. Inference never silently
inserts independent copies, freezes, or stronger authority to make a program pass.

An empty write summary must be checked against the whole body and its transitive calls.
It rules out writes to pre-existing storage; declared transitions still apply, and it does
not rule out allocation, divergence, or other effects. An `inspect` parameter alone does
not make a function read-only if it also receives
an edit capability or invokes a mutating closure. Conversely, an edit parameter may have a
checked empty write summary; acquiring its exclusive loan still requires the usual conflict
checks. Writing an explicitly constructed independent argument copy affects that copy's
regions, not the original mutable regions; any separately carried capabilities retain their
own effects. Passing an owner by move preserves its region identity, so callee writes and
cleanup must be tracked against that incoming region even though the source binding is gone.

For fact checking, `version(r)` identifies the current observable contents of `r` and its
exclusively owned subtree. A write conservatively refreshes versions for its whole permitted
allocation footprint and all owning ancestors. This versioning may remain allocation-wide
even when access authority is field- or partition-specific: disjoint access does not itself
promise preservation of a fact about a sibling. Thus changing a child invalidates a fact
about its parent's contents even if the parent's own bytes did not change. Shared frozen
edges do not make frozen data mutable. Facts about a separately retained frozen child
remain valid when a mutable parent changes its handle to that child, but facts connecting
the parent's field to that child require the parent's current version.

For a call, let `W_in` be the instantiated may-write footprint in the pre-call forest.
Let `changed(X)` conservatively include regions whose contents or ownership relationships
may change under the transition summary, including affected old and new parents. The
invalidation footprint is:

```text
I = W_in union changed(X) union owning_ancestors(W_in union changed(X))
```

Use the input forest and all possible outcome forests for ancestry; a detached child must
not hide a change to its old parent. In each outcome, each surviving mutable region in `I`
receives a fresh version; destroyed regions become unavailable, and fresh allocations have
fresh identities and versions. A possible write invalidates even when one execution writes
nothing or restores
the same bytes. Newly attached storage becomes part of the returned ownership description
and enclosing versions. Freezing may preserve content facts only when its checked transition
justifies relating the input contents to the frozen result. A handle transfer alone does
not change storage contents, but still changes which bindings have access authority.

Every storage-dependent fact records the region versions on which it relies. This includes
facts derived through function results, abstract observers, and other proofs: derivation
cannot erase a dependency on mutable contents. A fact may be used about current storage
only when all those versions match and any loan-scoped evidence remains valid. Immutable
snapshot propositions may survive a version change, but cannot be relabeled as facts about
the new contents. Storing or duplicating `proof(P)` does not bypass this check.

Call checking proceeds in the following order:

1. Evaluate and convert arguments in the elaborated order, applying their effects and
   preserving temporary carriers. Check preconditions against the resulting input state;
   a fact invalidated by a later argument cannot discharge a call precondition.
   If evaluation or conversion propagates a recoverable failure before invocation, follow
   caller-side failure control flow and clean already-created temporaries as required by its
   schedule. Preserve effects already performed; do not apply the uninvoked callee's contract.
2. Instantiate the effect and result contract, check authority and conflicts, then apply
   its abstract state transition for each possible outcome. Refresh affected versions using
   the complete effect summary and withdraw facts about their current contents. Preserve a
   current-storage fact only when all its dependencies are proved unchanged; uncertain
   aliasing prevents preservation.
3. Install result carriers and outcome-specific output states and justified postconditions
   using the resulting identities and versions. Until the outcome is discriminated, expose
   only their common guarantees while retaining all possible dependencies and obligations.
   Preconditions and old facts do not become postconditions automatically.
   Retire argument carriers only when permitted; any subsequent caller-side cleanup applies
   its own effects and can invalidate newly obtained facts.

Each outcome's postconditions are proved for every return of that variant, after required
callee-side cleanup, by checking the body in the bounded proposition language; they are not
accepted as user axioms. They may relate input snapshots to output contents, for example
`length(r, out) = length(r, in) + 1`, and thereby justify fresh bounds facts. Native contracts
belong to the explicit trusted boundary. Without a checked postcondition, callers must
re-establish affected facts by observation or runtime tests. At joins, incompatible storage
versions require a fresh joined version and only facts justified on every incoming path;
loop invariants must likewise be re-established for the versions on every back edge.

## User-defined capabilities

Users may define sealed abstract capabilities with finite operation signatures. A capability
is evidence of authority, not a freely constructible label attached to a value.

```text
capability append(r)                 // affine unless explicitly justified otherwise
append_item : append(r) * item -> append(r)

capability connection(r, state)
send  : connection(r, open) * message -> connection(r, open)
close : connection(r, open) -> connection(r, closed)
```

The declaring module defines private constructors and checked operation bodies. Creating an
`append(r)` token must consume appropriate exclusive authority; the token privately owns
that authority. Each operation preserves or consumes it according to its signature. A module
cannot mint a second token by naming the same region. An append token coexists with frozen
data only when it controls separate storage, not the frozen region itself.

Users can restrict and package existing authority. They cannot introduce unchecked primitive
permissions, declare a token duplicable while it contains exclusive authority, or prove their
way into write access to frozen storage. Duplicable capabilities must have duplicable private
representations. Native operations require an explicitly trusted boundary.

Protocol tokens are affine by default: abandoning one follows its declared cleanup behavior.
A guarantee that a user explicitly performs `close` would need an additional must-consume
rule; affinity alone cannot establish it.

## A terminating proof kernel

The guarantee is that every submitted proof has a terminating verification procedure and
every accepted proof follows the kernel's rules. It is not a guarantee to prove every true
statement or to discover a proof automatically.

The initial proposition language contains equality of immutable terms, linear integer
inequalities, conjunction, implication, and nominal type/capability witnesses. Integer terms
contain constants, variables, addition, subtraction, and multiplication by a literal.
Type equality uses finite structural comparison and nominal declaration identity; recursive
type unfolding is not an unrestricted proof computation.

Proofs are finite, acyclic certificate trees. Constructors include assumptions, reflexivity,
symmetry, transitivity, congruence, conjunction introduction/elimination, implication
introduction/application, and checked linear-arithmetic certificates. Implication introduction
discharges a scoped assumption; an assumption cannot become an unconditional exported fact.
Linear certificates combine inequalities with nonnegative coefficients and check normalization
using arbitrary-precision integers. They may be incomplete for integer arithmetic.
Explicitly encoded domain bounds justify reasoning about machine integers; arithmetic
operations require no-overflow evidence or use their specified modular semantics.

There is no general recursion, arbitrary Cinder evaluation, user axiom declaration, or
unbounded proof search inside verification. Optional bounded automation produces certificates
checked by the same kernel. Finite verification can still be expensive; implementation resource
limits report failure to check, never successful proof.

Examples of admissible statements:

```text
n >= 0  |- n + 1 > 0
i >= 0, i < n |- i is a valid index into an immutable array of length n
proof(A = B)  |- equality transport between A and B
```

Equality transport preserves region identity and capabilities; it cannot change `read` to
`own`. A nominal capability witness proves a fact about authority but does not duplicate the
authority itself. Resource-bearing tokens remain in `Delta`, not in erased proof data.

Proofs refer to stable identities, not variable spellings. A write creates a new storage
version, so facts about the old contents cannot establish facts about the new contents.
Primitive operations, calls, and cleanup all apply the footprint and version rules above.
Frozen values support persistent facts. Facts established under an inspection loan can
justify operations within that loan's validity; retaining a proof of the old snapshot never
authorizes an operation on changed storage. Runtime tests can introduce conditional evidence
in a successful branch, which is erased afterward; the runtime test itself is not erased.

## Examples and expected outcomes

An existing mutating `list.pop` API can translate to structural sequence removal without
requiring copyable elements. Here `push` transfers its owned element argument, `pop` returns
an owning sum value, and the consuming `match` elaborates to `match_move`:

```text
pop[T](xs: edit[sequence[T]]) -> option[T]:
    return remove_last(xs)       // reborrow xs; transfer the result

resource = new_resource()        // own(r, Resource), no copy witness
xs = new_sequence[own[Resource]]()
push(&mut xs, resource)          // owning parameter: move; resource is unavailable

match pop(&mut xs):
    @some(item) => use_resource(item)  // move the extracted owner
    @none       => ()

drop(xs)                        // clean only elements still present
```

This is a translation into the proposed core, not a change to the implemented Cinder
library. The list's checked representation contract must justify its sequence operations;
`use_resource` consumes the removed owner and accounts for its cleanup.

```text
x = new_buffer()                 // own(r, buffer)
push(&mut x, 1)                  // accepted; temporary exclusive loan
n = length(&x)                   // accepted; observation ends after call
push(&mut x, 2)                  // accepted
publish(&!copy_owned(x))         // explicitly copy, then freeze; preserve x
push(&mut x, 3)                  // accepted: x still owns mutable r
y = copy_owned(x)               // new region s, if buffer has a structural witness
push(&mut y, 3)                  // accepted; r is unchanged
consume(y)                      // own parameter: move; y is unavailable afterward
publish(&!x)                    // freeze original; x is unavailable afterward
```

Here `copy_owned` observes its source and requires a structural witness; `consume` requests
ownership and needs no copy witness. Passing `x` directly to `publish` would be rejected
because an owning argument is not implicitly frozen or copied.

```text
v = &x                          // scoped view into mutable x
push(&mut x, 1)                  // rejected if v is used afterward
consume_observation(v)           // last carrier use
push(&mut x, 1)                  // accepted after the loan ends
```

Required negative cases also include freezing with an active edit loan, storing an edit
token in a duplicable closure, returning a local loan, using an old bounds proof after a
length-changing write, and declaring a capability constructor that duplicates an owner.

Projection validation must also cover a frozen parent containing an owned child: observing
the child is accepted, retaining a shared child and then dropping the parent is accepted,
and extracting ownership or editing the child through that parent is rejected. A persistent
projection into an inline frozen field must likewise survive release of the outer handle.
An inspection of an owned child must not become a persistent shared handle, and a projected
loan must prevent destruction of its owning ancestors. Freezing a closure with a mutating
call contract is rejected; a closure whose read-only body remains valid under shared capture
projections may freeze when all its captures satisfy the freezing conditions.

Forest-conflict validation uses `parent -> child -> grandchild` and a separately allocated
owned sibling of `child`. Each rejection below assumes the loan has a later live use:

| Situation | Expected result |
|---|---|
| Inspect `parent`, then edit `grandchild` | Rejected: ancestor inspection covers descendants |
| Inspect `grandchild`, then move, freeze, or drop `parent` | Rejected: ancestor transition overlaps the loan |
| Inspect `grandchild`, then replace `parent`'s child field | Rejected before removing the ownership edge |
| Edit `child`, then read through `parent` | Rejected: competing ancestor read overlaps the edit |
| Reborrow an inspection from an edit of `child`, then write through that edit | Rejected until the inspection ends; accepted afterward |
| End a parent loan while its child reborrow survives | Rejected even if the parent handle has no further direct use |
| Inspect both `parent` and `grandchild` | Accepted: overlapping observations are compatible |
| Directly project edits of `child` and its separately allocated sibling from their owner | Accepted when neither grant is suspended by another loan |
| Edit two distinct named inline fields of `parent` independently | Accepted with checked field paths and valid granting authority |
| Iterate over `self.items` while updating `self.total` directly | Accepted when the iterator borrows only `items` and the fields and effects are disjoint |
| Keep a field loan, then access the same field or a prefix through competing edit authority | Rejected: equal and prefix paths overlap |
| Keep a field loan, then call a method requiring an edit of all of `self` | Rejected: the receiver requirement overlaps the field |
| Use distinct fields carrying aliases of the same mutable target as evidence of separation | Rejected: resolved target provenance governs conflicts |
| Edit an unrelated ownership tree | Accepted when disjointness is established |
| Release a shared handle that is the lifetime origin of a live inspection | Rejected despite immutable storage |

Carrier validation must cover the following cases. `observe_pair` takes two inspection
parameters, `return_view` returns an inspection derived from its inspection parameter,
and `mutate_then_inspect` mutates an edit parameter and returns a derived inspection:

| Situation | Expected result |
|---|---|
| Duplicate an inspection, retire the first copy, then mutate while the second has a later use | Rejected: the second carrier keeps the loan active |
| Retire every inspection copy, end the loan, then mutate its former target | Accepted when no other loan conflicts |
| Use an inspection binding after its carrier was retired and its loan ended | Rejected even if its former type remains known |
| Capture an inspection in a closure, retire the original binding, then mutate before calling the closure | Rejected: the capture is a live carrier |
| Capture an owner by value and return a view with `from self` | Move the source owner and preserve the returned view's supporting ownership dependencies; reject closure cleanup or replacement that invalidates the live view |
| Capture an observation by value and return its duplicate | Preserve the original target and dependencies; a sufficiently precise checked summary need not retain the closure |
| Capture an observation of an outer owner and return a dependent view | Preserve external source dependencies through both the capture and result |
| Return a view of captured storage destroyed by a consuming invocation | Rejected even with `from self`; an origin annotation cannot authorize dead storage |
| Move a container holding an inspection, then mutate before using the destination | Rejected: moving preserves the stored loan dependency |
| Evaluate `observe_pair(&x, mutate_then_inspect(&mut x))` from left to right | Rejected: the first argument keeps its inspection active when the second requests editing |
| Bind the result of `return_view(&x)`, then mutate before using that result | Rejected: returning transfers the dependency before argument retirement |
| Invoke an input-derived callback on different source slices | Instantiate each call's origin bound against its own argument; preserve each returned view's dependencies |
| Supply a callback returning captured storage to an input-only result-origin contract | Rejected: the captured dependency is outside the permitted bound |
| Supply a callback using a captured scalar to select a subrange of its input | Accepted when its complete checked contract is compatible; the scalar need not supply a result loan |
| Rename callable contract binders consistently or use different implementation parameter names | Preserve the same contract meaning |
| Return a loan of an unretained temporary owner | Rejected: the return carrier outlives its origin |
| End an inspection after loading a scalar snapshot, then use the snapshot | Accepted; the scalar is not a loan carrier |
| Retire a binding on one branch, then use it after the join | Rejected: availability is not restored by merging contexts |

Argument-conversion validation must include existing scoped handles:

| Situation | Expected result |
|---|---|
| Pass an owner bare to an owning parameter, including an uncopyable resource or unconstrained generic value | Accepted by transfer without a structural copy witness; source becomes unavailable |
| Pass an owner whose contents have a structural copy witness, then use its source binding | Rejected: witness availability does not change the transfer into an implicit copy |
| Pass a scalar or retained frozen handle by value, then reuse the source | Accepted under its duplication rule; the new frozen handle retains storage independently |
| Pass an owner bare or with `&>` to a `shared_read` parameter | Rejected: neither form requests freezing or independent copy construction |
| Pass an owner with `&!` to a `shared_read` parameter, then reuse the owner binding | Rejected: freezing consumes the owner |
| Explicitly materialize an owner with `copy_owned` and freeze that copy for a sharing parameter, then mutate the original | Accepted when copying, freezing, and loans are valid; the original remains owned |
| Pass an existing inspection unannotated to an inspection parameter | Accepted by duplicating the same loan and dependencies; the source remains available |
| Pass an existing inspection with `&` to an inspection parameter | Accepted by creating a fresh dependent observation loan |
| Pass an existing edit to an inspection helper, then reuse the edit after all derived carriers retire | Accepted after the observation reborrow ends and no other loan suspends the requested access |
| Pass the same edit to two sequential edit helpers that retain no loans | Accepted: each call creates and ends its own reborrow |
| Pass an owner or edit with `&mut` to an edit helper | Accepted when loan checks permit the explicit edit loan or reborrow |
| Pass an owner or edit with `&` to an edit helper | Rejected: expected parameter type cannot strengthen explicit observation to editing |
| Pass an owner or edit with `&mut` to an inspection helper | Rejected: the explicit edit request does not match the requested observation |
| Return or store a reborrow from a helper, then attempt conflicting reuse of the original edit | Rejected while the retained reborrow is live, including on an error outcome |
| Pass an edit with `&>`, then use its source binding after the call | Rejected: transfer invalidated the source binding |
| Transfer a suspended edit to a helper that writes through it while a conflicting child reborrow remains live | Rejected: transfer preserves the suspension and dependencies |
| Pass an inspection to an edit parameter, with or without an annotation | Rejected: observation authority cannot grant editing |

Call-footprint validation must include these cases. Bounds facts used after a loan ends
are versioned snapshot facts, not permission to keep using a retired inspection:

| Situation | Expected result |
|---|---|
| Prove an index in bounds, call a function that may shrink the same buffer, then index with the old proof | Rejected without a new current-version proof |
| Mutate a separately allocated child through a call, then reuse a fact about its ancestor's contents | Rejected: ancestor versions are invalidated |
| Call a mutator on a provably disjoint tree, then reuse a fact about an unchanged tree | Accepted if its other dependencies remain valid |
| Call an edit-taking function with checked `writes nothing`, then reuse a bounds fact | Accepted if transitions and cleanup also preserve its dependencies |
| Call a function with an imprecise edit effect and no explicit write annotation | Conservatively invalidate its entire permitted mutable footprint |
| Call a closure or token operation that mutates storage hidden in its representation | Its exposed effect summary must invalidate that storage's facts |
| Claim `writes nothing` while a callback or cleanup writes pre-existing storage | Contract rejected |
| Mutate a copied argument with no separately carried mutable capability | Facts about the original storage remain valid |
| Mutate while evaluating a later argument, then use an earlier proof to satisfy the callee's precondition | Rejected if the mutation invalidated that proof's storage dependencies |
| Use a checked postcondition to derive a bounds fact about the output version | Accepted when the kernel verifies the derivation |
| Preserve a fact across a call where region disjointness is unknown | Rejected unless another checked guarantee establishes preservation |
| Replace an owned child, then return an error | Error contract must describe the new ownership graph; old bounds facts cannot be reused without a current-version proof |
| Store a loan into caller-owned storage, then return an error | Error contract retains the stored loan's dependencies and prevents premature destruction of its origin |
| Mutate storage, then return an error under a contract claiming rollback | Contract rejected unless restoration is proved; the error tag alone establishes no rollback |
| Use a success-only capability before discriminating a fallible call's result | Rejected unless that capability is available in every possible outcome |
| Perform cleanup after a call, then match its result to recover an invalidated postcondition | Rejected: outcome selection cannot undo intervening effects |
| Propagate failure while converting a later argument before invocation | Apply caller-side effects and cleanup for evaluated temporaries; do not apply the uninvoked callee's contract |

Copy validation must distinguish equivalent contents from independent storage:

| Situation | Expected result |
|---|---|
| Require `copy` for an owner type with structurally copyable contents | Accepted when the witness is derived; ordinary value passing still transfers the owner |
| Use inspection duplication or a legacy `copy` classification as evidence for structural `copy` | Rejected: duplication does not establish the independent-copy witness |
| Copy a record and its owned mutable children into fresh regions, preserving all contents | Accepted when component witnesses and observation authority are valid |
| Return a fresh buffer with different elements from the source | Rejected: freshness does not establish value equivalence |
| Allocate a fresh record but reuse its source's mutable child | Rejected: the result's mutable reach is not fresh |
| Substitute unrelated pre-existing mutable storage for a child | Rejected even when it is disjoint from the source |
| Copy a value with a retained frozen child, then release the source | Accepted: the copy independently retains the frozen child |
| Call `copy` on a frozen handle | Accepted with the handle's structural witness; retain the same frozen target and preserve the source |
| Call `copy_owned` through a frozen handle or inspection | Accepted when represented `T` has a structural witness and observation authority is valid; construct `own[T]` with fresh mutable reach |
| Call `copy` on an inspection or edit handle itself | Rejected in the initial core: scoped capability duplication/reborrowing is distinct from independent value copying |
| Copy an owned record, mutate the copy, then observe the source | Accepted; the source retains its original contents and versions |
| Duplicate an inspection explicitly | Accepted with the same loan dependencies; this is not an independent value copy |
| Request an independent value copy of a record or closure containing a scoped loan | Rejected in the initial core; capability duplication is a separate operation |
| Copy a protocol token's backing bytes into a fresh allocation | Rejected: external authority has not been duplicated legitimately |
| Use a user-defined clone that mutates the source as an implicit copy | Rejected even if it returns equivalent contents in fresh storage |
| Materialize a frozen record with an owned child using `copy_owned` | Accepted when both root and reconstructed owned child are fresh and contents are equivalent |
| Retarget a captured bounds proof from the source region to the copy without a derivation | Rejected: region and version identities are not interchangeable |

Range-view validation must exercise shape, authority, and surviving dependencies:

| Situation | Expected result |
|---|---|
| Slice with a bare range or explicit observation | Produce a scoped observation range with checked bounds and origin dependencies; expected editing cannot strengthen it |
| Slice with an explicit edit request | Produce a mutable range only with valid edit authority and no conflicting loans |
| Pass an existing mutable range to observation and edit range helpers | Reborrow in the declared mode; preserve any dependencies retained in results or fields |
| Bind a mutable range by value or explicitly transfer it | Move its affine authority and preserve dependencies; reject subsequent use of the source binding |
| Request simultaneous edit subranges based only on nonoverlapping bounds | Rejected: bounds alone supply no checked partition |
| Split one mutable range with `split_at`, then edit both children | Accepted after bounds validation, with disjoint child footprints and shared backing dependencies |
| Split at zero or length, including an empty range | Accepted; empty children authorize no element access |
| Split outside the visible source bounds | Reject or fail the runtime check before creating either child loan |
| Split one child again while editing the other child | Accepted when inherited footprints are disjoint |
| Return or store a split child, then use conflicting parent authority before its last use | Rejected even after the original child handle retires |
| End one split child, then edit the entire parent while the other survives | Rejected; only unblocked authority can resume |
| Forge a partition from bounds, widen a child, or duplicate its edit | Rejected: none supplies the required authority |
| Borrow an element slot from a mutable range of owners | Yield edit authority over the stored owner slot; checked replacement requires no copy witness |
| Form an out-of-bounds range, invalid UTF-8 boundary, or range over uninitialized elements | Require discharge or runtime enforcement of the corresponding validity obligations |
| Treat a scoped range over frozen backing as independently retained | Rejected: scoped source dependencies remain in force |
| Return a frozen source's library `.slice()` result, then release the source handle | Accepted: the result independently retains its checked frozen backing without a loan on the source handle |
| Create a retained subrange and release its source range | Accepted within the source's visible bounds; preserve backing identity and independent retention |
| Duplicate a retained range over elements without an independent-copy witness | Retain the same frozen storage; do not copy elements or expose owner tokens |
| Call a retained-projection intrinsic with only a scoped view or a raw pointer/count | Rejected: persistent frozen authority and checked backing provenance are required |
| Retain a tiny inline range, release the parent, then release the final range | Backing storage remains live until the final retention and lifetime obligations end; release frozen outgoing edges once |
| Retain an invalid subrange or split a UTF-8 scalar | Reject or enforce the required validity checks before exposing the retained result |

Cleanup validation must cover both resource accounting and the authority used by disposal:

| Situation | Expected result |
|---|---|
| Move an owner on one branch and leave it unused on the other | Clean the remaining owner on that predecessor edge; never forget it at the join |
| Require an owner after a join where one branch consumed it | Rejected unless every path initializes a compatible result |
| Normalize a branch by dropping an owner still borrowed by the join result | Rejected: edge cleanup cannot invalidate the surviving loan |
| Overwrite a resource-bearing field with an independent replacement | Clean the old value once, then install the replacement and its obligation |
| Replace an owner with a value borrowing its old contents | Rejected: the replacement cannot survive disposal of its origin |
| Move one structural field, then drop the parent | Clean only remaining initialized fields; the moved field belongs to its destination |
| Reinitialize a moved field, then use the whole parent | Accepted when every field is initialized and loan checks pass |
| Freeze, copy, or borrow a whole parent with an empty field | Rejected |
| Partially move a value with custom whole-value cleanup | Rejected without a decomposition rule |
| Fail construction after only some fields initialize | Clean those fields once; do not invoke the complete-value destructor |
| Run custom cleanup and then unconditionally clean all fields again | Rejected: obligations already discharged cannot be discharged twice |
| Move observable cleanup to last direct use without checking its effects | Rejected |
| Release a frozen parent while a child or inline projection is independently retained | Retained storage remains live; inline projection delays reclamation of its backing allocation |
| Freeze a value whose required destructor needs mutable access | Rejected; last shared release cannot recreate edit authority |
| Execute cleanup that mutates another region, then reuse an old fact about that region | Rejected unless a current-version fact is re-established |

Exchange, removal, and consuming-match validation must include noncopyable elements:

| Situation | Expected result |
|---|---|
| Pop a noncopyable resource through an edit | Accepted: transfer it into the result without copying it or running its destructor |
| Drop a sequence after popping an element | Clean only remaining elements; the removed element's obligation belongs to its result carrier |
| Exchange an owned child with an independent replacement | Return the old owner and install the new owner; both obligations remain accounted for |
| Return a collection slot as `edit[own[T]]` and exchange through it in an unconstrained generic helper | Accepted with valid slot provenance and replacement authority; preserve initialization, return the old owner, and require no copy witness |
| Store or return a slot loan, then relocate, remove, or destroy its containing collection before its later use | Rejected: the surviving carrier retains the slot's origin and granting dependencies |
| Read an owner token by value or leave an empty element through a borrowed slot | Rejected: observation and edit do not grant ownership extraction |
| Select two different inline indices and claim simultaneous disjoint edits without inherited split separation | Rejected: unequal indices alone do not separate footprints |
| Use only an edit of a child allocation to exchange its owner's slot in the parent | Rejected: that slot requires authority over the containing allocation |
| Exchange with a replacement that borrows the outgoing owner | Rejected when the loan prevents transferring that owner |
| Pop while the last element has a live observation or edit loan | Rejected: removal cannot invalidate or bypass the loan |
| Pop an empty sequence | Return `@none` and preserve its contents |
| Use a pre-pop length proof after a possibly nonempty pop | Require a current-version proof justified by the outcome contract or a new observation |
| Consume `@some(resource)` with an owning match | Transfer the payload once; disposing the sum must not also destroy that resource |
| Attempt consuming payload extraction through an inspection or from a sum with custom cleanup and no decomposition rule | Rejected |
| Fail replacement evaluation before exchange commits | Follow caller-side failure cleanup; exchange has not transferred the destination's contents |

## Relationship to Cinder and next validation

This proposal retains move-by-default passing for owned values and makes borrowing depend
on declared parameter capabilities. Independent copying and conversion of owned mutable data
to persistent frozen sharing are explicit. A structural copy witness never enables implicit
deep copying or duplication of an owner. These choices, including the separation of retained
handle duplication from independent copying, require review against Cinder's value
semantics and existing APIs; this document does not silently amend those specifications.

The current borrowing analysis and bounded refinement reasoning are useful implementation
starting points. A future elaborator would make freeze, capability transfer, loans, cleanup,
and proof erasure explicit. Storage effects participate in the call contracts above;
staging and broader effects remain separate judgments.

Before implementation, formalize a small sequential operational semantics and establish
preservation, progress modulo runtime failures, absence of conflicting access, permanent
freeze, nonescape of dead storage, and sound proof erasure. Then translate representative
Cinder examples to measure which existing special cases disappear. Memory reclamation for
shared data, surface annotations, thread transfer, and cancellation remain design work.
