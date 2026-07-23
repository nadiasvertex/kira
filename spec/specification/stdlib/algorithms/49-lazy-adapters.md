# 49. Lazy Adapters

**Status:** Implemented

Covers every lazy iterator adapter in `src/std/algo.kira`: a free generic function returning a generic struct implementing `iterator[T]` (see [The Iterator Protocol](48-iterator-protocol.md)). Nothing is evaluated until the resulting chain is drained by a terminal (see [Aggregation](50-aggregation.md)); building an adapter costs no allocation and does no work.

Each entry gives the constructor's signature and its exact semantics. `it` is always taken by value (moved) and drives the adapter's `next`.

- **`map[I, T, U](it: I, f: fn(T) -> U) -> map_iter[I, T, U] where I: iterator[T]`** — applies `f` to each element. One-to-one, so `size_hint` passes through unchanged from the source.
- **`filter[I, T](it: I, pred: fn(T) -> bool) -> filter_iter[I, T] where I: iterator[T]`** — yields only elements satisfying `pred`. `size_hint` is not refined from the source (stays at the default `0`): `filter` may drop every element, so `0` is the only honest lower bound.
- **`filter_map[I, T, U](it: I, f: fn(T) -> option[U]) -> filter_map_iter[I, T, U] where I: iterator[T]`** — applies `f` and keeps the `@some` results. `map` and `filter` in one pass; the spelling to reach for when the test for keeping an element is the same work as producing it.
- **`take[I, T](it: I, n: usize) -> take_iter[I, T] where I: iterator[T]`** — yields at most the first `n` elements. `size_hint` is the lesser of `n` and the source's.
- **`skip[I, T](it: I, n: usize) -> skip_iter[I, T] where I: iterator[T]`** — discards the first `n` elements, lazily (the skip happens on the first `next` call, not in the constructor). `size_hint` is `max(0, source_hint - n)`.
- **`take_while[I, T](it: I, pred: fn(T) -> bool) -> take_while_iter[I, T] where I: iterator[T]`** — yields elements until `pred` first fails, then stops for good (the stopping condition latches; a later element satisfying `pred` again is not resumed).
- **`skip_while[I, T](it: I, pred: fn(T) -> bool) -> skip_while_iter[I, T] where I: iterator[T]`** — discards a leading run of elements satisfying `pred`, then yields everything after, including later elements that would satisfy `pred` again (only a *prefix* is dropped).
- **`step_by[I, T](it: I, n: usize) -> step_by_iter[I, T] where I: iterator[T]`** — yields every `n`th element, starting with the first (elements `0, n, 2n, ...` of the source).
- **`chain[I, J, T](it: I, rest: J) -> chain_iter[I, J, T] where I: iterator[T], J: iterator[T]`** — yields all of `it`'s elements, then all of `rest`'s. Concatenates two streams; does not interleave them. `size_hint` sums both sources.
- **`rev[I, T](it: I) -> rev_iter[I, T] where I: double_ended[T]`** — yields the source's elements back to front, by calling `next_back` on the source. Requires `double_ended[T]`, which is what keeps this lazy rather than buffering.
- **`peekable[I, T](it: I) -> peekable_iter[I, T] where I: iterator[T]`** — adapts an iterator so its next element can be examined without consuming it. Paired with a free function, not a trait method:
  - **`peek[I, T](it: &mut peekable_iter[I, T]) -> option[T] where I: iterator[T]`** — returns the next element without consuming it; repeated calls return the same element, and that element is what the following `next` yields.
- **`scan[I, T, S](it: I, init: S, f: fn(S, T) -> S) -> scan_iter[I, T, S] where I: iterator[T]`** — yields a running accumulation: `fold` with its intermediate results kept. The initial state is not itself yielded; the first element out is the state after folding in the first element.
- **`inspect[I, T](it: I, f: fn(T) -> unit) -> inspect_iter[I, T] where I: iterator[T]`** — runs `f` on each element as it passes through, yielding the element unchanged. For observing a pipeline (logging, counting) without altering it; because the chain is lazy, `f` runs only for elements actually drawn.
- **`zip[I, J, T, U](it: I, other: J) -> zip_iter[I, J, T, U] where I: iterator[T], J: iterator[U]`** — pairs elements of two iterators into `(T, U)` tuples. Stops as soon as either source does, so the result is as long as the shorter one. `size_hint` is the lesser of the two.
- **`enumerate[I, T](it: I) -> enumerate_iter[I, T] where I: iterator[T]`** — pairs each element with its zero-based position, counting from the point `enumerate` is applied (placing it after a `filter` numbers the *kept* elements consecutively).
- **`flatten[I, J, T](it: I) -> flatten_iter[I, J, T] where I: iterator[J], J: iterator[T]`** — yields the elements of each of an iterator-of-iterators' inner iterators, in order, flattening exactly one level. Empty inner iterators contribute nothing and are skipped without yielding. `size_hint` is not refined (stays at the default `0`): an outer count says nothing about the number of elements the inners will actually yield.
- **`flat_map[I, T, J, U](it: I, f: fn(T) -> J) -> flat_map_iter[I, T, J, U] where I: iterator[T], J: iterator[U]`** — `it.map(f).flatten()` in one adapter. `f` maps each element to an iterator; `f` is not called on an element until its predecessor's elements have been fully drained.
- **`values[I, T](it: I) -> map_iter[I, &T, T] where I: iterator[&T]`** — dereferences a `&T`-yielding iterator into a `T`-yielding one (`it.map(x => *x)`, packaged so a reference-iteration chain does not need to spell it out). Typically opens a chain: `xs.iter().values().sum()`.

## Known gap: `unique`

`spec/collections-algorithms-design.md` §6.1 lists `unique` (bound `T: hash + eq`) as a lazy adapter alongside the ones above. It is **not implemented** — there is no `unique`/`unique_iter` in `src/std/algo.kira`. Treat it as planned; the `hash` trait it would depend on is itself part of the not-yet-built [unordered map/set](../collections/46-unordered-map-and-set.md) work.

## See also

- [The Iterator Protocol](48-iterator-protocol.md) — the `iterator[T]` trait every adapter here implements.
- [Aggregation](50-aggregation.md) — the terminals that drain an adapter chain.
