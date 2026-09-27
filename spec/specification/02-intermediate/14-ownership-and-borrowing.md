# 14. Ownership and Borrowing

**Status:** Complete

Ownership transfer on assignment and call, `&`/`&mut` borrowing, and the invariant that a borrow never outlives what it borrows.

## Ownership

1. Assigning a value or passing it to a function transfers ownership by default. The source binding is no longer accessible afterward.
2. When the owner of a value goes out of scope, the value is freed (see [Shared Ownership and Drop](17-shared-ownership-and-drop.md)). This rule is not yet realized: the compiler emits no scope-exit `drop` glue, and every heap-backed value is allocated from a bump arena (`src/runtime/arena.h`) that never reclaims anything. The move checking that *would* drive rule 2 is implemented; the freeing is not.

```cinder
def process(data: list[int32]) -> int32:    # takes ownership of data
    ...

let numbers = [1, 2, 3]
let result = process(numbers)
# numbers is no longer accessible here — process owns it now
```

To use a value again after a call, copy it or lend it.

## Borrowing

A borrow lends a value without giving up ownership: the owner keeps it, and whoever holds the borrow may use the value but not move it. Most borrows are made for a single call, but a borrow is also a value in its own right — it may be bound to a name, stored in a struct field or collection, and returned, as long as it never outlives the value it borrows. There are no lifetime annotations: the compiler tracks how long each borrow is held by following the values that carry it.

Syntax: `&` marks an immutable-borrow parameter type and, at the call site, lends the value; `&mut` does the same for a mutable borrow. Writing `&mut` at the call site is the idiomatic form and keeps mutation-through-a-call visible where it happens. Two borrows are also created implicitly: a bare argument passed to a `&`/`&mut` parameter is borrowed rather than moved (implicit autoref), and a method receiver `x` in `x.m(...)` is borrowed for the call. These implicit borrows obey exactly the same rules below as an explicit `&`/`&mut`.

```cinder
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
- While a borrow is held, the borrowed value cannot be moved or replaced, and cannot be changed except through a `&mut` borrow — nor, while a `&mut` borrow is held, read directly.
- A borrow cannot outlive the value it borrows. In particular, a function cannot return a borrow of one of its own locals — scalar or not, directly or through a call — since every local is gone once the function returns. A borrow of a parameter the function was itself lent, or of a part of one, may be returned; the caller then keeps that value borrowed for as long as the returned borrow is used.
- A `&T` may be copied freely; a `&mut T` is exclusive, so binding or passing it by value moves it.

```cinder
type pair = { a: int32, b: int32 }

def second(q: &pair) -> &int32:    # a borrow of part of the caller's pair
    return &q.b

def dangle() -> &int32:
    let n = 5
    return &n                       # error: `n` is gone once `dangle` returns
```

When a function returns a borrow, the compiler does not know which argument it came from, so the caller keeps *every* value it lent that call borrowed while the result is in use. A view (see [Views](15-views.md)) is the same kind of tracked borrow, of a range or element of a collection.

## Implementation status

- All of this chapter's rules — ownership transfer, the four borrow rules, and the view and closure borrows of the next two chapters — are enforced by one pass, `check_ownership` (`src/semantic/ownership_check.cpp`), run after type checking over the user's own files (`src/driver/driver.cpp`). A file with an ownership error never reaches lowering.
- Each function and lambda body is lowered to a small control-flow graph of ownership events (`src/semantic/ownership_cfg.cpp`): every access to a local — read, move, borrow, write, and the end of its scope — and every flow of borrows from one value to another, in evaluation order. Loops, early exits (`return`, `break`, `continue`, `?`), and every pattern-binding form are ordinary edges and assignments in that graph, so no construct is handled by a special case.
- **Moves.** A forward "maybe moved" pass: passing a value by value, binding it, returning it, matching on it, or capturing it by move moves it, and any later use on *some* path is rejected — including the next iteration of a loop, and a use after a move in only one branch. Scalars, raw pointers, references and shared views copy. A bare argument passed to a `&`/`&mut` parameter is lent, not moved (the parameter's passing mode is recorded once by the checker, `call_argument_mapping::passing_by_param`).
- **Loans.** A *loan* is one borrow of a local: an explicit `&`/`&mut`, an implicit autoref of an argument or method receiver, a `slice`/`cell` view made by indexing, or a closure's `&`/`&mut` capture. A loan is live exactly as long as some value carrying it is live: the call whose arguments are being evaluated, a view binding, a struct that stores a view, a closure, a `for` loop's source, or the returned value. Liveness is computed backward over the graph (last use, not lexical scope), so a view that is overwritten before it is read again is dead in between.
- **Exclusivity.** Every access is checked against the live loans of the local it touches, with one table: a read conflicts with a live `&mut`; a `&` borrow conflicts with a live `&mut`; a `&mut` borrow, a write, a move, or the end of the local's scope conflicts with any live loan. A method receiver's mutable autoref is two-phase (as in Rust): a shared reservation while the call's arguments are evaluated, activated as `&mut` when the call runs — so `xs.set(i, xs.get(j))` is accepted and `xs.set(i, take(&mut xs))` is not.
- **Dangling borrows.** A loan still carried by a live value when the local it borrows goes out of scope is rejected: a view outliving the block of its source, or a returned view or closure that borrows one of the function's own locals. Views of borrowed parameters and of `self` may be returned; the caller then keeps that data borrowed.
- **Stored and returned borrows.** A `&`/`&mut` is a value like a view: bound, stored in an aggregate, or returned, it is a loan carried by whatever holds it, with the same exclusivity and dangling checks. A `&mut T` is move-only, so it cannot be copied into a second live alias. A reference returned by a call reads the original storage on both backends (checked for a scalar local and a struct field).
- Because the language has no lifetime annotations to say *which* argument a returned borrow comes from, a call whose result can carry a borrow (`checked_types::borrow_bearing_types`) conservatively carries every borrow its arguments and receiver made. This over-approximates (it can flag an alias that does not occur) but never misses one.
- Not yet enforced — precision limits, not soundness gaps: borrows are compared at whole-variable granularity (`&mut p.a` and `&mut p.b` conflict); a returned view borrows every argument rather than the one it came from; a move out of a field or element counts as a read (partial moves are not tracked); a type parameter's values are never tracked as moved (that needs to know which `T`s copy). `str`, though modelled as a view, is treated as an owned value. The source material's `E0013` example is illustrative only — this compiler attaches no numeric error codes to diagnostics.

## See also

- [Views](15-views.md) — the one first-class way for a borrow to outlive a call.
- [Closures and Capture](16-closures-and-capture.md) — borrow vs. move capture follows the same rules.
