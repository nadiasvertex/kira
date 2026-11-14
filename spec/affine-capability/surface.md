# Affine capability surface mapping

Status: accepted experimental-subset spellings for declarations, receivers, loans, transfer,
freeze, copying, the `copy` constraint, single-element and scoped/retained range views,
the library `.slice()` API, local bindings, callable parameter names/result-origin bounds,
write bounds including `writes nothing` and the callable environment root `self`, and
observing/editing/consuming callable receivers with inferred lambda call modes, capture
lists with value-capture defaults, and capture-derived result origins using `from self`.
Contract inference and optional programmer constraints are accepted policies; the worked contract
clauses beyond the accepted result-origin and write-bound forms remain syntax candidates.
This document does not amend the implemented grammar or establish T08 completion.
See [Q2 decisions](decisions.md#q2--ownership-default-and-explicit-copying)
and the [worked translation](translations.md).

## Types and parameter modes

| Meaning | Explicit type | Existing shorthand |
|---|---|---|
| Ordinary by-value parameter | `T` | — |
| Exclusive owner handle | `own[T]` | — |
| Scoped observation | `inspect[T]` | `&T` |
| Scoped editing | `edit[T]` | `&mut T` |
| Retained, permanently frozen sharing | `shared_read[T]` | — |
| Retained, permanently frozen contiguous range | `shared_slice[T]` | — |
| Retained, permanently frozen UTF-8 text range | `shared_str` | — |

An ordinary `T` and `own[T]` remain distinct types; no owner wrapper is silently added or
removed. Ordinary by-value uses transfer affine values and duplicate values that have checked
duplication rules. An ownership transfer does not need an independent-copy witness.

Inspection and edit parameters request borrowing, inspection duplication, or reborrowing as
specified by the core conversion table. The shorthand reference types have the same authority,
loan identity, and lifetime obligations as their explicit capability forms. They do not
introduce a second borrowing model or independent-copy witness for scoped handles.

## Collection views and borrowed slots

Collapse `cell[T]` into `inspect[T]` and `mut cell[T]` into `edit[T]` in the experimental
design. Single-element views use the same capability types, loan rules, and inferred result
origins as other borrowed values. They are not a separate family of builtin types.

| Existing view | Capability mapping |
|---|---|
| `cell[T]` | `inspect[T]`, also spelled `&T` |
| `mut cell[T]` / internal `cell_mut[T]` | `edit[T]`, also spelled `&mut T` |
| `slice[T]` | Scoped observation of a contiguous range, carrying its length |
| `mut slice[T]` | Scoped editing of a contiguous range, carrying its length |
| `str` | Scoped observation of UTF-8 text |

Keep the existing slice and text spellings. Range views remain distinct from single-value
loans because they describe a range and its shape. Their stored and returned loans carry
the same origin and granting-chain dependencies; no lifetime annotations are introduced.

A single-element capability may target an initialized storage slot. For an element of type
`own[resource]`, `edit[own[resource]]` lends the owner slot inside the collection, whereas
`edit[resource]` lends the resource's contents. A slot capability records the checked
containing allocation and element path, with authority derived from the collection. A child
edit cannot be reinterpreted as an edit of its owner's slot.

`*slot` designates the borrowed place. `exchange(*slot, incoming)` replaces its contents and
returns the previous value, transferring cleanup obligations without requiring `copy`.
`*slot = incoming` performs checked replacement and cleans the previous value. A plain read
does not extract an owner or leave a borrowed slot empty; it follows the capability-dependent
load and observation rules. A stored edit handle may exercise this authority even when the
handle binding uses `let`.

Element-borrowing collection APIs return `inspect[T]` or `edit[T]`; for example, an
`at_mut` operation returning a slot view returns `edit[T]`. Its checked contract must
establish the slot origin, permitted replacement, and lifetime dependencies. The live loan
prevents conflicting element access and collection changes that could invalidate it,
including relocation, removal, or destruction. Different indices alone do not establish
disjoint edit authority; element views preserve any inherited checked split partition.

Single-element `&items[i]` and `&mut items[i]` use the checked element-borrowing operations
and yield `inspect[T]` and `edit[T]` for stored element type `T`. For an owner element,
the slot type therefore includes `own`; borrowing its child contents is a separate projection.
Borrow dispatch returns the capability itself, not a reference to a temporary view handle.

Different collection implementations can expose the same capability while keeping their
element type known. This hides collection layout, not element types or loan dependencies.
Heterogeneous element types require a sum or a separately checked trait-object abstraction;
`cell` is not an element-type-erasure mechanism.

Legacy mode retains its current cell types and methods until migration. Compatibility
spellings, if retained in the new mode, must have exactly the corresponding capability
semantics. Migrate `get` to a permitted load or observation, with independent copying
explicit; migrate `set` to checked replacement. See the
[worked slot example](translations.md#worked-example-a-borrowed-collection-slot).

## Range construction and passing

Use observation by default for range indexing, with explicit editing when requested:

| Expression | Result for stored element type `T` |
|---|---|
| `items[start .. end]` | `slice[T]` |
| `&items[start .. end]` | `slice[T]`, explicitly requesting observation |
| `&mut items[start .. end]` | `mut slice[T]`, explicitly requesting editing |

Borrow dispatch yields the range capability itself, not a reference to a temporary slice
descriptor. An expected mutable result type cannot reinterpret an observation range as an
edit. All forms require checked source authority, valid bounds, initialized storage, and
loan provenance. Text observation slicing yields `str` and additionally checks UTF-8
boundaries; this decision introduces no mutable text-view spelling. Slicing copies no
elements and takes no ownership of the backing storage.

Range parameters follow the corresponding single-element capability modes. A bare existing
mutable view passed to a `slice[T]` parameter makes an observation reborrow; passing it to a
`mut slice[T]` parameter makes an edit reborrow. A read-only view cannot satisfy a mutable
parameter. Explicit requests retain their stated authority and mismatches are diagnosed.
Observation views may duplicate under the loan rules, preserving their dependencies.
Ordinary value binding of a mutable view moves it; `&>view` explicitly transfers its existing
edit authority and makes the source binding unavailable. Transfer preserves the granting
chain and dependencies and does not transfer ownership of the backing collection.

Returned or stored subranges retain their source and granting-chain dependencies. Conflicting
authority is suspended for the dependent loan's lifetime, including after a helper returns.
Subrange bounds alone do not prove disjoint editing; use the checked `split_at` operation.
For `mut slice[own[T]]`, borrowing an element slot yields `edit[own[T]]`; replacing its owner
uses the checked slot rules rather than extracting ownership through a load.

Slicing frozen storage can produce a scoped observation view, subject to the source lifetime
obligation. It does not implicitly produce an independently retained range. See the
[range translation](translations.md#worked-example-range-views-and-reborrowing).

## Named-field borrowing and mutable range splitting

Borrow distinct named fields directly with the existing `&` and `&mut` spellings. No new
field-borrow syntax is needed. Checked structural field paths can separate siblings without
inventing allocations. A loop borrowing only `self.items` may update `self.total` directly
when its effects respect that separation. A call requiring an edit of the whole receiver
still conflicts with the live field loan. Private fields and aliases retain their ordinary
abstraction and provenance rules; arbitrary indices do not become disjoint field paths.

Use built-in `split_at(view, index)` with a `mut slice[T]` to obtain a pair of `mut slice[T]`
results. The bare source reborrows in edit mode; it is not consumed or copied. Both children
retain the source's backing lifetime and granting dependencies. Bounds are relative to the
visible source range and checked before results are exposed. Runtime failure follows the
selected bounds-failure policy; this decision adds neither an error type nor unwinding.

The children permit simultaneous edits of their disjoint portions. Splitting one child again
preserves separation from the other. Conflicting use of the parent is suspended while a
child or dependent view survives, including across returns or storage in another value.
Transferring a child preserves those obligations. Splits at zero or length are valid;
empty results cannot lend an element. Ordinary slicing cannot manufacture split authority,
and no mutable text view is introduced. See the
[worked translation](translations.md#worked-example-named-field-borrowing-and-range-splitting).

## Retained frozen ranges

Use `shared_slice[T]` for an independently retained contiguous range of frozen elements and
`shared_str` for an independently retained UTF-8 text range. These types carry bounds and
backing-storage provenance plus their own retention obligation. They are distinct from
scoped `slice[T]` and `str`, and are not spelled `shared_read[slice[T]]` or `shared_read[str]`:
wrapping a scoped descriptor must not erase its loan dependencies.

Expose creation as the ordinary library method `.slice(range)`, using the existing
`range[usize]` value and method-call syntax:

| Expression on a suitable frozen source | Result |
|---|---|
| `frozen[start .. end]` | Scoped `slice[T]`, or `str` for text |
| `frozen.slice(start .. end)` | Retained `shared_slice[T]`, or `shared_str` for text |

The library provides `.slice()` on suitable frozen collection/text capabilities and on
retained ranges themselves. It preserves the source, validates bounds relative to that
source's range and initialized storage, and checks UTF-8 boundaries for text. It copies no
elements and does not freeze, thaw, or materialize owned storage. A retained subrange has
independent retention and can survive release of the source handle; a scoped subrange keeps
its source lifetime obligation. All element access remains subject to frozen projection
rules, including when stored element representations contain owner handles.

Retained range handles duplicate under the checked frozen-handle rules, independently
retaining the same storage without an element-copy witness. Final release follows the frozen
release recipe; a small range may keep the whole backing allocation alive. This specifies
semantic provenance and retention, not a concrete ABI or control-block layout.

The public method is library code over an explicitly contracted compiler/runtime intrinsic
for retained frozen projection. It introduces no new expression syntax or universal
`retain_range` builtin. The intrinsic requires an existing persistent frozen capability,
valid projection provenance, and established range validity; it retains the correct backing
storage before exposing the result. A raw pointer and a reference-count increment are not
authority, and scoped observation/edit views cannot be promoted by this operation. The
intrinsic's exact name and ABI remain implementation details. Checked library code handles
the public bounds/text validation and API, while the intrinsic contract enforces its required
preconditions and retention guarantees. The failure policy remains subject to Q5.

See the [retained-range translation](translations.md#worked-example-retained-frozen-ranges).

## Receivers

| Receiver declaration | Capability |
|---|---|
| `self` | `self: inspect[Self]` |
| `mut self` | `self: edit[Self]` |
| `self: own[Self]` | Consume the receiver's owner |

`Self` denotes the implementing or extended type. Ordinary method calls select the declared
receiver mode. They may borrow or reborrow without an explicit call-site annotation; a
consuming receiver moves its owner. Trait and extension implementations must satisfy the
same capability and contract requirements as ordinary functions.

```cinder
def length(self) -> usize: ...
def push(mut self, value: byte) -> unit: ...
def finish(self: own[Self]) -> output: ...
```

These declarations illustrate receiver syntax; their bodies and enclosing types are omitted.

## Explicit conversions

| Expression | Request |
|---|---|
| `x` | Use the parameter's declared passing mode |
| `&x` | Begin an observation loan or dependent observation reborrow |
| `&mut x` | Begin an edit loan or dependent edit reborrow |
| `&>x` | Transfer existing affine authority |
| `&!x` | Consume an owner and permanently freeze its storage and owned descendants |

`&x` always requests observation. An edit parameter cannot reinterpret it as `&mut x`.
`&mut x` always requests editing; a mismatched parameter is diagnosed instead of silently
weakening the explicit request. Bare arguments remain concise: their parameter modes can
select observation or edit borrowing without these annotations. No explicit loan form can
satisfy an owning parameter by copying or transferring the referent.

```cinder
length(b)             # parameter requests observation
length(&b)            # explicitly request observation
push(b, 1)            # parameter requests editing
push(&mut b, 1)       # explicitly request editing
push(&b, 1)           # rejected: observation cannot satisfy editing

consume(b)            # owning parameter: transfer
consume(&>b)          # explicitly state the same transfer
helper(e)             # edit parameter: dependent edit reborrow of e
helper(&mut e)        # explicitly request the edit reborrow
helper(&>e)           # transfer e itself; invalidate its source binding
```

These are independent call examples. A transferred binding is unavailable to later uses.
Transferring an edit preserves its identity, granting chain, suspension, and dependencies;
it never transfers ownership of the target. Freezing is subject to deep-freeze, loan, and
cleanup checks. Neither inference nor an expected result type inserts independent copying
or freezing. Prefix operators bind to their operand before the containing call's conversion;
the parser must preserve the distinct observation, edit, transfer, and freeze requests.

## Local construction and bindings

Use existing `let` and `var`, with optional type annotations. Ownership comes from the
initializer's type; no new `builder` declaration or implicit freeze is introduced.

```cinder
var b = buffer.new()                 # checked constructor returns own[buffer]
push(&mut b, 1)
let frozen = &!b                     # consumes b; produces shared_read[buffer]
```

An explicit declaration can write `var b: own[buffer] = buffer.new()` instead. `var` permits
local mutation only when capability authority also permits it. `let` keeps its existing
restrictions on reassignment and mutation through the binding; a stored edit handle can
still exercise its existing authority over its referent. Binding an owner with `let` does
not freeze it. Binding a frozen handle with `var` permits replacing the binding but cannot
make its target writable. Allocation and resource-constructor APIs are not chosen here.

## Explicit copying

Use the language-provided `copy(x)` and `copy_owned(x)` operations. Both observe and preserve
their source; neither applies the usual move-by-default conversion to consume that source.
They require live read authority and the relevant checked structural witness. Copying
mutable contents may allocate and fail under the modeled failure policy; a partial result
must be cleaned up without changing the source. Frozen backing may be shared when retained
independently and permitted by the recipe.

| Operation | Result | Witness requirement |
|---|---|---|
| `copy(x)` | Same surface value type as `x`; equivalent representation with fresh mutable reach | Structural witness for the source value type |
| `copy_owned(h)` | `own[T]` containing equivalent represented `T` under fresh exclusive authority | Structural witness for represented `T` and valid observation through `h` |

`copy` elaborates to the core `copy_value` operation. The fresh-owner core operation is
named `copy_owned` as well. The operation is selected by its resolved identity and checked
witness, not by the spelling of an arbitrary user-declared function.

### The `copy` constraint

Use `copy` as the language-provided structural constraint for explicit independent copying.
As a constraint it requires the checked witness for the constrained type; as an operation,
`copy(x)` requests construction using that witness. `copy_owned(h)` requires this constraint
on represented `T`, while `copy(x)` requires it on the source value type.

An `own[T]` value can satisfy `copy` when its contents support checked copying, while still
moving by default. The constraint permits explicit construction and never enables implicit
copying or duplication of the original ownership token. An inspection may duplicate under
its loan rules while failing `copy`; these are separate compiler-checked properties.

In the experimental mode, retire the legacy `copy` concept's meaning of implicit duplication.
Keep implicit duplication as a compiler-tracked property; introduce no public `copyable` or
`duplicate` constraint. Legacy mode retains its current meaning until adoption. Migration
must audit existing `copy` bounds according to their uses rather than treating the shared
spelling as equivalent semantics.

### Copying through capabilities

```cinder
# frozen: shared_read[config]; config has a structural copy witness
let retained = copy(frozen)          # shared_read[config]; same frozen target
var editable = copy_owned(frozen)    # own[config]; fresh mutable reach

# view: inspect[config]
var from_view = copy_owned(view)     # own[config]; independent of view's loan
```

`copy(frozen)` requires a witness for the frozen handle value; it does not require copying
the target's contents. `copy_owned(frozen)` requires a witness for the represented target
and reconstructs owned descendants even when their source edges are frozen. It never thaws
the original. `copy_owned` can observe an owner, frozen handle, or valid scoped inspection
or edit; an edit source is reborrowed for observation during construction.

Scoped inspection and edit handles have no independent value-copy witness in the initial
core. `copy(view)` therefore fails even when `copy_owned(view)` succeeds. Ordinary inspection
duplication and edit reborrowing retain their existing capability rules. If represented
`T` itself contains scoped loans or unsupported resources, `copy_owned` cannot shed those
dependencies by copying; its witness must be rejected.

`clone` is not a synonym for either operation. Existing user or legacy library `clone`
methods retain their own declared contracts; their names establish no structural witness.
The paired names `copy` and `copy_owned` make the preserved value type versus fresh-owner
result explicit without assigning another meaning to `clone`.

## Legacy sharing

Use `shared_read[T]` for retained frozen sharing in the experiment. Legacy `shared[T]` and
the `shared expr` construction form are not aliases or unchecked conversions to this
capability. Read-only access through a reference-counted handle does not establish deep
immutability, valid frozen cleanup, or retained projection lifetimes.

Legacy sharing remains available in legacy mode during the experimental transition. Its
replacement and eventual removal follow the compatibility and adoption gates; this decision
does not remove current library code. Reusing the shorter `shared[T]` spelling for frozen
sharing is a possible adoption-time naming decision, requiring explicit migration. Neither
spelling alone establishes thread-transfer safety.

## Callable parameter names and result origins

Allow optional parameter names in `fn(...)` types so contracts can refer to their arguments:

```cinder
fn(part: slice[int32]) -> slice[int32] from part
```

Reuse the declaration result-origin form: `from part`, or `from {left, right}` for multiple
permitted origins. It bounds scoped dependencies throughout the result; it does not grant
authority or permit a loan of dead storage. For each invocation, substitute that call's
actual arguments into the contract. An input-only bound cannot hide a dependency on a
capture, an unrelated source, or an argument retained from an earlier invocation.

Parameter names are local contract binders. Consistently renaming a binder and its uses
does not change the contract, and the implementation's parameter names need not match.
Existing unnamed parameter types remain available when no contract needs to name them.

Infer checked contracts for source functions and closures, and preserve sufficient summaries
when passing or storing callable values. An expected callable contract is a checked
requirement on the supplied implementation. Omitting `from` neither asserts an input-only
origin nor erases captured dependencies; opaque boundaries need sufficient explicit contracts
or sound conservative interface defaults. Request only the information needed to establish
a useful relationship.

An input-only result bound permits captures used for computation, such as a scalar length
limit. It rejects a result borrowed from captured storage. Result origins do not constrain
writes: effects, transitions, captures, and cleanup still require complete checked summaries.
Capture-derived results use the environment-origin forms described below. See the
[callback translation](translations.md#worked-example-callback-result-origins).

## Write bounds and callable environments

Use `writes nothing` for a checked guarantee of no writes to pre-existing storage. Omit the
clause to request a sound inferred or conservative bound. Use a nonempty root set, such as
`writes {count}`, to constrain writes through named authority. Reject `writes {}` with a
diagnostic suggesting `writes nothing` for an explicit prohibition or omission for inference.
`writes none` is not an alternative spelling.

Write bounds apply to function declarations and callable types, independently of result
origins. A callback can modify a counter while returning a slice borrowed only from its input:

```cinder
fn(part: slice[int32], count: edit[usize]) -> slice[int32] from part
    writes {count}
```

In a callable type's contract, `self` denotes the callable environment, allowing a captured
counter instead of an explicit counter parameter:

```cinder
fn(part: slice[int32]) -> slice[int32] from part
    writes {self}
```

The bound includes storage reached through captured capabilities, such as a counter targeted
by a captured edit; it is not limited to the environment's own storage or ownership subtree.
At an enclosing function boundary, `writes {choose}` names the corresponding authority
reachable through the callback parameter `choose`. Checked summaries retain actual region
identities without exposing private capture field names. This environment root grants no
authority and does not choose an observing, editing, or consuming call mode. In method
declarations, `self` continues to denote the receiver; nested callable types have their own
contract scope.

Bounds include transitive calls and cleanup, and accept implementations that write less.
`writes nothing` is not a purity or termination guarantee: fresh allocation, writes to fresh
internal storage, and declared transitions remain possible. Complete transition and output
summaries still account for changes to availability, ownership, and cleanup. An omitted
clause never silently establishes `writes nothing`, and a `from` clause never implies it.
See the [counter callback translation](translations.md#worked-example-callback-write-bounds).

## Callable receivers and call modes

Use an optional leading environment receiver in a callable type, reusing method receiver
spellings:

| Callable type | Authority required to invoke it |
|---|---|
| `fn(self, ...) -> R` | Observation of the environment; reusable |
| `fn(mut self, ...) -> R` | Editing access to the environment; reusable |
| `fn(self: own[Self], ...) -> R` | Ownership of the environment; invocation consumes it |

The receiver is implicit at the call site: `choose(input)` supplies only the ordinary
arguments. Here `Self` denotes the callable's environment type; this spelling does not
require a heap allocation or expose private capture layout. `self` in the callable contract
denotes the same environment root used by write bounds.

Infer the required receiver authority from a source lambda's body, including uses through
helpers and cleanup. Reading a captured scalar permits observation; reborrowing a captured
edit requires editing access; moving a captured owner out requires consumption. Check
optional explicit callable constraints against the inferred entry and report which capture
use needs stronger authority. An omitted receiver retains sufficient inferred or sound
conservative call-mode information; it does not silently promise an observing entry.

Parameter passing mode and invocation mode remain separate:

```cinder
choose: &mut fn(mut self, part: slice[int32]) -> slice[int32]
    from part writes {self}
```

The outer `&mut` borrows the callback for editing; the inner `mut self` describes the
authority its call entry requires. A function can instead take ownership of an editing
callback and invoke it repeatedly before disposing of it. Owning the callback does not
make each invocation consuming. Borrowed observation cannot invoke an editing entry, and
borrowing cannot satisfy a consuming entry by implicitly copying or transferring its owner.

Calls reborrow the environment as required. An input-only result does not retain the
environment loan merely because the callback produced it, so sequential calls are possible
while earlier input-derived results remain live, provided all other loans permit. Captured
loans retain their own dependencies until their carriers retire.

Call mode, result origins, and write bounds are independent. An observing entry may write
through an explicit edit argument; `writes nothing` does not establish an observing entry
or prevent a consuming ownership transition. No call-mode constraint grants missing
authority. Frozen callable entries still require separate checking under frozen projections.
See the [call-mode translation](translations.md#worked-example-callable-receivers).

## Captures and environment-derived results

Keep the existing capture-list shapes with the experimental capability semantics:

| Capture | Meaning |
|---|---|
| `[x]` | Capture the value: move affine values; duplicate only under checked duplication rules |
| `[&x]` | Capture an observation loan |
| `[&mut x]` | Capture an edit loan or dependent edit reborrow |
| `[]` | Capture nothing |

An explicit list is closed: only listed enclosing locals are available to the lambda.
Without a list, infer the required enclosing variables and apply the same value-capture
rule. An owner or edit handle moves; a scalar or duplicable observation follows its checked
duplication rules. All dependencies and cleanup obligations survive capture. Independent
copying is never implicit. Borrowing is explicit and does not change according to escape
analysis or an expected callable constraint. An escaping borrowed capture must still have
a valid lifetime; request a legal ownership or retention change rather than silently
changing its capture mode.

Use `from self` to permit result dependencies supplied through the callable environment:

```cinder
fn(self) -> slice[int32] from self
    writes nothing

fn(self, part: slice[int32]) -> slice[int32] from {part, self}
```

The mixed form permits dependencies from either source or both. These are optional checked
constraints; source lambdas infer captures, receiver authority, origins, and effects.
`self` is local to the callable contract and does not expose private capture names.

The bound does not automatically attach every result to the environment's storage. A view
of an owner captured by value retains the dependencies supporting that captured ownership;
releasing or replacing the closure cannot invalidate a live result. A returned duplicate of
an already captured observation instead retains its original backing, grant-chain, and
lifetime dependencies. When sufficient checked summaries preserve that relationship, the
returned observation need not keep the closure alive. A less precise interface may retain
additional dependencies conservatively, but may never discard the actual ones.

Capture by observation keeps the external source's dependencies live through the capture
and any dependent results. Neither `from self` nor a consuming receiver permits a result
to borrow storage destroyed by invocation cleanup. Returning ownership or a separately
retained capability is checked under its own rules. See the
[capture translation](translations.md#worked-example-capture-origins).

## Remaining Q2 choices

- Contract forms beyond the worked examples and remaining legacy compatibility mappings.
- Complete translations and grammar integration for the advertised experimental subset.

Q2 remains partially resolved. This surface mapping changes no normative or implemented
Cinder behavior until the plan's formal, implementation, and adoption gates are satisfied.
