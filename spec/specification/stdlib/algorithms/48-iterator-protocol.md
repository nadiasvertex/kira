# 48. The Iterator Protocol

**Status:** Implemented

Covers the core iterator traits (`iterator[T]`, `double_ended[T]`, `exact_size[T]`, `into_iterator[T]`, `from_iter[T]`), `collect`, and the three concrete iterator types `list[T]` provides, all from `src/std/iter.kira`.

## Traits

```kira
pub trait iterator[T]:
    def next(mut self) -> option[T]
    def size_hint(self) -> usize:
        return 0
```

- `next(mut self) -> option[T]` — advances the iterator, moving it. Returns `@some(value)` if a value is available, `@none` when exhausted.
- `size_hint(self) -> usize` — a defaulted method (default `0`), not a separate trait. A lower bound on the number of elements remaining, used only to pre-size a collection in `collect`; an answer that is too small costs a reallocation and nothing else. Adapters that can refine it do (see [Lazy Adapters](49-lazy-adapters.md) for which).

```kira
pub trait double_ended[T] requires iterator[T]:
    def next_back(mut self) -> option[T]
```

- `next_back(mut self) -> option[T]` — removes and returns the last value. This is what makes `rev`, `last`, and (a future) `rfind` linear rather than quadratic: a `rev` over a `double_ended` source draws from its end directly rather than buffering.

```kira
pub trait exact_size[T] requires iterator[T]:
    def exact_len(self) -> usize
```

- `exact_len(self) -> usize` — the exact number of elements remaining. Named `exact_len` rather than `len`, deliberately: under UFCS a trait method named `len` would collide with the free `len` function every container already provides.

```kira
pub trait into_iterator[T]:
    type iter
    def into_iter(self) -> self.iter
```

- For types where iterating means handing back something *else*. `for` does not require this trait — it resolves iterators structurally, so a type with a suitable `next` is already usable in a `for` loop without implementing `into_iterator`.

```kira
pub trait from_iter[T]:
    static def from_iter[I](it: I) -> self
```

- Implement this to make a type a valid target of `collect`. `it: I` is drained (moved) to build `self`.

## `collect`

```kira
pub def collect[I, C](it: I) -> C:
    return C.from_iter(it)
```

A single free function, generic over both the source iterator type `I` and the target collection type `C`. `C` is resolved from the expected type at the call site (`let names: list[str] = words.collect()`) or supplied explicitly where no expected type exists (`words.collect[list[str]]()`).

`collect` is **inferred, not bounds-checked**: a `C` that is not a valid collection is still a compile-time error, but it surfaces indirectly, as a missing `from_iter` on `C` rather than a direct complaint about the `collect` call.

Currently `list[T]` is the only `from_iter` implementor in the standard library:

```kira
impl[T] from_iter[T] for list[T]:
    static def from_iter[I](it: I) -> list[T] where I: iterator[T]:
        var out = []
        var cursor = it
        while let @some(x) = cursor.next():
            out.push(x)
        return out
```

## The three iteration families over `list[T]`

Each is a plain generic struct holding its source and a cursor:

```kira
pub type list_iter[T]      = { src: &list[T],     at: usize, end: usize }  # yields &T
pub type list_iter_mut[T]  = { src: &mut list[T], at: usize, end: usize }  # yields &mut T
pub type list_into_iter[T] = { src: list[T],       at: usize, end: usize }  # yields T
```

Each carries *both* a front cursor (`at`) and a back cursor (`end`) rather than only `at` with the length read from the source: a `next_back` computed as `src[src.len() - 1]` would hand back the same element forever and never meet `next` in the middle. The two cursors close on each other; the iterator is exhausted once `at >= end`.

Constructors, chainable via UFCS as `xs.iter()` / `xs.iter_mut()` / `xs.into_iter()`:

```kira
pub def iter[T](xs: &list[T]) -> list_iter[T]
pub def iter_mut[T](xs: &mut list[T]) -> list_iter_mut[T]
pub def into_iter[T](xs: list[T]) -> list_into_iter[T]
```

`iter` borrows, `iter_mut` borrows mutably (so elements can be mutated in place through the yielded `&mut T`), `into_iter` consumes the list and yields owned values.

Trait coverage of each, exactly as implemented:

| Type | `iterator[T]` | `double_ended[T]` | `exact_size[T]` |
|---|---|---|---|
| `list_iter[T]` | yes (yields `&T`) | yes | yes |
| `list_iter_mut[T]` | yes (yields `&mut T`) | no | no |
| `list_into_iter[T]` | yes (yields `T`) | yes | yes |

`list_iter_mut[T]` implements only `iterator` — it does not currently implement `double_ended` or `exact_size`, unlike the other two families.

## Example

```kira
for x in nums.iter():
    ...                       # x: &int32

let doubled: list[int32] = nums.iter().values().map(x => x * 2).collect()
```

## See also

- [Lazy Adapters](49-lazy-adapters.md) — adapters built as generic `iterator[T]` impls over these types.
- [Aggregation](50-aggregation.md) — the terminals that drain an `iterator[T]`.
- [`list[T]`](../collections/43-list.md) — the container these three families iterate over.
