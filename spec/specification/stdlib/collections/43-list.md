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
- A future denaturalization to `pub type list[T] = { data: *mut T, len: usize, cap: usize }` over `std.machine` is designed (`spec/collections-algorithms-design.md` §6.2) but not started; it additionally requires array-literal lowering through a `list_from_array` constructor, `for` routed through `into_iterator`, and an `index` trait with an operator hook for `v[i]` (none of these exist today — indexing is a builtin operator, not a user-overridable trait method).

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

## See also

- [The Iterator Protocol](../algorithms/48-iterator-protocol.md) — `iter`, `iter_mut`, `into_iter` over `list[T]`.
- [Lazy Adapters](../algorithms/49-lazy-adapters.md) and [Aggregation](../algorithms/50-aggregation.md) — where `filter`, `contains`-equivalents (`any`/`find`), and friends now live.
- [Sorting and Searching](../algorithms/51-sorting-and-searching.md) — planned home of `sort` and the other slice algorithms.
