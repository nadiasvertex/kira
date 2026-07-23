# 14. Ownership and Borrowing

**Status:** Partial

Ownership transfer on assignment and call, `&`/`&mut` borrowing, and the invariant that a borrow cannot escape the call it was made for.

## Ownership

1. Assigning a value or passing it to a function transfers ownership by default. The source binding is no longer accessible afterward.
2. When the owner of a value goes out of scope, the value is freed (see [Shared Ownership and Drop](17-shared-ownership-and-drop.md)).

```kira
def process(data: list[int32]) -> int32:    # takes ownership of data
    ...

let numbers = [1, 2, 3]
let result = process(numbers)
# numbers is no longer accessible here — process owns it now
```

To use a value again after a call, copy it or lend it.

## Borrowing

A borrow lends a value to a function for the duration of one call. The owner keeps ownership; the callee may use the value but not move it. A borrow is a way of *passing* a value, not a value in its own right — it cannot be stored in a variable that outlives the call, returned, or placed in a struct field or collection. Because every borrow is bounded by the call that created it, there are no lifetime annotations.

Syntax: `&` marks an immutable-borrow parameter type and, at the call site, lends the value; `&mut` does the same for a mutable borrow. `&mut` is required at the call site as well as in the signature, so mutation through a call is visible where it happens.

```kira
def sum(data: &list[int32]) -> int32:
    var total = 0
    for x in data: total += x
    return total

let numbers = [1, 2, 3, 4, 5]
let total = sum(&numbers)                # lend it; numbers is still accessible

def double_all(data: &mut list[int32]) -> unit:
    for i in 0..data.len():
        data[i] = data[i] * 2

var ns = [1, 2, 3]
double_all(&mut ns)    # ns is now [2, 4, 6]
```

The declared rules:

- Any number of immutable borrows (`&`) may exist simultaneously.
- At most one mutable borrow (`&mut`) may exist at a time.
- A mutable borrow cannot coexist with any immutable borrows.
- A borrow cannot escape the call it was made for — it cannot be returned, stored in a field, or placed in a collection.

A construct that needs to hand back a window into a collection instead uses a view type, the one kind of borrowing value the language allows to outlive a single call; see [Views](15-views.md).

## Implementation status

- Ownership transfer and use-after-move detection are implemented in `src/semantic/move_check.cpp` (`move_checker`), which tracks per-binding move state across a function/lambda body and rejects use of a moved-from binding.
- `&T`/`&mut T` are real parameter and argument types (`type_kind::ref_kind`, resolved in `src/semantic/resolution.cpp` and `src/semantic/check.cpp`); a call through a `&`/`&mut`-typed parameter is recognized by the move checker as non-consuming (a UFCS call resolving to a `&`/`&mut` first parameter does not move the receiver, so calling it more than once is accepted — see `src/testdata/semantic_move_check_test/accept_repeated_borrowing_ufcs_calls.kira`).
- The four bullet rules above describe the target aliasing discipline (simultaneous immutable borrows; at most one mutable borrow; no mixing; no escape past the call). No dedicated aliasing/exclusivity pass exists in the compiler today — there is no `src/semantic/borrow_check.cpp` or equivalent, and no diagnostic corresponding to the `E0013` example in the source material was found. The move checker enforces use-after-move, not concurrent-borrow exclusivity. Treat the four rules as the specified target behavior, not as something you can currently trigger a compiler error for by, say, taking `&mut x` while `&x` is live.

## See also

- [Views](15-views.md) — the one first-class way for a borrow to outlive a call.
- [Closures and Capture](16-closures-and-capture.md) — borrow vs. move capture follows the same rules.
