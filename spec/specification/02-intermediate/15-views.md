# 15. Views

**Status:** Partial

`slice[T]`/`mut slice[T]` as first-class borrowing values, view inference through returned values and struct fields, and single-element views (`cell[T]`/`mut cell[T]`).

## Slices and Strings

A view is the one kind of borrowing value the language permits to be passed *and* returned — its borrow is tracked without a lifetime annotation.

```cinder
slice[T]       # a read-only view of a contiguous run of elements
mut slice[T]   # a mutable view
str            # a read-only view of UTF-8 text
```

A view is produced by slicing:

```cinder
def first_half[T](xs: &list[T]) -> slice[T]:
    xs[0 .. xs.len() / 2]

let data  = [1, 2, 3, 4, 5, 6]
let front = first_half(&data)    # a view; data stays borrowed while front is alive
```

Rules:

1. The compiler infers that a returned view borrows from the argument it was sliced from, and keeps that source borrowed for as long as the view lives.
2. This inference extends to any struct that *stores* a view in one of its fields, not only to a bare returned view:

```cinder
type window[T] = { s: slice[T], pos: usize }

def make_window[T](xs: &list[T]) -> window[T]:
    window{ s: xs.as_slice(), pos: 0 }
```

`make_window` returns an ordinary struct, but the compiler traces the view stored in its `s` field back to `xs`: the returned `window[T]` keeps `xs` borrowed for as long as it lives, exactly as if `make_window` had returned the slice directly. This is what lets a hand-written iterator type carry a `slice[T]` cursor over a collection without a lifetime annotation.

3. When a result needs to escape and stand on its own, return an owned value or a handle (an index) rather than a view.

## Single-Element Views: `cell[T]` and `mut cell[T]`

`cell[T]` (read-only) and `mut cell[T]` (mutable) are borrowed views of a single element — the result of a hash-map lookup, a tree search, an array access — filling the gap between a whole collection (`slice[T]`) and a temporary borrow in a call (`&T`).

```cinder
cell[T]       # a read-only view of a single element
mut cell[T]   # a mutable view
```

Like `slice[T]`, a `cell` is a first-class value: passed and returned, with its borrow tracked automatically so it cannot outlive the collection it points into, and without lifetime annotations.

```cinder
def find[T](xs: &list[T], pred: fn(&T) -> bool) -> option[cell[T]]:
    for i in 0..xs.len():
        if pred(&xs[i]):
            return @some(xs.cell(i))
    return @none

def find_and_double(xs: &mut list[int32]) -> unit:
    if let @some(c) = xs.mutable_cell(2):
        c.set(c.get() * 2)         # mutates the element in place
```

`c.get()` yields the element value; a `mut cell` enforces exclusive access.

## Implementation status

- `slice[T]` is a real builtin type constructor (`src/semantic/types.cpp`, registered as `"slice"`, one type argument), with dedicated resolution and checking paths throughout `src/semantic/resolution.cpp` and `src/semantic/check.cpp` (index expressions with a range key produce a `slice`, `slice`/`slice_mut` are distinguished, `.len()` and other builtin methods are wired up). It is genuinely usable today.
- The tracking claim here — a view keeping its source borrowed for as long as the view lives, and never outliving it — **is** enforced by the ownership checker (see [Ownership and Borrowing](14-ownership-and-borrowing.md#implementation-status)). A view is a loan carried by whatever value holds it, through `let`/`var` bindings, reassignment, struct fields, `option`/sum payloads, `if`/`match` results, destructuring and pattern bindings, and across loop iterations; any conflicting access to the source while it is live — a borrow, a write, a move, or (for a `mut` view) a plain read — is rejected, as is a view outliving its source's scope or being returned from the function that owns its source. The `make_window` case above works because a call whose result stores a view carries every borrow its arguments made. What remains target design is *precision*: whole-variable granularity, and attributing a returned view to every reference argument rather than the exact one it was sliced from.
- `cell[T]` / `mut cell[T]` are implemented, both directly (`xs.cell(i)`/`xs.mutable_cell(i)`, `.get()`/`.set()`) and as the return shape of a user `std.traits.index_mut` impl's `at_mut` (`&mut v[i]` on a type that implements it). Runtime representation is a bare pointer, identical to `&T`/`&mut T` — see `spec/todo.md` item 19 for the full implementation account.

## See also

- [Ownership and Borrowing](14-ownership-and-borrowing.md) — views are the escape hatch from "a borrow cannot escape a call."
