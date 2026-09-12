# 51. Sorting and Searching

**Status:** Implemented

Covers the slice algorithms `sort`, `sort_by`, `sort_by_key`, `sorted`, `stable_sort`, `binary_search`, `binary_search_by`, `reverse`, `rotate`, `dedup`, `partition_point`, `lower_bound`, `upper_bound`, `windows`, `chunks` — all implemented in `src/std/algo.kira` as free functions over `slice[T]`/`slice_mut[T]`, reachable by UFCS (`s.sort()`).

## Design

These are **slice algorithms, not iterator adapters** — in-place (where applicable) and operating over a contiguous range rather than draining a lazily-pulled `iterator[T]`:

- `sort` (`T: ord`), `sort_by`, `sort_by_key` — in-place sort of a mutable slice.
- `sorted` — the non-mutating counterpart, producing a new sorted collection.
- `stable_sort` — sort preserving the relative order of equal elements.
- `binary_search`, `binary_search_by` — O(log n) lookup over an already-sorted slice.
- `reverse` — in-place reversal.
- `rotate` — in-place rotation.
- `dedup` — in-place removal of consecutive duplicate elements.
- `partition_point`, `lower_bound`, `upper_bound` — binary-search-based boundary queries over a sorted slice.

This is also where `sort` acquires a comparator and a connection to `ord` at all; today `sort` has neither (indeed, `sort` does not exist yet as anything callable).

`windows` and `chunks` live here too, as **slice** operations rather than iterator adapters: a general iterator cannot produce overlapping views without buffering, and the buffering version has a materially different cost profile. Putting them on slices keeps the complexity honest — `windows` and `chunks` yield views into existing storage rather than allocating a copy per window/chunk.

## Design

- `sort` (`T: ord`), `sort_by`, `sort_by_key` — in-place, not guaranteed stable. `sort` uses `ska_sort` (a byte-radix LSD integer sort — see "Ska sort" below) when `T` is one of the builtin integer types small enough for a 64-bit key to represent losslessly (every integer type except `int128`/`uint128`), and introsort (quicksort with a heapsort worst-case fallback, insertion sort below a small-range cutoff) otherwise. `sort_by`/`sort_by_key` always use introsort, since a caller-supplied comparator isn't known to correspond to integer ordering.
- `sorted` — the non-mutating counterpart: copies into a `list[T]`, sorts the copy, and returns it.
- `stable_sort` — a simplified timsort: natural runs (ascending, or descending and reversed) are detected and extended to a fixed minimum length via insertion sort, then merged bottom-up in pairs until one run remains. Real timsort's exact stack-balancing merge order and galloping mode are not implemented — both are performance refinements over an already-`O(n log n)` merge, not correctness requirements.
- `binary_search`, `binary_search_by` — `O(log n)` lookup over an already-sorted slice, built on `lower_bound`/a direct binary search respectively.
- `reverse`, `rotate` — in-place, via a small number of `[lo, hi)` sub-range reversals.
- `dedup` — in-place removal of *consecutive* duplicates; returns the new length, since a slice cannot shrink itself.
- `partition_point`, `lower_bound`, `upper_bound` — binary-search-based boundary queries over a sorted (or predicate-partitioned) slice.
- `windows`, `chunks` — return `list[slice[T]]`: views into the existing storage, not copies.

### Ska sort

`sort[T]`'s integer fast path is named for Malte Skarupke's byte-radix integer sort. It is realized here as a classic double-buffered LSD (least-significant-digit) radix pass over an order-preserving 64-bit key — `x as usize`, sign-flipped at bit 63 for a signed source — rather than his fully in-place American-flag variant, which needs pointer-level bucket bookkeeping this module has no primitive for yet. Still `O(n)`, and correct for every integer width up to 64 bits; `int128`/`uint128` are excluded (truncating to a 64-bit key could make two distinct values collide) and fall back to introsort.

Selecting `ska_sort` for exactly the integer types requires knowing, from inside a single generic `sort[T]`, which concrete type `T` was monomorphized with — `T.name()` compile-time reflection, called from ordinary (non-`static`) code. This did not work before this chapter was implemented: see [Compile-Time Execution](../../03-advanced/31-compile-time-execution.md#implementation-status).

## Implementation status

Implemented in `src/std/algo.kira`, both backends (bytecode VM and LLVM AOT), verified via `src/testdata/std_test/algo_sort.kira`. This chapter was blocked on two real compiler gaps, both found and fixed in the course of implementing it:

- Range-indexing (`arr[a..b]`, which *forms* the `slice`/`slice_mut` view every algorithm here operates through) only scaled its resulting view's data pointer correctly for 1-byte-stride sources — silently wrong for `array[int32, N]`/`list[int32]`/etc. Fixed; see `general-stride-range-indexing-implemented` project memory.
- `T.name()` reflection, needed for `sort`'s integer/non-integer dispatch, type-checked but never lowered outside `static` constructs. Fixed; see [Compile-Time Execution § Implementation status](../../03-advanced/31-compile-time-execution.md#implementation-status).

`T: ord` bounds, needed for `sort`/`binary_search`/`lower_bound`/`upper_bound`, remain advisory only — unenforced at the call site (a pre-existing, unrelated gap) — so a misuse would surface as an error inside the standard library's own source rather than at the caller's line.

## See also

- [`list[T]`](../collections/43-list.md) — `list.kira`'s note on how `xs.sort()` reaches this chapter's `sort` via UFCS.
- [`deque[T]` and `bitset`](../collections/44-deque-and-bitset.md) — still blocked on `machine`-based containers' own, separate pointer-model gap.
