# 43. `list[T]`

**Status:** Implemented

Covers `list[T]`'s representation, growth strategy, and the operations available on it today, split across a compiler builtin core and the ordinary-Kira extensions in `std.list`.

## Representation

`list[T]` is a compiler builtin generic (`type_kind::builtin_generic_kind`), not (yet) an ordinary Kira struct — `list` still appears in the compiler's `k_builtin_generic_arities` table rather than being defined as `pub type list[T] = { ... }` over the `machine` primitive substrate. The runtime layout, shared by both backends, is a 3-slot heap header:

```
{ u64 len; u64 cap; T* data; }
```

(`src/runtime/layout.h`.)

- **Growth.** `push` reserves a slot via `list_reserve_slot`, which grows `data` when `len == cap`: starting capacity 4, doubling thereafter. Growing allocates a fresh, larger block from the shared bump arena and copies the existing `len * elem_size` bytes across; there is no in-place realloc.
- **Element width.** The reserved slot's address is computed from `layout_of(T).size_bytes` (1/2/4/8 bytes), so `list[bool]` and `list[int16]` do not pay 8 bytes per element the way the header's own slots do.
- The denaturalization to `pub type list[T] = { data: *mut T, len: usize, cap: usize }` over the `machine` substrate (`spec/collections-algorithms-design.md` §6.2) is **half done**: the destination type exists and works — see `vector[T]` below — but `list[T]` itself has not moved to it. What still stands in the way is the *sugar*, not the data structure: array-literal lowering through a `list_from_array` constructor, `for` routed through `into_iterator`, and an `index` trait with an operator hook for `v[i]` (indexing is still a builtin operator, not a user-overridable trait method). **Those three language changes have since landed** — indexing is the `std.traits.index`/`index_set` traits, `for` routes through `std.iter.into_iterator`, and literals construct through `std.traits.from_array` — and `vector[T]` implements all four, so it is now usable as `let v: vector[int32] = [1, 2, 3]`, `v[0]`, `v[1] = x`, `for x in v`. What still blocks `list[T]` itself moving is scope-exit `drop` (`../../../todo.md` item 6) and a *borrowing* iteration route, since `into_iter(self)` consumes the collection (item 20). See [list-migration-design.md](../../../list-migration-design.md).

## Operations

### Builtin inherent methods

Only two names have real lowering as builtin methods, from the compiler's `k_builtin_methods` table:

- `len(self) -> usize`
- `push(mut self, x: T) -> unit` — amortized O(1).

Indexing (`xs[i]`, `&xs[i]`, `&mut xs[i]`) is a builtin operator on `list[T]`, O(1), and is not expressed through any trait yet.

### `std.list` extensions

`src/std/list.kira` adds, as an ordinary `extend[T] list[T]` block over `len` and indexing:

- `is_empty(self) -> bool`
- `first(self) -> option[T]` — `@some` of the first element, `@none` if empty.
- `last(self) -> option[T]` — `@some` of the last element, `@none` if empty.

### Deliberately absent

- **`pop`, `clear`, `insert`, `remove`.** These need to *shrink* a list, which no primitive currently exposes; they return once `list` is rebuilt over `alloc`/pointer primitives.
- **`contains`, `any`, `all`, `find`, `filter`.** These were builtin methods historically but are not reimplemented as `list` methods: they are iterator operations (`xs.iter().any(...)`), so they compose and stay lazy. An eager `list.filter` would have shadowed `std.algo`'s lazy `filter` with a different meaning for the same name under UFCS.
- **`sort`.** Belongs with the slice algorithms, over a range rather than a whole list — see [Sorting and Searching](../algorithms/51-sorting-and-searching.md) (planned).

Every builtin-method entry with no working lowering was removed from the table outright: an entry that type-checks and then fails inside the compiler is worse than an honest "no method" diagnostic with a suggestion.

## `std.mem` — typed allocation

`std.mem` (`src/std/mem.kira`) wraps the raw `rt_alloc`/`rt_realloc`/`rt_free` intrinsics (`src/runtime/allocator.h`) in typed, *element-counted* form. Every function is `machine` and a few lines long; a caller that already knows it holds `T`s never multiplies by `size_of[T]()` itself, because that multiplication is where a buffer overflow comes from.

```kira
machine def alloc[T](count: usize) -> *mut T
machine def resize[T](p: *mut T, old_count: usize, new_count: usize) -> *mut T
machine def free[T](p: *mut T, count: usize) -> unit
machine def copy[T](dst: *mut T, src: *T, count: usize) -> unit
```

