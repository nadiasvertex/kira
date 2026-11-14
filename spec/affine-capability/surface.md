# Affine capability surface mapping

Status: accepted experimental-subset spellings for declarations, receivers, loans, transfer,
freeze, copying, the `copy` constraint, and local bindings. Contract inference and optional
programmer constraints are accepted policies; the worked contract clauses remain syntax
candidates. This document does not amend
the implemented grammar or establish T08 completion. See [Q2 decisions](decisions.md#q2--ownership-default-and-explicit-copying)
and the [worked translation](translations.md).

## Types and parameter modes

| Meaning | Explicit type | Existing shorthand |
|---|---|---|
| Ordinary by-value parameter | `T` | — |
| Exclusive owner handle | `own[T]` | — |
| Scoped observation | `inspect[T]` | `&T` |
| Scoped editing | `edit[T]` | `&mut T` |
| Retained, permanently frozen sharing | `shared_read[T]` | — |

An ordinary `T` and `own[T]` remain distinct types; no owner wrapper is silently added or
removed. Ordinary by-value uses transfer affine values and duplicate values that have checked
duplication rules. An ownership transfer does not need an independent-copy witness.

Inspection and edit parameters request borrowing, inspection duplication, or reborrowing as
specified by the core conversion table. The shorthand reference types have the same authority,
loan identity, and lifetime obligations as their explicit capability forms. They do not
introduce a second borrowing model or independent-copy witness for scoped handles.

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

## Remaining Q2 choices

- Full view mappings, callable contract spellings, closure call-mode and capture spellings,
  and contract forms beyond the worked replacement example.
- Complete translations and grammar integration for the advertised experimental subset.

Q2 remains partially resolved. This surface mapping changes no normative or implemented
Cinder behavior until the plan's formal, implementation, and adoption gates are satisfied.
