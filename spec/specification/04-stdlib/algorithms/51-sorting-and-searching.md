# 51. Sorting and Searching

**Status:** Planned

Covers the designed-but-unimplemented slice algorithms: `sort`, `sort_by`, `sort_by_key`, `sorted`, `stable_sort`, `binary_search`, `binary_search_by`, `reverse`, `rotate`, `dedup`, `partition_point`, `lower_bound`, `upper_bound`, `windows`, `chunks`.

None of this exists in `src/std/algo.kira` today — confirmed by grep: no `sort`, `binary_search`, `reverse`, or `windows` definitions appear there. `src/std/list.kira`'s header comment is explicit about the omission: *"`sort` is likewise absent: it belongs with the slice algorithms, over a range rather than a whole list."* This chapter describes the target design fixed in `spec/collections-algorithms-design.md` §6.1, normatively.

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

## Implementation status

Nothing in this chapter is implemented. Blocking work, per `spec/collections-algorithms-design.md` §8:

- These operate over `slice[T]`/`slice_mut[T]`, and in-place mutation through a slice presupposes the same reference/pointer model gap (R2) that blocks the `machine`-based containers: reading through `&mut T`/`*T` fails on both the bytecode VM and LLVM backend today.
- `T: ord` bounds, needed for `sort`/`binary_search`/`lower_bound`/`upper_bound`, are currently advisory only — unenforced at the call site (R1) — so a misuse would surface as an error inside the standard library's own source rather than at the caller's line, absent the instantiation-site diagnostic note that mitigation requires.

## See also

- [`list[T]`](../collections/43-list.md) — `list.kira`'s explicit note that `sort` was deferred to this chapter.
- [`deque[T]` and `bitset`](../collections/44-deque-and-bitset.md) — another chapter blocked on the same pointer-model gap.
