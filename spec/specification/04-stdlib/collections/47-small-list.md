# 47. `small_list[T, N]`

**Status:** Planned

Covers `small_list[T, N]`, a small-size-optimized list built on `uninit[T, N]`, and the `uninit[T, N]` primitive it is designed over.

## `uninit[T, N]`

`uninit[T, N]` is an opaque builtin type: `N` slots, sized and aligned for `T`, that carries no guarantee any slot holds a valid `T`. It is specced in `spec/kira-reference.md` ("The `machine` Layer") and reiterated unchanged by `spec/collections-algorithms-design.md` §4.3, which additionally fixes that it is **not** made redundant by a general allocator — it is inline, fixed-capacity storage, the thing that lets a small-size-optimized container avoid heap traffic entirely below its inline capacity.

Ordinary code cannot read or write into a `uninit[T, N]` directly. A fixed set of `machine`-prefixed accessor functions can:

```kira
pub type uninit[T, N: usize]     # opaque; N slots, sized/aligned for T

machine def slot_ptr[T, N: usize](buf: &uninit[T, N], i: usize) -> *mut T
machine def write_slot[T, N: usize](buf: &mut uninit[T, N], i: usize, value: T) -> unit
machine def read_slot[T, N: usize](buf: &mut uninit[T, N], i: usize) -> T
    # moves the value out of slot i — the caller must not treat it as initialized afterward
machine def drop_first[T, N: usize](buf: &mut uninit[T, N], len: usize) -> unit
    # runs T's drop over slots 0..len only; slots len..N are assumed never initialized
machine def as_slice[T, N: usize](buf: &uninit[T, N], len: usize) -> slice[T]
    # claims slots 0..len are initialized and hands back an ordinary view over them
```

`spec/collections-algorithms-design.md` §4.3 additionally specifies these as *derivable* from the `std.machine` primitive substrate rather than intrinsics in their own right: `drop_first` is a loop over the substrate's `drop_in_place`, and the rest reduce similarly. Only the opaque `uninit[T, N]` type itself is compiler-intrinsic work; its accessors are ordinary (if `machine`-gated) Kira.

Ordinary, non-`machine` code is free to *call* these functions; the unsafety is contained entirely inside them. A type built on `uninit[T, N]` calls them once, internally, and exposes a fully safe public API to everyone else — `small_list[T, N]` is the motivating example both documents name for this pattern.

## `small_list[T, N]`

`small_list[T, N]` is referenced, by name, as `uninit[T, N]`'s motivating use case in both `spec/kira-reference.md` and `spec/collections-algorithms-design.md` §4.3: a type that holds a *variable* number of `T` values, up to `N`, inline — no heap allocation for the common small case, falling back to heap storage past `N` elements.

Neither source goes further than that one-sentence motivation. No field layout, growth-to-heap transition strategy, or method catalog for `small_list[T, N]` has been written down anywhere in the repository as of this writing.

## Implementation status

Nothing in this chapter is implemented. `uninit[T, N]` itself has no entry in the compiler's builtin generic table today, and there is no `small_list.kira` or equivalent under `src/std`. Beyond the representation and accessor signatures reproduced above (which are real, specced content), this chapter's coverage of `small_list[T, N]` proper is limited to what the two source documents state: that it exists as a design goal and that it is what `uninit[T, N]` is *for*. Its own structure and API remain open design work.

## See also

- [`list[T]`](43-list.md) — the heap-only container `small_list[T, N]` complements.
- [Sorting and Searching](../algorithms/51-sorting-and-searching.md) — another chapter dependent on the same `machine`/pointer substrate.
