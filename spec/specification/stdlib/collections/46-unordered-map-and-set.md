# 46. `unordered_map[K, V]` and `unordered_set[K]`

**Status:** Planned

Covers the designed-but-unimplemented `unordered_map[K, V]` (and its named companion `unordered_set[K]`), a hash-based associative container.

Neither type exists in `src/std` today. This chapter describes the design fixed in `spec/collections-algorithms-design.md` §6.5, normatively, as the target shape.

## Design

Open addressing, linear probing, Robin Hood hashing with backward-shift deletion:

```kira
pub type unordered_map[K, V] = {
    ctrl: *mut uint8,   # 0 = empty, otherwise 1 + probe distance
    keys: *mut K,
    vals: *mut V,
    len: usize,
    cap: usize,         # power of two
}
```

- **Backward-shift deletion**, not tombstones. Tombstone degradation under repeated insert/remove is the failure mode users actually hit; backward-shift avoids it at the cost of a shifting pass on removal.
- **Power-of-two capacity** with mask indexing (no modulo). Growth is triggered at 7/8 load factor.
- **Complexity** — average O(1) per operation, worst case O(n).
- **Hashing.** Comes from `K: hash` (returning `uint64`), but a fibonacci-multiply finalizer is applied to the hash before masking it down to a bucket index: user `hash` implementations will generally be low-quality, and linear probing is unforgiving of clustered hashes.
- **No SIMD group-scan.** A Swiss-table-style vectorized control-byte scan would require SIMD intrinsics that do not exist in the primitive substrate, and would be the one part of the standard library not writable in portable Kira — deliberately not pursued.

`unordered_set[K]` is named alongside `unordered_map[K, V]` in the design catalog but has no representation or API of its own written down beyond that; expect it to follow once `unordered_map` lands.

## Implementation status

Nothing in this chapter is implemented. Confirmed by absence: no file under `src/std` matches `*unordered*`. This container is specified entirely over raw-pointer fields (`*mut uint8`, `*mut K`, `*mut V`), so — like [`deque[T]` and `bitset`](44-deque-and-bitset.md) — it is blocked on the `machine`-gated pointer substrate (`std.machine`) and, on the bytecode VM, a reference/pointer model that does not exist yet.

## See also

- [Ordered Map and Set](45-ordered-map-and-set.md) — the comparison-based alternative.
- [`list[T]`](43-list.md) — the container this design contrasts with for representation strategy.
