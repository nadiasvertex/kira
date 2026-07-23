# 15. Views

**Status:** Partial

`slice[T]`/`mut slice[T]` as first-class borrowing values, view inference through returned values and struct fields, and single-element views (`cell[T]`/`mut cell[T]`).

## Slices and Strings

A view is the one kind of borrowing value the language permits to be passed *and* returned — its borrow is tracked without a lifetime annotation.

```kira
slice[T]       # a read-only view of a contiguous run of elements
mut slice[T]   # a mutable view
str            # a read-only view of UTF-8 text
```

A view is produced by slicing:

```kira
def first_half[T](xs: &list[T]) -> slice[T]:
    xs[0 .. xs.len() / 2]

let data  = [1, 2, 3, 4, 5, 6]
let front = first_half(&data)    # a view; data stays borrowed while front is alive
```

Rules:

1. The compiler infers that a returned view borrows from the argument it was sliced from, and keeps that source borrowed for as long as the view lives.
2. This inference extends to any struct that *stores* a view in one of its fields, not only to a bare returned view:

```kira
type window[T] = { s: slice[T], pos: usize }

def make_window[T](xs: &list[T]) -> window[T]:
    window{ s: xs.as_slice(), pos: 0 }
```

`make_window` returns an ordinary struct, but the compiler traces the view stored in its `s` field back to `xs`: the returned `window[T]` keeps `xs` borrowed for as long as it lives, exactly as if `make_window` had returned the slice directly. This is what lets a hand-written iterator type carry a `slice[T]` cursor over a collection without a lifetime annotation.

3. When a result needs to escape and stand on its own, return an owned value or a handle (an index) rather than a view.

## Single-Element Views: `cell[T]` and `mut cell[T]`

`cell[T]` (read-only) and `mut cell[T]` (mutable) are borrowed views of a single element — the result of a hash-map lookup, a tree search, an array access — filling the gap between a whole collection (`slice[T]`) and a temporary borrow in a call (`&T`).

```kira
cell[T]       # a read-only view of a single element
mut cell[T]   # a mutable view
```

Like `slice[T]`, a `cell` is a first-class value: passed and returned, with its borrow tracked automatically so it cannot outlive the collection it points into, and without lifetime annotations.

```kira
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
- The view-through-struct-field inference described above depends on the same borrow-exclusivity machinery discussed in [Ownership and Borrowing](14-ownership-and-borrowing.md#implementation-status), which is not yet a dedicated aliasing pass; treat the tracking claims here (a view keeping its source borrowed for its lifetime) as target design, verified only insofar as `slice` typechecks and lowers correctly, not as an enforced borrow-conflict diagnostic.
- `cell[T]` / `mut cell[T]` are **not implemented**: no `"cell"` builtin type constructor is registered anywhere in `src/semantic/types.cpp`, no `cell`/`mutable_cell` method exists in the standard library or the checker's builtin-method tables, and no lexer/parser keyword or codegen support exists. `cell` used as a type name today would resolve as an ordinary unbound identifier. This entire subsection is design-only.

## See also

- [Ownership and Borrowing](14-ownership-and-borrowing.md) — views are the escape hatch from "a borrow cannot escape a call."
