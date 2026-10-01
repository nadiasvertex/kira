# 14. Ownership and Borrowing

**Status:** Partial

Ownership transfer on assignment and call, which types copy instead of moving, moving out of places, `&`/`&mut` borrowing, and the invariant that a borrow never outlives what it borrows.

## Ownership

1. Assigning a value or passing it to a function transfers ownership by default. The source binding is no longer accessible afterward. Values of a `copy` type (below) are copied instead, and the source stays usable.
2. Every value has exactly one owner at a time. When the owner goes out of scope while still holding the value, the value is dropped (see [Shared Ownership and Drop](17-shared-ownership-and-drop.md)).

```cinder
def process(data: list[int32]) -> int32:    # takes ownership of data
    ...

let numbers = [1, 2, 3]
let result = process(numbers)
# numbers is no longer accessible here — process owns it now
```

To use a value again after a call, lend it instead.

## Copy and move

Whether using a value by value copies it or moves it depends only on its type. A type is `copy` when a bit-for-bit duplicate is an independent, correct value that owns nothing the original also owns. Every other type moves.

These types are `copy`:

- the scalars: `bool`, `char`, `byte`, `unit`, and every integer and floating-point type
- `str`, which is an immutable view of shared bytes
- `&T` for any `T`, and the raw pointers `*T` and `*mut T`
- the read-only views `slice[T]` and `cell[T]`

- a sum type — a user sum, `option[T]` or `result[T, E]` — whose every variant payload is `copy` and that does not implement `drop`, including a sum whose variants carry nothing (`ordering`). A sum value never changes after it is built, so sharing it is a copy.

Every other type moves, including `&mut T`, `slice_mut[T]`, `cell_mut[T]`, function values, and every struct, tuple, fixed array and `list`, whatever they contain: each of those can be changed in place. A type that implements `drop` is never `copy`.

`copy` is a built-in concept (see [Concepts](../03-advanced/35-concepts.md)). Like any concept it is satisfied structurally, with no `impl`, and it can be used as a bound:

```cinder
def larger[T: copy + ord](a: T, b: T) -> T:
    return if a > b: a else: b
```

A type parameter is `copy` only under a `copy` bound. Without one, a value of type `T` moves, and a second use after a move is rejected exactly as it is for a concrete type:

```cinder
def keep_if[T](x: T, pred: fn(T) -> bool) -> option[T]:
    if pred(x):          # moves x into pred
        return @some(x)  # error: `x` used after being moved
    return @none
```

The fix is to lend the value (`pred: fn(&T) -> bool`) or to require `T: copy`.

An array fill `[v; n]` puts the same value in every element, so `v` must be `copy`.

## Moving out of places

A place is a local, or a field, element, or dereference reached from one: `p`, `p.a.b`, `xs[i]`, `*r`. Using a place by value moves out of it unless its type is `copy`.

- **Fields of a local.** Moving out of a field of a local the function owns is a partial move. The moved field cannot be used again, and neither can the whole local, until the field is assigned a new value. The other fields stay usable. When the local goes out of scope, only the fields it still owns are dropped.
- **A type with its own `drop`.** A field of a value whose type implements `drop` cannot be moved out, because that type's `drop` needs the whole value.
- **Matching.** `match`, `if let`, `while let`, and a destructuring `let` on a field, element, or dereference move out of it only when a pattern binds a non-`copy` part by value. A pattern that binds only `copy` parts, or nothing, leaves the place as it was. To bind parts of a place that cannot be moved out of, match on a borrow of it (`match &self.head`).
- **Elements and dereferences.** Moving out of `xs[i]`, `*r`, or a field reached through a reference or through `self` is an error. The owner of that storage would still drop the value later, so the move would give it two owners. Borrow the place instead, or exchange it with `swap`, `replace`, or a collection method that removes the element, such as `pop`.

```cinder
type job = { name: str, input: list[int32], output: list[int32] }

def split(j: job) -> list[int32]:
    let out = j.output         # partial move: j.output is gone
    println(j.name)            # fine: j.name is untouched (and copy)
    return out                 # j.input is dropped at the end of split

def first(xs: &list[list[int32]]) -> list[int32]:
    return xs[0]               # error: cannot move out of `xs[0]`
```

