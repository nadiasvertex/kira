# 43. `list[T]`

**Status:** Implemented

Covers `list[T]`'s representation, growth strategy, ownership, and the operations available on it.

## Representation

`list[T]` is an ordinary Cinder struct in `std.list` (`src/std/list.cn`), written over `std.mem` and the [`machine` layer](../../03-advanced/38-machine-layer.md). The compiler has no special knowledge of it: literals, indexing, and `for` reach it through the same traits any user collection can implement.

```cinder
pub type list[T] = { len: usize, cap: usize, data: *mut T }
```

- **Storage.** Exactly `cap` elements at `data`, or a null `data` when `cap == 0`. `len <= cap` always; slots `len..cap` are zeroed but hold no meaningful value.
- **Element width.** Elements are stored at their own size (`size_of[T]()`), so `list[bool]` uses one byte per element.
- **Growth.** `push` on a full list doubles the capacity, starting at 4, via `std.mem.resize`. A sequence of `n` pushes performs O(log n) reallocations.
- **`vector[T]`** is an alias (`pub type vector[T] = list[T]`). There is one growable-sequence type; the alias only lets either name be used.

An unannotated sequence literal is a `list`: `let xs = [1, 2, 3]` is a `list[int32]`. An `array` is spelled by asking for one (`let a: array[int32, 3] = [1, 2, 3]`).

## Ownership and `drop`

`list[T]` owns its storage and its elements. Its `drop` drops each element in index order, then frees the storage. Calling `free()` explicitly is allowed; it drops the elements the same way, and the later `drop` has nothing left to do.

Because the list owns its elements, nothing else may own one while it is stored:

- `v[i]` by value copies the element, so it requires `T: copy`. For any other `T`, borrow it (`&v[i]`, `&mut v[i]`, or a method call on `v[i]`) or take it out with `pop`.
- `first`, `last` and `get` return copies, so they are available only when `T` is `copy`. `cell` and `mutable_cell` borrow for any `T`.
- `v[i] = x`, `set`, and `clear` drop the elements they overwrite or remove.
- `for x in v` consumes `v`: each element moves into `x`, and the elements a loop leaves unvisited (through `break` or `return`) drop with the iterator.

**Not yet implemented:** today elements are never dropped, and the copy-only methods are available for every `T`, returning bitwise copies. That is sound only while no element type needs a drop. See [`todo.md`](../../../todo.md) item 6.

## Operations

### Inherent methods

| Method | Notes |
|---|---|
| `new() -> list[T]` | empty; allocates nothing |
| `len(self) -> usize`, `capacity(self) -> usize`, `is_empty(self) -> bool` | |
| `first(self) -> option[T]`, `last(self) -> option[T]` | `@none` when empty; `T: copy` |
| `get(self, i) -> option[T]` | `@none` when out of range; `T: copy` |
| `set(mut self, i, value) -> bool` | `false` when out of range; drops the old element |
| `push(mut self, value: T)` | amortized O(1) |
| `pop(mut self) -> option[T]` | `@none` when empty |
| `reserve(mut self, n)` | ensures room for `n` elements; never shrinks |
| `clear(mut self)` | keeps capacity; drops the elements (O(1) when `T` needs no drop) |
| `free(mut self)` | drops the elements and releases the storage; the list is empty and reusable afterwards |
| `cell(self, i) -> cell[T]` | view of one element; panics when out of range |
| `mutable_cell(mut self, i) -> option[cell_mut[T]]` | writable view; `@none` when out of range |
| `as_slice(self) -> slice[T]`, `as_mut_slice(mut self) -> slice_mut[T]` | view of every element |
| `as_ptr(self) -> *T`, `as_mut_ptr(mut self) -> *mut T` | `machine` access; bounds are the caller's responsibility |

### Trait impls

