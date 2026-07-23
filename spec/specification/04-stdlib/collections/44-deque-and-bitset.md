# 44. `deque[T]` and `bitset`

**Status:** Planned

Covers the designed-but-unimplemented `deque[T]` (double-ended ring buffer) and `bitset` (word-packed bit set) containers.

Neither type exists in `src/std` today; there is no `deque.kira`, `bitset.kira`, or equivalent. This chapter describes the design fixed in `spec/collections-algorithms-design.md` §6.3, normatively, as the target shape.

## `deque[T]`

A ring buffer over a power-of-two-capacity allocation:

```
{ data: *mut T, head: usize, len: usize, cap: usize }
```

- Indexing into logical position `i` masks rather than takes a modulo: `(head + i) & (cap - 1)`, which is why `cap` is constrained to a power of two.
- `push_front`, `push_back`, `pop_front`, `pop_back` — amortized O(1).
- Indexing — O(1).
- Not contiguous in general (the live range can wrap around the end of `data`), so it does not offer `as_slice`. A `make_contiguous` operation rotates the storage so the live range is contiguous, in place, and is the escape hatch for algorithms that need a slice.

## `bitset`

```
{ words: list[uint64], nbits: usize }
```

- `set`, `clear`, `flip`, `test` — per-bit operations.
- `count_ones` — population count, implemented as SWAR (bit-twiddling over the packed words) rather than an intrinsic; no popcount intrinsic exists in the primitive substrate (see the `machine` layer's fourteen intrinsics — no SIMD, no popcount, on the grounds that both are expressible or unnecessary in portable Kira).
- `union`, `intersect`, `difference` — set algebra over the packed words.
- `iter()` — yields the indices of set bits, produced by trailing-zero scanning of each word in turn.

The set operations depend on bit-operator traits (`bit_and`, `bit_or`, `bit_xor`, `shl`, `shr`) that do not exist yet in `std.traits`. Adding them follows the existing `add`/`type output` operator-trait pattern and is mechanical, but it is a prerequisite this chapter's operations have not cleared.

## Implementation status

Nothing in this chapter is implemented. Confirmed by absence: no file under `src/std` matches `*deque*` or `*bitset*`. Blocking work:

- The bit-operator traits (`bit_and`/`bit_or`/`bit_xor`/`shl`/`shr`) `bitset` needs do not exist in `std.traits`.
- Both containers are specified over raw-pointer fields (`*mut T`), which in turn depend on the `machine`-gated pointer/intrinsic substrate (`std.machine`) and, on the bytecode VM specifically, a reference/pointer model that does not exist yet (R2 in `spec/collections-algorithms-design.md` §8) — reading through `&mut T`/`*T` fails on both backends today.

## See also

- [`list[T]`](43-list.md) — `bitset`'s backing storage.
- [Sorting and Searching](../algorithms/51-sorting-and-searching.md) — another planned chapter blocked on the same slice/pointer substrate.
