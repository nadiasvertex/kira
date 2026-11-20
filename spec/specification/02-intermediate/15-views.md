# 15. Views

**Status:** Partial

`slice[T]`/`mut slice[T]` as first-class borrowing values, view inference through returned values and struct fields, and references to single elements.

## Slices and Strings

A view is a borrow of part of a collection that may be passed, stored, *and* returned — like any borrow (see [Ownership and Borrowing](14-ownership-and-borrowing.md)), it is tracked without a lifetime annotation.

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

## Single Elements

A borrow of one element is an ordinary reference: `&xs[i]` is a `&T` and `&mut xs[i]` is a `&mut T`, the same types `&x` and `&mut x` give for a variable. A reference is a first-class value like a view: it can be passed, stored and returned, and the collection it points into stays borrowed for as long as the reference lives.

```cinder
def find(xs: &list[int32], want: int32) -> option[&int32]:
    for i in 0..xs.len():
        if xs[i] == want:
            return @some(&xs[i])
    return @none

def double_third(xs: &mut list[int32]) -> unit:
    if let @some(c) = xs.get_mut(2):
        *c = *c * 2                # mutates the element in place
```

`*c` reads the element and `*c = v` writes it. A `&mut T` gives exclusive access while it lives.

## Implementation status

- `slice[T]` is a real builtin type constructor (`src/semantic/types.cpp`, registered as `"slice"`, one type argument), with dedicated resolution and checking paths throughout `src/semantic/resolution.cpp` and `src/semantic/check.cpp` (index expressions with a range key produce a `slice`, `slice`/`slice_mut` are distinguished, `.len()` and other builtin methods are wired up). It is genuinely usable today.
- The tracking claim here — a view keeping its source borrowed for as long as the view lives, and never outliving it — **is** enforced by the ownership checker (see [Ownership and Borrowing](14-ownership-and-borrowing.md#implementation-status)). A view is a loan carried by whatever value holds it, through `let`/`var` bindings, reassignment, struct fields, `option`/sum payloads, `if`/`match` results, destructuring and pattern bindings, and across loop iterations; any conflicting access to the source while it is live — a borrow, a write, a move, or (for a `mut` view) a plain read — is rejected, as is a view outliving its source's scope or being returned from the function that owns its source. The `make_window` case above works because a call whose result stores a view carries every borrow its arguments made. What remains target design is *precision*: whole-variable granularity, and attributing a returned view to every reference argument rather than the exact one it was sliced from.
- A reference to an element is implemented for builtin collections and for a user type through `std.traits.index_ref` (`&v[i]`) and `std.traits.index_mut` (`&mut v[i]`). `list.get_mut(i)` gives an `option[&mut T]`. Until 2026-11-20 these were the separate types `cell[T]` and `mut cell[T]`, which the checker treated as equal to references without recording a conversion (`spec/todo.md` items 33 and 38).

## See also

- [Ownership and Borrowing](14-ownership-and-borrowing.md) — the borrow rules views follow, and how a borrow's lifetime is tracked.
