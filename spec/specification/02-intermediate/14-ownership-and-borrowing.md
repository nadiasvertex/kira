# 14. Ownership and Borrowing

**Status:** Complete

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
- The four bullet rules are enforced by a dedicated pass, `src/semantic/borrow_check.cpp` (`borrow_checker`), run after the move checker over the same checked result and gated to user files (`check_borrows`, called from `src/driver/driver.cpp`). It checks two things:
  - **No escape.** A plain-reference borrow (`&`/`&mut` typed `ref_kind`) is legal only in a *passing* position — a direct call argument, the callee, or the base of a field/index projection. Stored anywhere else (a `let`/`var` initializer, a `return` value, an assignment right-hand side, or an element of a tuple, array, struct literal, or collection) it is reported as escaping the call it was made for. A `&`/`&mut container[a..b]` borrows a *view* (its referent is a `slice`/`slice_mut`, see [Views](15-views.md)) and is exempt, since views are allowed to outlive a call.
  - **Exclusivity.** Because a borrow can never be stored, two borrows are only ever simultaneously live within one call's argument list, so exclusivity is a purely local per-call check with no dataflow. Within a call, any number of `&` borrows may coexist, but at most one `&mut`, and a `&mut` may not coexist with any `&`. Borrows are compared at whole-variable (root-binding) granularity — matching the move checker — so two borrows of different fields of the same variable are conservatively rejected.
- Not yet enforced, left as follow-up: exclusivity does not yet count a bare argument passed by implicit autoref to a `&`/`&mut` parameter, nor a method-call receiver, as a participating borrow (so `f(x, &mut x)` and `x.mutate(&x)` are not yet caught); field/index-precise places (distinguishing `&mut p.a` from `&mut p.b`); and closure borrow-capture, which the [Closures and Capture](16-closures-and-capture.md) chapter covers. The source material's `E0013` example is illustrative only — this compiler attaches no numeric error codes to diagnostics.

## See also

- [Views](15-views.md) — the one first-class way for a borrow to outlive a call.
- [Closures and Capture](16-closures-and-capture.md) — borrow vs. move capture follows the same rules.