Assigning to a place that still holds a value drops the old value, after the new value has been computed. Assigning to a place that was moved out of fills it again without dropping anything.

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
- **Moves.** A forward "maybe moved" pass: passing a value by value, binding it, returning it, matching on it, or capturing it by move moves it, and any later use on *some* path is rejected — including the next iteration of a loop, and a use after a move in only one branch. Scalars, raw pointers, `&T` references and shared views copy. A bare argument passed to a `&`/`&mut` parameter is lent, not moved (the parameter's passing mode is recorded once by the checker, `call_argument_mapping::passing_by_param`).
- **Loans.** A *loan* is one borrow of a local: an explicit `&`/`&mut`, an implicit autoref of an argument or method receiver, a `slice`/`cell` view made by indexing, or a closure's `&`/`&mut` capture. A loan is live exactly as long as some value carrying it is live: the call whose arguments are being evaluated, a view binding, a struct that stores a view, a closure, a `for` loop's source, or the returned value. Liveness is computed backward over the graph (last use, not lexical scope), so a view that is overwritten before it is read again is dead in between.
- **Exclusivity.** Every access is checked against the live loans of the local it touches, with one table: a read conflicts with a live `&mut`; a `&` borrow conflicts with a live `&mut`; a `&mut` borrow, a write, a move, or the end of the local's scope conflicts with any live loan. A method receiver's mutable autoref is two-phase (as in Rust): a shared reservation while the call's arguments are evaluated, activated as `&mut` when the call runs — so `xs.set(i, xs.get(j))` is accepted and `xs.set(i, take(&mut xs))` is not.
- **Dangling borrows.** A loan still carried by a live value when the local it borrows goes out of scope is rejected: a view outliving the block of its source, or a returned view or closure that borrows one of the function's own locals. Views of borrowed parameters and of `self` may be returned; the caller then keeps that data borrowed.
- **Stored and returned borrows.** A `&`/`&mut` is a value like a view: bound, stored in an aggregate, or returned, it is a loan carried by whatever holds it, with the same exclusivity and dangling checks. A `&mut T` is move-only, so it cannot be copied into a second live alias. A reference returned by a call reads the original storage on both backends (checked for a scalar local and a struct field).
- Because the language has no lifetime annotations to say *which* argument a returned borrow comes from, a call whose result can carry a borrow (`checked_types::borrow_bearing_types`) conservatively carries every borrow its arguments and receiver made. This over-approximates (it can flag an alias that does not occur) but never misses one.
- **References are addresses.** A `&x`/`&mut x` is the address of the place `x`, for every type, on both backends; a `*r` loads from it. `self` in a method on a heap-represented type is a `&T` (`mut self`: `&mut T`); a scalar `self` is passed by value. The checker still lets a `&T` stand where a `T` is expected for heap-represented types; lowering spells every such borrow and deref out (`hir::make_references_explicit`, `src/hir/reference_check.cpp`), and the driver rejects any it missed (`find_implicit_references`) rather than run it.
- Not yet enforced — precision limits, not soundness gaps: borrows are compared at whole-variable granularity (`&mut p.a` and `&mut p.b` conflict); a returned view borrows every argument rather than the one it came from. The source material's `E0013` example is illustrative only — this compiler attaches no numeric error codes to diagnostics.
- **Copy and move** is implemented. `type_table::is_copy` (`src/semantic/types.cpp`) classifies the built-in `copy` types, and `checked_types::copy_sum_types` the sums whose payloads copy (`checker::value_is_copy`); the checker answers the `copy` concept (`src/std/traits.cn`) from it, and the ownership graph uses it to decide what moves. A type parameter moves unless a `copy` bound (inline or `where`) puts it in `checked_types::copy_type_params`. Tests: `accept_copy_values_reused.cn` and `reject_type_param_reused_after_move.cn` (`src/testdata/semantic_ownership_check_test/`), `report_copy_bound_unsatisfied.cn` (`src/testdata/semantic_check_test/`). A category bound (`T: integer`, `T: numeric`) implies `copy`.
- **Moving out of places** is implemented in the ownership graph (`src/semantic/ownership_cfg.cpp`). A part of an owning local reached through fields alone is tracked as its own `field_path` local, so a partial move, a use of the whole afterward, and a refill by assignment are all ordinary moves and writes in the "maybe moved" pass. A move the rules forbid is an `invalid_move_event`, reported by `check_ownership`. A raw-pointer read is not a blocked move (ch. 38). Tests: `accept_partial_moves.cn` and `reject_moves_out_of_places.cn` (`src/testdata/semantic_ownership_check_test/`).
  - Not yet implemented: `match` on a whole local still moves it even when no pattern binds a non-`copy` part. The codegen corpus (`src/testdata/codegen_stress/`) is compiled without the ownership checker; some of those programs move a value twice or move out of `self`, which the checker would reject. The standard library is checked like user code.

## See also

- [Views](15-views.md) — the one first-class way for a borrow to outlive a call.
- [Closures and Capture](16-closures-and-capture.md) — borrow vs. move capture follows the same rules.
