# Affine surface translations

Status: contract syntax candidate for Q2 review, using the accepted type and conversion
spellings in the [surface mapping](surface.md). These examples are not implemented grammar,
runnable fixtures, or evidence that T08 is complete. See the
[decision ledger](decisions.md#q2--ownership-default-and-explicit-copying).

## Worked example: inspect, validate, and replace an owned object

The example uses a resource with observable scalar data. The same ownership flow applies
to an opaque resource, provided its operations and cleanup have checked contracts. No
resource copy witness is required by any function below.

```cinder
type resource = {
    id: int64,
    usable: bool,
}

type resource_slot = {
    item: own[resource],
}

type replace_error = @unsuitable

type replace_failure = {
    reason: replace_error,
    incoming: own[resource],
}

def suitable(item: inspect[resource]) -> bool
    writes nothing:
    return item.usable

def current(slot: inspect[resource_slot]) -> inspect[resource] from slot
    writes nothing:
    return &slot.item

def try_replace(
    slot: edit[resource_slot],
    incoming: own[resource]
) -> result[own[resource], replace_failure]
    writes {slot}
    outcome @ok(previous) transfers entry(slot.item) to previous, entry(incoming) to slot.item
    outcome @err(failure) preserves slot
    outcome @err(failure) transfers entry(incoming) to failure.incoming:
    if not suitable(incoming):
        return @err(replace_failure{
            reason: @unsuitable,
            incoming: incoming,
        })

    return @ok(exchange(slot.item, incoming))

def exercise(
    initial: own[resource],
    replacement: own[resource]
) -> int64:
    var slot = resource_slot{item: initial}
    let attempt = try_replace(&mut slot, replacement)

    match attempt:
        @ok(previous) => drop(previous)
        @err(failure) => drop(failure.incoming)

    let view = current(&slot)
    return view.id
```

The caller takes existing owners as inputs so the example does not choose a new allocation
or resource-acquisition syntax. `resource_slot` has no custom unique cleanup. Its remaining
owned field is structurally disposed when the local goes out of scope.

### Declaration and parsing choices

1. Capability constructors use square brackets: `own[T]`, `inspect[T]`, and `edit[T]`.
2. A result's optional `from` clause follows its type. `from slot` bounds the scoped
   dependencies in `current`'s result. Multiple possible origins use `from {left, right}`.
3. `writes` and `outcome` are header contract clauses, alongside existing `pre` and `post`.
   The final header clause ends with the colon introducing the implementation body, matching
   the existing contract layout. Without contract clauses, that colon follows the signature.
4. An outcome clause binds a result pattern followed by one relationship kind. `transfers`
   accepts a comma-separated list of `source to destination` relationships. `preserves`
   accepts a comma-separated list of storage roots or checked paths. Each clause's pattern
   bindings are local to that clause; repeated `@err(failure)` patterns describe the same
   outcome independently. No nested contract block or separate body keyword is introduced.
5. `entry(path)` is contract-only notation for the value or ownership relationship reached
   at function entry, after argument conversion and before execution of the body. It is a
   symbolic reference, not a runtime snapshot, independent copy, executable read, or new
   owner. Moving an input does not invalidate that reference inside its contract.
6. A bare destination path denotes its output-state location. A path rooted at an outcome
   binding denotes the corresponding result payload. Paths are statically checked against
   their types and outcome patterns; runtime-dependent paths require additional contracts
   outside this example.
7. `exchange(place, incoming)` is a structural intrinsic whose first argument is a place.
   Here `slot.item` selects the owner field's storage through the edit of its enclosing
   holder. It does not load or borrow the old resource's contents. Elaboration emits the
   core operation `exchange(slot, field_path(item), incoming)`. The operation requires valid
   edit authority for that slot, initialization, no conflicting loans, and a valid incoming
   value; it is not an ordinary function that accepts an arbitrary place by value.

The header clauses describe guarantees. They do not execute transfers or grant permissions.
In particular, `writes nothing` is a checked bound over pre-existing storage, not an effect-free
or termination annotation. `suitable` and `current` also have checked summaries with no
storage-changing transitions; an empty write bound alone would not establish that property.
Omission requests inference. The empty-brace spelling `writes {}` is rejected; it cannot
serve as either an empty assertion or an inference request.

### Contract meaning

Let A be the resource owner initially stored in `slot.item`, and B the incoming owner.
The holder remains live and initialized in both outcomes because it is lent by edit.

| Outcome | `slot.item` afterward | Result | Cleanup ownership |
|---|---|---|---|
| `@ok(previous)` | B | `previous` owns A | Caller owns both obligations, one through the holder and one through the result |
| `@err(failure)` | A, with original contents and ownership relationships | `failure.incoming` owns B | Caller owns both obligations, one through the holder and one through the error payload |

`transfers` relates the same resource authority at entry and exit. Returning a freshly
allocated equivalent resource does not satisfy it. Transfer identity by itself does not
promise unchanged resource contents; content guarantees require preservation or checked
logical postconditions. The checker accounts for all reachable cleanup and loan dependencies.
No entry owner can satisfy two exclusive output positions in a single outcome.

`preserves slot` promises unchanged represented contents and ownership relationships in
the holder and its owned subtree. It is an output guarantee, not a claim that no intermediate
write occurred. The overall `writes {slot}` still conservatively invalidates affected
storage-version facts on both outcomes. Matching `@err` does not revive old certificates;
using preservation requires checked evidence relating the current output to the entry state.

There is no `from` clause on `try_replace`: its result contains owners and scalar data, not
scoped loans. The input edit's lifetime is not attached to a transferred owning result.
`current` instead returns an inspection derived through the holder. Its checked summary
retains the granting and lifetime dependencies and records a dependent projection, not
duplication of an incoming inspection of the same resource.

### Core translation and exit accounting

The following is an explanatory trace rather than executable core syntax.

1. At the call to `try_replace`, create an edit loan of the holder and move B into an
   argument carrier. The caller's `replacement` binding becomes unavailable on every outcome.
2. Borrow B for `suitable`, load its `usable` scalar, retire the temporary inspection, and
   end that loan. B remains owned by the function.
3. On rejection, construct the error payload by moving B into `failure.incoming`. Return
   that payload without writing the holder. No resource is disposed by this return.
4. On success, select the initialized owner slot through the holder edit and prepare result
   storage. One exchange commit removes A into a result carrier and installs B in the slot.
   No user callback or recoverable failure can intervene between removal and installation.
   Transfer A into the `@ok` payload. No resource is disposed by this return either.
5. Check the outcome's post-state after callee cleanup. Neither outcome retains the argument
   edit or another scoped loan. Retire the argument edit carrier and end its loan. The caller
   can access the holder again; its item is an owner of a valid resource in either outcome.
6. Matching selects the ownership relationships for that outcome. The success arm disposes
   A; the error arm disposes B. The consumed payload cannot be cleaned up a second time.
   This requires consuming pattern elimination and partial-field cleanup from the core.
7. The remaining owner in the holder survives the match. `current(&slot)` makes a new
   inspection of the holder and derives an inspection of its owned resource. The returned
   view carries those dependencies after argument-carrier retirement.
8. Load `view.id` into an independent scalar return carrier. Retire the view and end its
   dependent loans before disposing the holder and its remaining resource. The scalar
   result outlives the resources. Across the complete caller exit, A and B are each disposed
   exactly once.

The structural exchange's commit is infallible. If a future surface models recoverable
result-preparation failures, they must occur before that commit and have explicit error
states that return or dispose B according to the declared API. This example does not choose
an allocation-failure policy or imply panic unwinding.

### Required rejection cases

- Reuse `replacement` after the call: it moved on both outcomes. On failure, the caller
  must bind `failure.incoming` to recover that owner under a new binding.
- Keep a `current(&slot)` view for a later use and call `try_replace` meanwhile: the live
  loan of the old resource and holder conflicts with replacement.
- Claim the error preservation contract after modifying the holder without establishing
  restoration: the body does not justify the declared output relationship.
- Return a new resource instead of the old slot owner in `@ok`: this violates the transfer
  relationship even if the result has the correct type and equal fields.
- Extract the old owner and then call fallible user code before installing the replacement:
  this is not the checked atomic exchange operation.
- Bind a holder with `let` and attempt replacement through that binding: local mutation
  requires `var` as well as suitable capability authority.

### Inference and interface boundary

Omitted clauses request sound, conservative inference from checked bodies, for public as
well as private definitions. The annotations above demonstrate optional programmer-written
constraints; none is mandatory merely because its function is public. The unconstrained
`try_replace` definition can omit all four contract lines and retain the same body. Likewise,
`suitable` can omit `writes nothing`, and `current` can omit both its `from` and `writes` clauses.
The compiler still establishes and exports complete checked summaries for callers.

Explicit constraints pin desired guarantees. Inferred guarantees can change with a body
change and require callers to be rechecked. Trait requirements and native/opaque boundaries
need sufficient interface contracts or sound defaults; recursive groups may infer candidate
summaries but must check every body against them before acceptance. Request an annotation
only where inference or a sound default cannot establish the required relationship.

Following the compiler-as-a-teacher approach, diagnostics explain which relationship is
missing or conservatively approximated, why that matters at the requested use, and the
smallest relevant annotation or implementation change. Suggestions must themselves check;
an annotation cannot make a false transfer or preservation claim true.

The clauses shown here cover this example. Sealed protocol-token transitions, persistent
freeze results, loans stored into caller-owned objects, higher-order calls, and more complex
outcome alternatives still require representative translations before Q2 or T08 is complete.

## Worked example: a borrowed collection slot

This example uses the accepted single-element view consolidation. `inspect[T]` and `edit[T]`
replace `cell[T]` and `mut cell[T]`; a slot loan records its containing allocation and checked
path. Reuse the `resource` type above. No function below requires a `copy` constraint.
Contracts are inferred from checked bodies; none needs a handwritten clause for this example.

```cinder
def replace[T](slot: edit[T], incoming: T) -> T:
    return exchange(*slot, incoming)

def replace_first(
    items: edit[list[own[resource]]],
    incoming: own[resource]
) -> own[resource]:
    let slot: edit[own[resource]] = items.at_mut(0)
    return replace(slot, incoming)

def exercise_slots(
    items: list[own[resource]],
    incoming: own[resource]
) -> int64:
    let previous = replace_first(&mut items, incoming)
    let previous_id = previous.id
    drop(previous)
    return previous_id
```

The element-access API shape is `list[T].at_mut(index) -> edit[T]`, with a result loan
derived from the collection receiver. An element observation API returns `inspect[T]`.
For single-element indexing, `&items[i]` and `&mut items[i]` expose those slot capabilities
directly; they do not borrow a temporary handle returned by the collection method.
These are library interface requirements, not unchecked compiler intrinsics selected by
method name. The collection must prove that its exposed slot supports the promised
operations and cannot relocate or disappear while the loan is live. Bounds checks follow
the collection's checked contract; the trace below assumes index 0 exists and lookup returns
normally. This does not select a recoverable-versus-fatal bounds failure policy.

`*slot` is a place expression: it designates the slot without moving the value stored there.
The compiler elaborates the exchange using the edit's stored place provenance. It does not
load an owner token, move it out, and subsequently assign a replacement. Assignment through
`*slot` instead uses the checked replacement/cleanup operation when the old value is not
wanted as a result. Binding the edit handle with `let` does not revoke its target authority.

### Slot authority versus child authority

| Capability | Target and permitted operation |
|---|---|
| `inspect[own[resource]]` | Observe the stored owner slot under capability-dependent load rules; cannot extract or replace its owner |
| `edit[own[resource]]` | Edit the collection slot; checked exchange can return its old owner and install a new one |
| `edit[resource]` | Edit the child resource's contents; cannot replace the owner stored in the collection |

The slot loan is grounded in the allocation containing the element and its element path.
The child loan is grounded in the resource's allocation. Both retain the relevant granting
and ancestor dependencies. An expected type or matching machine address cannot convert
child authority into slot authority. Inline elements do not acquire disjoint edit rights
merely because their indices differ.

### Ownership flow and core trace

Let the first slot initially own A, and let `incoming` own B. On normal return:

| Storage or carrier | Before exchange | After exchange |
|---|---|---|
| `items[0]` | Owner of A | Owner of B |
| `incoming` parameter in `replace` | Owner of B | Unavailable |
| Return carrier from `replace` | Uninitialized | Owner of A |

1. `replace_first` reborrows the caller's collection edit. `at_mut` creates a dependent slot
   edit with the checked backing allocation, element path, and stored type `own[resource]`.
   The result summary preserves those dependencies after the method's argument carriers end.
2. Passing `slot` to `replace` makes a dependent edit reborrow; it does not transfer or
   duplicate the original edit handle. `incoming` transfers into the ordinary `T` parameter.
3. Prepare the destination and result carrier, then check initialization, replacement validity,
   all live loans, and ownership-edge changes against the pre-transition forest. In one
   exchange commit, move A's owner to the result and B's owner into the initialized slot.
   Update affected storage versions and ancestor effects. No resource cleanup runs in this
   exchange, and no user code or modeled failure occurs between removal and installation.
4. Return A's owner through `replace_first`. The return value carries its own resource
   lifetime and cleanup obligations, with no dependency on the collection slot merely
   because it came from that slot. End dependent reborrows only after their carriers retire.
5. `exercise_slots` reads A's scalar ID and explicitly disposes A. Its list still owns B;
   normal scope cleanup disposes B and the other remaining elements exactly once.

The generic helper depends only on the element type and checked slot authority. A hash table
or tree exposing an equivalent slot capability can use the same helper. This hides collection
representation without erasing element types or loan dependencies. A sum or a separately
checked trait-object abstraction is needed when the elements themselves have different
concrete types. Range access continues to return a range view, not an edit of a temporary
slice descriptor.

### Required rejection cases

- Pass only an `edit[resource]` to replace an `own[resource]` element: the required slot
  authority is absent.
- Read the stored owner by value or leave the borrowed slot empty: a loan does not grant
  ownership extraction.
- Keep a returned or stored slot view for later use and grow, remove from, move, or destroy
  its collection in a way that invalidates the view: the surviving loan blocks that action.
- Exchange while an outgoing child has a conflicting surviving loan, or install a replacement
  borrowing from that child: check the pre-transition forest and reject the invalid transfer.
- Create simultaneous edit views of two inline elements using only unequal indices: the
  views conflict within their containing sequence or inherited split partition.
- Export a slot view from a collection that cannot preserve its lifetime or representation
  under the advertised replacement operations: the collection's method contract cannot check.

## Worked example: range views and reborrowing

These functions use the accepted scoped range mapping. Their checked contracts are inferred;
no lifetime or origin annotations are needed. The normal-return trace assumes at least three
elements in `data`, so the middle range is nonempty. Bounds checks remain required; this
example does not choose a recoverable-error or fatal-error policy.

```cinder
def middle(xs: inspect[list[int32]]) -> slice[int32]:
    return xs[1 .. xs.len() - 1]

def middle_mut(xs: edit[list[int32]]) -> mut slice[int32]:
    return &mut xs[1 .. xs.len() - 1]

def first(xs: slice[int32]) -> int32:
    return xs[0]

def clear_first(xs: mut slice[int32]) -> unit:
    xs[0] = 0

def exercise_ranges(data: edit[list[int32]]) -> int32:
    let window = middle_mut(data)
    let before = first(window)
    clear_first(window)
    let moved = &>window
    let after = first(moved)
    return before + after
```

`xs[a .. b]` and `&xs[a .. b]` both request observation and yield `slice[T]` for
stored element type `T`. `&mut xs[a .. b]` requests editing and yields `mut slice[T]`.
The range capability carries its shape and backing provenance directly; the explicit borrow
does not borrow a temporary descriptor. Text observation slicing instead yields `str` and
checks UTF-8 boundaries as well as bounds. No range construction copies elements.

The mutable trace has the following authority flow:

1. `middle_mut(data)` reborrows the incoming edit and creates a checked subrange. Its inferred
   result origin preserves the backing allocation and granting chain after the call returns.
   Conflicting use of `data` stays suspended while that returned loan survives.
2. `first(window)` makes an observation reborrow for the `slice[int32]` parameter. The scalar
   load returns an independent `int32`; the temporary observation can end after the call.
3. `clear_first(window)` makes an edit reborrow for the `mut slice[int32]` parameter. The store
   uses checked element-slot authority, with the usual write effects and version invalidation.
4. `&>window` transfers the mutable range and its existing dependencies to `moved`. `window`
   becomes unavailable. An ordinary `let moved = window` would also move it.
5. The final `first(moved)` observes the same backing range and reads zero from its first
   element. When no carrier or dependent view needs the range loan, conflicting authority
   through `data` can resume. Ending the range loan does not destroy the backing collection.

If a helper returns or stores a dependent subrange, its loan survives the helper call and
continues to suspend conflicting authority. Observation-view duplication preserves all such
dependencies. For `mut slice[own[resource]]`, element borrowing yields `edit[own[resource]]`,
so the preceding slot-replacement example applies without a copy witness.

### Required rejection cases

- Infer editing for `xs[a .. b]` merely because a mutable range is expected: the expression
  requests observation; the diagnostic should point to `&mut xs[a .. b]` when editing is legal.
- Pass a read-only range to a mutable range parameter, or silently weaken an explicit editing
  request to fit an observation parameter: explicit authority requests remain distinct.
- Use `window` after transferring it, or exercise conflicting parent authority while a returned
  or stored dependent range remains live: the carrier and granting-chain rules still apply.
- Create simultaneous edit subranges using only nonoverlapping bounds: only checked split
  provenance establishes independent range partitions.
- Slice outside valid bounds, form an invalid UTF-8 text boundary, or access uninitialized
  elements: the checked range obligations must be discharged or checked at runtime.
- Treat a scoped range over frozen storage as independently retained and release a source
  handle required by its loan: frozen contents do not erase source lifetime dependencies.

The library `.slice()` method creates an independently retained frozen range as shown below.
Scoped loans do not permit freezing the backing collection through the view or implicitly
materializing an owned copy.

## Worked example: named-field borrowing and range splitting

Assume `self` has edit authority over a structural product with fields `items` and `total`,
the iterator borrows only `items`, and arithmetic is valid under the selected integer rules:

```cinder
for x in self.items:
    self.total += x
```

Elaboration projects an inspection rooted at the checked `items` path and retains the
iterator's dependencies for their actual lifetimes. Loading and storing `total` uses its
distinct named-field footprint; no temporary whole-`self` loan is introduced for that
projection. An owned backing allocation reached through `items` remains protected from
relocation or destruction. The scalar load of `x` carries no loan after loading. Effects
include the write to `total`; allocation-wide versions may invalidate prior facts about
`self` even though the field access is permitted. Loop exit disposes iterator state under
the ordinary sequential-generator and cleanup rules.

For splitting, assume `data` is a live `mut slice[int32]` with four initialized elements.
The following schematic trace uses the accepted built-in call; names denote the pair's
components, without selecting additional destructuring syntax:

```text
(left, right) = split_at(data, 2)
(head, tail) = split_at(left, 1)
write head[0] = 10
write tail[0] = 20
write right[0] = 30
```

1. Check `2 <= length(data)` and the source authority. Create a partition and child loans
   for `[0,2)` and `[2,4)`, dependent on `data` and its backing lifetime. The source handle
   stays bound, with conflicting authority suspended.
2. Check the nested split relative to `left`. Its children cover `[0,1)` and `[1,2)` in the
   original backing. They inherit the outer partition, so both remain disjoint from `right`.
3. Each write borrows an initialized slot within its own partition, applies the ordinary
   representation and effect checks, and invalidates the required versions. No element
   copying, owner extraction, or fresh backing allocation occurs.
4. A helper may return a child or dependent view, preserving its partition and granting
   dependencies in its result contract. Retiring a local descriptor does not end that loan.
5. End each child loan only after its carriers and descendants retire. Authority resumes
   only where no remaining loan conflicts; whole-range access through `data` requires all
   conflicting children to end. Disposing views never disposes the backing elements.

Required acceptance cases include nested named fields, independent named-field edits,
splits at zero and length, empty input, and a split child transferred through a helper while
its disjoint sibling remains usable. Required rejection cases include:

- Borrow a field and then access the same path or a prefix through competing edit authority,
  replace/drop/freeze the parent, or call a method requiring a whole-parent edit.
- Treat two fields containing aliases of the same external mutable target as disjoint.
- Borrow two arbitrary indices or subranges independently using bounds as the only evidence.
- Split outside the visible range: reject statically or fail the bounds check before either
  result exists, with no partial split state.
- Widen a child, duplicate its edit, or use it to invalidate the backing storage.
- End one child and access the whole parent while its sibling or a returned descendant
  remains live; transferring or retiring the original child handle cannot evade this check.

These translations are T08 coverage requirements, not evidence that proofs or compiler
implementation are complete.

## Worked example: retained frozen ranges

This example uses the accepted `shared_slice[T]` and `shared_str` types and ordinary library
`.slice(range)` method over a checked retained-projection intrinsic. These are experimental
design translations, not implemented grammar or runnable fixtures. The intrinsic's exact
name and ABI are not selected here; there is no universal `retain_range` builtin.

```cinder
def frozen_middle(items: own[list[int32]]) -> shared_slice[int32]:
    let frozen = &!items
    return frozen.slice(1 .. frozen.len() - 1)

def retained_prefix(window: shared_slice[int32]) -> shared_slice[int32]:
    return window.slice(0 .. 2)

def text_prefix(text: shared_str) -> shared_str:
    return text.slice(0 .. 5)
```

The normal-return trace assumes at least two list elements for `frozen_middle`, at least two
visible elements for `retained_prefix`, and at least five bytes with a UTF-8 boundary at byte
five for `text_prefix`. These assumptions do not remove checks or select Q5's failure policy.
Contracts are inferred; an independently retained result does not need a scoped origin
annotation tying it to the lifetime of a local source handle. It still denotes the same
frozen backing storage, not a fresh independent copy.

For `frozen_middle`, the authority and cleanup trace is:

1. The incoming list owner moves into `items`. Freezing consumes it and yields
   `shared_read[list[int32]]`, subject to the structural freeze and release requirements.
2. The library method validates bounds and initialized elements. Its checked intrinsic
   receives existing persistent frozen authority and valid backing provenance, and retains
   the storage required by the selected range before exposing a `shared_slice[int32]`.
3. The result moves into the return slot. Temporary receiver borrows end; the local `frozen`
   handle can be released because the result has its own retention obligation.
4. The caller can read the returned range after the helper has returned. No elements were
   copied. The last retaining handle/projection and any scoped lifetime dependencies govern
   reclamation; even a small range may retain the entire backing allocation.

The method on retained collection and text ranges follows the same rules. Subrange bounds
are relative to the visible source range. A text range validates UTF-8 boundaries as well as
byte bounds. Neither operation copies elements or grants mutable authority. Duplicating a
retained handle retains the same frozen storage without an element-copy witness.

Ordinary indexing keeps its scoped meaning:

```cinder
let borrowed = window[0 .. 2]        # slice[int32]; depends on window
let retained = window.slice(0 .. 2)  # shared_slice[int32]; independent retention
```

`retained` can survive release of `window`; a live `borrowed` prevents release of the source
handle on which its loan depends. An unrelated retaining handle does not erase that loan.
Likewise, this helper is rejected:

```cinder
def borrowed_middle(items: own[list[int32]]) -> slice[int32]:
    let frozen = &!items
    return frozen[1 .. frozen.len() - 1]
```

The result would keep a loan on local `frozen`, blocking the cleanup required to leave the
function. A teaching diagnostic should identify the local source and return, explain the
release conflict, and suggest `.slice()` with a retained result type, or keeping the source
handle in the caller and returning a scoped view of that caller-provided source.

Required validation also rejects creating retention from an ordinary scoped view or a raw
pointer/count without persistent frozen authority, treating `shared_read[slice[T]]` as a
conversion that sheds a scoped loan, accessing outside a retained subrange, and exposing
stored owner tokens through frozen elements. Invalid bounds, uninitialized elements, and
invalid text boundaries must be rejected or checked before the retained result is exposed.
Final release must reclaim backing storage and release frozen outgoing edges exactly once,
without mutable cleanup or thawing. Library bodies and intrinsic contracts remain subject
to verification; method names alone confer no retention authority.

## Worked example: callback result origins

This is experimental design syntax, not an implemented or runnable language example.
Optional parameter names in callable types let a result-origin bound name the argument
supplying a borrowed result:

```cinder
def first_three(input: slice[int32]) -> slice[int32]:
    return input[0 .. min(3, input.len())]

def select(
    input: slice[int32],
    choose: fn(part: slice[int32]) -> slice[int32] from part,
) -> slice[int32]:
    return choose(input)

# items: own[list[int32]]
let selected = select(items[0 .. items.len()], first_three)
```

The checked summary inferred for `first_three` bounds the result's scoped dependencies by
`input`. That satisfies the expected callback contract after binding its argument to `part`;
the implementation parameter need not have the same name. The summary inferred for `select`
likewise bounds its result by its own `input`. Either declaration may explicitly constrain
that relationship with `from input`; neither needs the annotation to establish this example.

The bound applies separately to every invocation, including calls with different source
allocations. It is not a dependency on whichever argument the callback most recently saw.
An implementation may not substitute captured storage or an earlier call's input unless
those dependencies are permitted by its checked interface.

The ownership and loan trace is:

1. Range indexing creates a scoped observation of `items`, with checked bounds and backing
   provenance. No elements or owners are copied.
2. `select` passes its observation to the callback under the ordinary parameter rules.
   The callback result retains the required input backing, grant-chain, and lifetime
   dependencies, whether it duplicates an incoming observation or creates a dependent view.
3. The result carrier receives those dependencies before intermediate argument carriers
   retire. Forwarding the result through `select` preserves them.
4. While `selected` remains live, conflicting mutation, relocation, or destruction of
   `items` is rejected. The result acquires no scoped dependency on the local callback
   carrier merely because it was produced by a callback.

A captured scalar can influence the range without supplying its storage:

```cinder
let limit: usize = 2
let limited = part => part[0 .. min(limit, part.len())]
let selected = select(items[0 .. items.len()], limited)
```

The inferred result still depends only on that call's `part`. The implicit value capture
duplicates the scalar `limit`, and reading it permits an observing entry. In contrast,
this callback cannot satisfy the input-only origin bound:

```cinder
# elsewhere: slice[int32], from an unrelated source
let choose_elsewhere = part => elsewhere
let selected = select(items[0 .. items.len()], choose_elsewhere)
```

A teaching diagnostic should point to the returned capture and the callback requirement:

```text
callback result borrows captured `elsewhere`
required contract permits result dependencies only through `part`

return a view derived from `part`, or change the callback
contract to account for the captured dependency
```

Changing the contract must retain the real lifetime dependency and satisfy the caller's
uses; an annotation cannot hide it. Capture origins use `from self`, or a mixed bound such
as `from {part, self}`, as worked through below.

No `writes nothing` promise follows from this result-origin bound. The complete inferred or
explicit interface still accounts for effects, transitions, capture dependencies, and
cleanup. Unnamed `fn(...)` parameter types remain available; omitted origin clauses must
preserve sufficient checked summaries or use sound conservative interface defaults.
Unavailable bodies cannot justify an inferred input-only promise.

Required validation includes consistent binder renaming, unnamed forms, independently
instantiated calls, forwarding without dependency loss, compatible computation-only
captures, capture-backed result rejection, and preservation through stored callable values
and exported metadata. Formal validation and implementation remain pending.

## Worked example: callback write bounds

This example uses accepted experimental write-bound and capture spellings. Callable
receivers are worked through below. This is not an implemented fixture.
An explicit counter argument keeps the two relationships visible:

```cinder
def counted_prefix(
    input: slice[int32],
    count: edit[usize],
) -> slice[int32]:
    *count += 1
    return input[0 .. min(3, input.len())]
```

Assume the increment is within range under the chosen arithmetic rules. The compiler infers
`from input` and `writes {count}`. A compatible callback constraint is:

```cinder
fn(part: slice[int32], count: edit[usize]) -> slice[int32] from part
    writes {count}
```

The counter edit is reborrowed for the call. The returned slice carries input dependencies,
not a counter dependency; the counter can be reused once its other loans permit. The
argument capabilities must still be compatible: these clauses cannot authorize a counter
edit that conflicts with the input observation.

Alternatively, capture an edit of the counter explicitly:

```cinder
var calls: usize = 0
let choose = [&mut calls] part => counted_prefix(part, &mut calls)
let selected = select(items[0 .. items.len()], choose)
```

The callback still satisfies `from part`, but writes through its captured edit of `calls`.
The original `select` example omitted a write constraint, so its complete checked summary
must include that effect. To pin a bound allowing writes through captured authority, use:

```cinder
fn(part: slice[int32]) -> slice[int32] from part
    writes {self}
```

Here `self` is the callable environment root. Its footprint follows the edit to the actual
counter; it is not limited to the environment's own bytes. At `select`'s boundary, a bound
`writes {choose}` includes this authority through its callback parameter. These bounds do
not expose private capture fields, grant authority, or select an environment call mode.
The counter loan stays live while the closure or another carrier retains it. The returned
slice does not inherit that loan merely because the callback incremented the counter.

To prohibit writes to pre-existing storage, spell the constraint explicitly:

```cinder
fn(part: slice[int32]) -> slice[int32] from part
    writes nothing
```

`first_three` satisfies this constraint; both counter-updating forms fail it. A diagnostic
should identify the counter write (including any helper or cleanup responsible) and the
`writes nothing` constraint it violates. Omission instead requests inference or a sound
conservative bound. The spelling `writes {}` is rejected with guidance:

```text
use `writes nothing` to prohibit writes to pre-existing storage,
or omit the clause to infer a sound write bound
```

The no-write constraint still permits fresh allocation and writes to fresh internal storage.
Transitions, ownership changes, and cleanup have their own checked summaries; no purity,
termination, rollback, or unchanged-availability guarantee follows from `writes nothing`.

Required validation covers declaration and callable parsing, rejected empty-brace syntax,
omitted versus explicit constraints, writes through captured edits, transitive helper and
cleanup effects, compatibility of result origins with independent counter writes, and
propagation of actual region footprints through higher-order calls and metadata.

## Worked example: callable receivers

This example uses accepted experimental callable receiver and capture spellings. It is
not an implemented fixture. Capture-derived result origins are worked through below.

Reuse `counted_prefix` from the write-bound example. A callback that captures an edit of
`calls` and passes a reborrow to that helper needs editing access to its environment on
each invocation. Its inferred contract is:

```cinder
fn(mut self, part: slice[int32]) -> slice[int32]
    from part writes {self}
```

The environment receiver is implicit at invocation. `select` borrows the callback for
editing and invokes its reusable editing entry:

```cinder
def select(
    input: slice[int32],
    choose: &mut fn(mut self, part: slice[int32]) -> slice[int32]
        from part writes {self},
) -> slice[int32]:
    return choose(input)

var calls: usize = 0
var choose = [&mut calls] part => counted_prefix(part, &mut calls)

let first = select(items[0 .. items.len()], choose)
let second = select(items[0 .. items.len()], choose)
```

Assume valid initialized `items` storage, compatible input and counter capabilities, and
increments within the chosen arithmetic bounds. Infer `select`'s result origin from `input`
and its write bound through `choose`; neither needs another source annotation. The outer
`&mut` in the parameter type specifies how `select` receives the callback. The inner
`mut self` specifies what invoking its entry requires. Bare arguments automatically select
the declared borrowing mode, so both calls reborrow `choose` without moving it.

For each call, establish an environment edit reborrow, then reborrow the captured edit
for `counted_prefix`. Retire those call-local reborrows after their last required use.
The returned slice carries the input's backing, grant-chain, and lifetime dependencies;
it does not acquire a dependency on the callback environment or counter merely from
passing through the call. Thus the second call remains valid even if `first` is used
afterward. The closure still carries the counter edit, and direct access to `calls`
must wait until that loan and any other conflicting dependencies retire.

A function may instead take ownership of the same editing callback, invoke it repeatedly,
and dispose of it. Owning the callback does not change its entry into a consuming call.
Conversely, a callback with `fn(self: own[Self], ...)` consumes its environment when invoked;
observation or editing loans cannot supply that ownership. Attempting to call the counter
callback through observation must identify the captured edit reborrow and explain that
the invocation requires editing access. A consuming callback supplied to the borrowed
`select` must similarly be rejected with guidance about the ownership it requires.

The complete receiver family reuses method syntax: `fn(self, ...)` observes,
`fn(mut self, ...)` edits, and `fn(self: own[Self], ...)` consumes the environment.
Source lambdas infer the required receiver from their bodies; optional callable constraints
are checked. Omitting a receiver preserves inferred or sound conservative call-mode
information rather than promising observation. `Self` denotes the environment type without
requiring a particular heap representation or exposing capture fields.

Call modes do not replace effects or result origins. An observing callback can write
through an explicit edit argument. A consuming callback can move a captured owner into its
result without a write to pre-existing storage, so `writes nothing` does not establish
an observing entry. Neither a receiver nor a write bound grants missing authority.

Required validation covers inferred and explicit receivers, implicit call-site receivers,
borrowed versus owned callback parameters, repeated editing calls with earlier input-derived
results still live, preserved capture loans, rejected editing through observation, rejected
consumption through loans, and call-mode summaries preserved through values and metadata.

## Worked example: capture origins

These examples use accepted experimental capture and origin spellings. They are not
implemented fixtures. Each snippet starts with independent initialized `items` storage
of type `own[list[int32]]`; all ranges are checked under the chosen bounds rules.

First, capture ownership and return a view of the captured list:

```cinder
let reader = [items] () => items[0 .. min(3, items.len())]
let selected = reader()
```

Creating the closure moves `items`, including its authority and cleanup obligation, into
the environment. The source binding becomes unavailable even though invocation only
observes the captured list. The compiler infers the callable contract:

```cinder
fn(self) -> slice[int32] from self
    writes nothing
```

The returned view requires the dependencies supporting that captured ownership to remain
live. Releasing or replacing the closure cannot destroy the list while `selected` has a
later use. Multiple observing calls remain compatible, and moving a carrier must preserve
all required ownership and lifetime relationships. `from self` is optional here because
the source body supplies enough information to infer the result origin.

Second, explicitly borrow the external owner:

```cinder
let reader = [&items] () => items[0 .. min(3, items.len())]
let selected = reader()
```

The inferred callable also permits `from self`, but the environment contains an observation
rather than the owner. The original owner remains outside the closure. The capture and any
dependent result keep the external source's required loans live, preventing conflicting
mutation or destruction. Escaping the closure cannot extend that source's lifetime; a
borrow of storage destroyed on return must be rejected.

Third, capture an existing observation and return its duplicate:

```cinder
let saved = items[0 .. items.len()]
let reader = [saved] () => saved
let selected = reader()
```

Value capture duplicates `saved` under the checked observation-duplication rules, preserving
its original target, grant chain, and lifetime dependencies. Returning it duplicates that
observation again. A sufficiently precise checked summary records that `selected` needs
the original backing dependencies rather than the closure's storage. Retiring the closure
therefore need not invalidate `selected`. A less precise boundary may conservatively retain
additional dependencies, but cannot drop the dependencies on `items` or assume a fresh loan
origin merely because the view passed through an environment.

The origin bound `from self` permits dependencies supplied through captures; it does not
unconditionally borrow the environment's own storage. Portable summaries preserve sufficient
dependency relations to justify the claimed precision without exposing capture field names.

A callback returning either its argument or captured storage uses a mixed bound:

```cinder
fn(self, part: slice[int32]) -> slice[int32] from {part, self}
    writes nothing
```

The result can carry dependencies from either source or both. An input-only `from part`
constraint still rejects a possible capture-derived result. Optional explicit constraints
check inferred contracts rather than changing captures or manufacturing new authority.

The same value-capture policy applies without a list: infer the required enclosing variables
and move affine values or duplicate according to checked capability rules. `[&mut x]`
explicitly captures an edit loan or reborrow; capturing an existing edit as `[x]` instead
moves that affine handle. `[]` admits no enclosing captures, and an explicit list excludes
unlisted enclosing locals. Neither escape analysis nor an expected callable type silently
changes a value capture into a borrow. No independent copy is introduced to keep a source
usable; that requires an explicit copy operation with its own witness.

Diagnostics should identify the captured owner or external lender supporting a returned
view, explain which use keeps its dependency live, and point to the conflicting destruction,
replacement, or mutation. If an opaque boundary lacks a needed relationship, show the
smallest useful contract or ownership change. A consuming callable that returns a loan of
captured storage destroyed by invocation cleanup is rejected even with `from self`.

Required validation covers all explicit capture forms and omitted lists, source availability,
closed-list enforcement, escaping borrowed lifetimes, captured ownership versus stored
observations, mixed origins, checked summary preservation through forwarding and metadata,
conservative boundary behavior, and rejection of views of storage destroyed by cleanup.