Every block is zero-filled, including the grown tail of a `resize`. The allocator behind these is selectable at run time — `KIRA_ALLOCATOR=system` (the default: `calloc`/`realloc`/`free`, memory genuinely reclaimed) or `KIRA_ALLOCATOR=arena` (the historical bump arena, where `free` is a no-op). Code written on `std.mem` must be correct under both, which in practice means it must not depend on a freed block being reused.

## `vector[T]` — a list owning its own storage

`vector[T]` (`src/std/list.kira`) is the same data structure as `list[T]`, written in Kira with no compiler support beyond what any user struct gets:

```kira
pub type vector[T] = { len: usize, cap: usize, data: *mut T }
```

It exists because `list[T]`'s missing operations were never a library omission — `pop`, `clear` and `reserve` all need to *shrink* or *re-home* storage, and until `rt_realloc`/`rt_free` existed no primitive could. `vector[T]` has all of them:

| Method | |
|---|---|
| `new() -> vector[T]` | empty, allocates nothing |
| `len`, `capacity`, `is_empty` | |
| `push(mut self, value: T)` | amortized O(1); capacity 4 then doubling |
| `pop(mut self) -> option[T]` | |
| `get(self, i) -> option[T]`, `set(mut self, i, value) -> bool` | bounds-checked, no panic |
| `reserve(mut self, n)` | never shrinks |
| `clear(mut self)` | O(1); keeps capacity |
| `free(mut self)` | releases the storage |

Every method that touches memory is `machine` and short; the public API is entirely safe.

**`free` must be called explicitly.** Kira runs no scope-exit `drop` glue on either backend (`../../../todo.md` item 6), so a `vector` that goes out of scope leaks its buffer under `KIRA_ALLOCATOR=system`, exactly as every heap value already leaks under the arena. Elements are not dropped either, for the same reason. This is the one place `vector[T]` is worse than `list[T]` today — a bump-arena `list` never promised to free anything, so it had nothing to forget to do.

## Example

```kira
var xs: list[int32] = []
xs.push(1)
xs.push(2)
xs.push(3)
xs.is_empty()    # false
xs.len()         # 3
xs[0]            # 1
xs.last()        # @some(3)
```

```kira
use std.list.vector

var v = vector[int32].new()
v.push(1)
v.push(2)
v.len()          # 2
v.pop()          # @some(2)
v.reserve(64)
v.clear()
v.free()
```

## See also

- [The `machine` Layer](../../03-advanced/38-machine-layer.md) — the raw-pointer and allocation substrate `std.mem` and `vector[T]` are built on.
- [The Iterator Protocol](../algorithms/48-iterator-protocol.md) — `iter`, `iter_mut`, `into_iter` over `list[T]`.
- [Lazy Adapters](../algorithms/49-lazy-adapters.md) and [Aggregation](../algorithms/50-aggregation.md) — where `filter`, `contains`-equivalents (`any`/`find`), and friends now live.
- [Sorting and Searching](../algorithms/51-sorting-and-searching.md) — planned home of `sort` and the other slice algorithms.


## `vector[T]` as an ordinary collection

`vector[T]` implements the four traits that used to be `list[T]`'s exclusive privileges, so it is usable with the same syntax:

```kira
use std.list.vector

var v: vector[int32] = [10, 20, 30]   # from_array[T]
let first = v[0]                      # index[usize]
v[1] = 99                             # index_set[usize]
for x in v:                           # into_iterator[T]
    println("{x}")
```

| Trait | Gives | Notes |
|---|---|---|
| `std.traits.index[usize]` | `v[i]` | bounds-checked; panics out of range |
| `std.traits.index_set[usize]` | `v[i] = x` | same check |
| `std.iter.into_iterator[T]` | `for x in v` | **consumes `v`** — see below |
| `std.traits.from_array[T]` | `let v: vector[int32] = [...]` | allocates exactly the literal's length |

**Iterating consumes the vector.** `into_iter(self)` takes the collection by value, so `for x in v` moves it and the move checker refuses every later use, `v.free()` included. That is correct for `into_iterator` and is not what a collection wants; a borrowing route (`for x in &v`) is [`todo.md`](../../../todo.md) item 20, and a prerequisite for `list[T]` itself moving onto this storage.

`&mut v[i]` is not available: `std.traits.index_mut` is declared but not wired (item 19).
