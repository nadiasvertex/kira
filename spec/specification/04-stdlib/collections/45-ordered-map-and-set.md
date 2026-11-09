# 45. `ordered_map[K, V]` and `ordered_set[K]`

**Status:** Planned

Covers the designed-but-unimplemented `ordered_map[K, V]` (and its named companion `ordered_set[K]`), a key-ordered associative container.

Neither type exists in `src/std` today. This chapter describes the two-phase target design normatively.

## Design: sorted vector, then B-tree

**Phase one** — sorted vector:

```
{ keys: list[K], vals: list[V] }
```

- Lookup — O(log n), by binary search over `keys`.
- Insert/remove — O(n), by `memmove`-style shifting of both parallel lists.

Roughly 150 lines of Cinder; it exercises `K: ord` end to end, and below about a thousand elements it outperforms a B-tree regardless. It ships the complete API immediately, before phase two lands.

**Phase two** replaces the internals behind an unchanged public API with a **B-tree**, order B = 6 (five to eleven keys per node). Chosen over a red-black tree because a B-tree node's payload is `array[K, 11]` plus a length — which the existing const-generic support already handles — whereas a red-black tree needs option-of-node-pointer chasing and colour invariants that are considerably harder to get right without a borrow checker.

## API

Bound `K: ord` throughout.

- `insert`, `get`, `get_mut`, `remove`, `contains`, `len`
- `first`, `last`
- `range(lo, hi)`
- `keys()`, `values()`, `values_mut()`
- The three iterator families (`iter`, `iter_mut`, `into_iter`), yielding `(&K, &V)` / `(&K, &mut V)` / `(K, V)` tuples in key order.

`ordered_set[K]` is named alongside `ordered_map[K, V]` in the design catalog but has no representation or API of its own written down beyond that; expect it to follow once `ordered_map` lands.

## Implementation status

Nothing in this chapter is implemented. Confirmed by absence: no file under `src/std` matches `*ordered*`. `for (k, v) in it:`, needed to consume this container's tuple-yielding iterators directly, works: a `for` head may destructure a tuple pattern.

## See also

- [`list[T]`](43-list.md) — the phase-one backing storage.
- [Unordered Map and Set](46-unordered-map-and-set.md) — the hash-based alternative.
- [The Iterator Protocol](../algorithms/48-iterator-protocol.md) — the three iterator families this container will expose.
