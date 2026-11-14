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
    writes {}:
    return item.usable

def current(slot: inspect[resource_slot]) -> inspect[resource] from slot
    writes {}:
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
In particular, `writes {}` is a checked bound over pre-existing storage, not an effect-free
or termination annotation. `suitable` and `current` also have checked summaries with no
storage-changing transitions; an empty write bound alone would not establish that property.

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
`suitable` can omit `writes {}`, and `current` can omit both its `from` and `writes` clauses.
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