| Trait | Gives | Notes |
|---|---|---|
| `std.traits.from_array[T]` | `let v: list[int32] = [1, 2, 3]` | allocates exactly the literal's length |
| `std.traits.index[usize]` | `v[i]` | `T: copy`; bounds-checked; out of range panics with `index out of bounds` |
| `std.traits.index_set[usize]` | `v[i] = x` | same check; drops the old element |
| `std.traits.index_ref[usize]` | `&v[i]` → `cell[T]` | same check |
| `std.traits.index_mut[usize]` | `&mut v[i]` → `cell_mut[T]` | same check |
| `std.traits.index[range[usize]]` | `v[a..b]` → `slice[T]` | no copy; `b == len` is allowed, `a > b` panics |
| `std.traits.index_mut[range[usize]]` | `&mut v[a..b]` → `slice_mut[T]` | same check |
| `std.iter.into_iterator[T]` | `for x in v` | **consumes `v`** |
| `drop` | scope-exit release | see above |

`for x in v` moves the list. To keep it, iterate a borrow: `for x in &v` yields `&T` and `for x in &mut v` yields `&mut T`, through `std.iter`'s `iter`/`iter_mut` (see [The Iterator Protocol](../algorithms/48-iterator-protocol.md)).

### Not provided

- **`insert`, `remove`, `truncate`, `extend`.** Not implemented yet.
- **`contains`, `any`, `all`, `find`, `filter`.** These are iterator operations (`xs.iter().any(...)`), not `list` methods, so they compose and stay lazy. An eager `list.filter` would shadow `std.algo`'s lazy `filter` under UFCS with a different meaning.
- **`sort`, `binary_search`.** These operate on a range, not a whole list: `sort(&mut xs[0..xs.len()])`. See [Sorting and Searching](../algorithms/51-sorting-and-searching.md).

## `std.mem` — typed allocation

`std.mem` (`src/std/mem.cn`) wraps the `rt_alloc`/`rt_realloc`/`rt_free` intrinsics (`src/runtime/allocator.h`) in typed, *element-counted* form. A caller that knows it holds `T`s never multiplies by `size_of[T]()` itself, because that multiplication is where buffer overflows come from.

```cinder
machine def alloc[T](count: usize) -> *mut T
machine def resize[T](p: *mut T, old_count: usize, new_count: usize) -> *mut T
machine def free[T](p: *mut T, count: usize) -> unit
machine def copy[T](dst: *mut T, src: *T, count: usize) -> unit
```

Every block is zero-filled, including the grown tail of a `resize`. The allocator is selected at run time by `CINDER_ALLOCATOR`: `system` (the default; `calloc`/`realloc`/`free`) or `arena` (a bump arena where `free` is a no-op). Code on `std.mem` must be correct under both, so it must not depend on a freed block being reused.

## Example

```cinder
use std.algo.sort

def main() -> unit:
    var xs: list[int32] = [3, 1, 2]
    xs.push(5)
    xs[0] = 4                          # [4, 1, 2, 5]

    var total: int32 = 0
    for x in &xs:                      # borrows; xs is still usable
        total = total + *x             # 12

    match xs.pop():
        @some(v) => println("popped {v}")   # popped 5
        @none => println("empty")

    let middle = xs[1..3]              # slice[int32] over [1, 2]
    sort(&mut xs[0..xs.len()])         # [1, 2, 4]
```

## See also

- [The `machine` Layer](../../03-advanced/38-machine-layer.md) — the raw-pointer and allocation substrate `std.mem` and `list[T]` are built on.
- [The Iterator Protocol](../algorithms/48-iterator-protocol.md) — `iter`, `iter_mut`, `into_iter` over `list[T]`.
- [Lazy Adapters](../algorithms/49-lazy-adapters.md) and [Aggregation](../algorithms/50-aggregation.md) — where `filter`, `any`, `find`, and friends live.
- [Sorting and Searching](../algorithms/51-sorting-and-searching.md) — `sort`, `binary_search`, and the other slice algorithms.
