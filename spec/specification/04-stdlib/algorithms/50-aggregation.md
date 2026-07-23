# 50. Aggregation

**Status:** Implemented

Covers every terminal (draining) algorithm in `src/std/algo.kira`: functions that consume an `iterator[T]` and return a final answer rather than another lazy iterator. Each takes its iterator by value and moves it into a local `var`, since `next` needs a mutable receiver and a parameter is not one.

The short-circuiting terminals — `any`, `all`, `find`, `find_map`, `position` — return from inside their loop rather than setting a flag and draining to the end; that is what makes `any` over an infinite iterator able to terminate.

- **`count[I, T](it: I) -> usize where I: iterator[T]`** — the number of elements yielded, draining the iterator.
- **`fold[I, T, A](it: I, init: A, f: fn(A, T) -> A) -> A where I: iterator[T]`** — combines elements left to right; `f` receives the accumulator and an element, returns the next accumulator. The general shape the other folds specialize (`count` is a fold adding one).
- **`reduce[I, T](it: I, f: fn(T, T) -> T) -> option[T] where I: iterator[T]`** — like `fold` but seeded from the iterator's first element rather than a supplied initial value; `@none` if the source is empty, since there is then no answer.
- **`for_each[I, T](it: I, f: fn(T) -> unit) -> unit where I: iterator[T]`** — calls `f` on each element, for effects.
- **`any[I, T](it: I, pred: fn(T) -> bool) -> bool where I: iterator[T]`** — whether any element satisfies `pred`; stops at the first match; `false` on an empty iterator.
- **`all[I, T](it: I, pred: fn(T) -> bool) -> bool where I: iterator[T]`** — whether every element satisfies `pred`; stops at the first failure; `true` on an empty iterator (vacuously — no element fails).
- **`find[I, T](it: I, pred: fn(T) -> bool) -> option[T] where I: iterator[T]`** — the first element satisfying `pred`, or `@none`.
- **`find_map[I, T, U](it: I, f: fn(T) -> option[U]) -> option[U] where I: iterator[T]`** — the first non-`@none` result of applying `f`; `xs.find_map(f)` is `xs.filter_map(f).next()`, avoiding doing the test-and-convert work twice as `find` followed by a conversion would.
- **`position[I, T](it: I, pred: fn(T) -> bool) -> option[usize] where I: iterator[T]`** — the zero-based index of the first element satisfying `pred`. Indices count from the iterator's *current* position, not from whatever it was originally built from.
- **`last[I, T](it: I) -> option[T] where I: iterator[T]`** — the final element, or `@none` if empty. Drains the whole iterator; there is no way to reach the end of a general iterator without visiting everything before it (a `double_ended` source can instead answer via `rev().next()` without draining).
- **`nth[I, T](it: I, n: usize) -> option[T] where I: iterator[T]`** — the element at zero-based offset `n` from the iterator's current position; consumes the skipped elements too, so repeated calls on the same iterator walk forward.
- **`min[I, T](it: I) -> option[T] where I: iterator[T], T: ord`** — the smallest element, or `@none` if empty. Compares with `<`, not through `ord.cmp` (see below). Ties keep the earliest.
- **`max[I, T](it: I) -> option[T] where I: iterator[T], T: ord`** — the largest element, or `@none` if empty. Ties keep the latest — the mirror of `min`.
- **`min_by_key[I, T, K](it: I, key: fn(T) -> K) -> option[T] where I: iterator[T], K: ord`** — the element with the smallest `key(element)`. `key` is called once per element, not once per comparison (the running best's key is kept alongside it), which matters when `key` is expensive or has effects.
- **`max_by_key[I, T, K](it: I, key: fn(T) -> K) -> option[T] where I: iterator[T], K: ord`** — the element with the largest key. Ties keep the latest, matching `max`.
- **`partition[I, T](it: I, pred: fn(T) -> bool) -> (list[T], list[T]) where I: iterator[T]`** — splits elements into those satisfying `pred` and the rest, as `(yes, no)`; both halves keep the source's relative order.
- **`eq_by[I, J, T, U](it: I, other: J, pred: fn(T, U) -> bool) -> bool where I: iterator[T], J: iterator[U]`** — whether two iterators yield equal elements under `pred`, position by position. Length is part of equality — a prefix does not count as equal to the whole. `pred` is not called again once either source has ended.
- **`sum[I, T](it: I) -> T where I: iterator[T], T: zero`** — adds up the elements; an empty iterator sums to `T.zero()`, which is why `sum` returns `T` where `reduce` must return `option[T]`.
- **`product[I, T](it: I) -> T where I: iterator[T], T: one`** — multiplies the elements together; an empty iterator's product is `T.one()`.

## The comparator-taking terminals

These take a three-way comparator `fn(T, T) -> ordering` rather than a `less` predicate, because a predicate cannot express "equal" without being called twice, and `cmp_by` needs that distinction to decide whether to inspect the next pair at all.

- **`min_by[I, T](it: I, cmp: fn(T, T) -> ordering) -> option[T] where I: iterator[T]`** — smallest element under `cmp`; ties keep the earliest.
- **`max_by[I, T](it: I, cmp: fn(T, T) -> ordering) -> option[T] where I: iterator[T]`** — largest element under `cmp`; ties keep the latest, matching `max_by_key`.
- **`cmp_by[I, J, T, U](it: I, other: J, cmp: fn(T, U) -> ordering) -> ordering where I: iterator[T], J: iterator[U]`** — compares two iterators lexicographically under `cmp`, walking both in lockstep and answering at the first unequal pair without drawing further from either. If one runs out first it is the smaller; two iterators that end together having compared equal throughout are equal.

## `collect`

`collect[I, C](it: I) -> C` is also a terminal in the sense that it drains its iterator, but it is defined in `std.iter` rather than `std.algo` because it is coupled to the `from_iter[T]` trait — see [The Iterator Protocol](48-iterator-protocol.md).

## Known gap: `group_by`

`spec/collections-algorithms-design.md` §6.1 lists `group_by` among the terminals. It is **not implemented** — there is no `group_by` in `src/std/algo.kira`. Treat it as planned.

## Notes

- `min`/`max` and the `_by_key` pair compare with `<` rather than through `ord.cmp`, because `T: ord` bounds are currently advisory (unenforced at the call site) and `cmp`'s `ordering` return type had no runtime representation until recently. `min_by`/`max_by`/`cmp_by`, which need a genuine three-way comparator, arrived only once `ordering` became a real sum type (`@less`/`@equal`/`@greater`).
- `sum`/`product` are bounded `T: zero` / `T: one` alone, not the `T: add + zero` / `T: mul + one` design §6.1 specifies — a compound `where` bound cannot currently be cloned for monomorphization, so only the half the body actually calls (`T.zero()`/`T.one()`) is written in the bound.

## See also

- [The Iterator Protocol](48-iterator-protocol.md) — `iterator[T]` and `collect`.
- [Lazy Adapters](49-lazy-adapters.md) — the adapters typically chained ahead of these terminals.
