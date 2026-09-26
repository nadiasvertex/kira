# 58. `std.traits` — Type Predicates and Transformations

**Status:** Implemented

Compile-time type predicates (`is_integer[T]`, `is_same[A, B]`, ...) and type transformations (`remove_view[T]`, ...), reinterpreting C++'s `<type_traits>` over Cinder's reflection and monomorphization model.

## Rationale

 Cinder monomorphizes every generic instantiation, so a "type trait" is not a runtime tag test — it is an ordinary compile-time boolean or type computed once per instantiation, exactly like the values [Compile-Time Execution](../../03-advanced/31-compile-time-execution.md) already produces. `std.traits` gives that computation a stable, named vocabulary instead of leaving each library to hand-roll it.

The need is not hypothetical: `src/std/algo.cn`'s `is_radix_integer[T]`/`is_signed_integer[T]` (backing `sort[T]`'s integer fast path) already compare `T.name()` against a hand-written list of string literals. `std.traits` generalizes that pattern into a reusable predicate library and gives `algo.cn` a single call (`is_integer[T]() and bit_width[T]() <= 64`) in place of the string list — see [Implementation status](#implementation-status).

## Type predicates

Each predicate is a `static pure def name[T]() -> bool`, evaluated by the compile-time interpreter and usable anywhere a compile-time `bool` is (`static if`, `static assert`, a `concept` value-constraint clause, an ordinary generic body per [Compile-Time Execution § reflection](../../03-advanced/31-compile-time-execution.md#compile-time-reflection)).

| Predicate | True for |
|---|---|
| `is_bool[T]` | `bool` |
| `is_char[T]` | `char` |
| `is_signed_integer[T]` | `int8`..`int128`, `isize` |
| `is_unsigned_integer[T]` | `uint8`..`uint128`, `usize`, `byte` |
| `is_integer[T]` | either of the above |
| `is_float[T]` | `float32`, `float64`, `float128` |
| `is_numeric[T]` | `is_integer[T]` or `is_float[T]` |
| `is_scalar[T]` | `is_numeric[T]`, `is_bool[T]`, or `is_char[T]` |
| `is_str[T]` | `str` |
| `is_unit[T]` | `unit` |
| `is_array[T]` | `array[_, _]` |
| `is_slice[T]` | `slice[_]` or `mut slice[_]` |
| `is_view[T]` | `&T'` or `&mut T'` for some `T'` (see [Views](../../02-intermediate/15-views.md)) |
| `is_box[T]` | `box[_]` or `box[trait]` |
| `is_option[T]` | `option[_]` |
| `is_result[T]` | `result[_, _]` |
| `is_fn[T]` | a function type `fn(...) -> _` |
| `is_struct[T]` | a user `type ... = { ... }` declaration |
| `is_sum[T]` | a user `type ... = @a | @b(...) | ...` declaration |

`is_struct`/`is_sum` are the only two that read a user declaration rather than matching a builtin name; both are thin wrappers over the `T.kind()` primitive introduced in [Meta Queries § type_kind](60-meta-queries.md#type_kind).

## Type relationships

```kira
is_same[A, B]() -> bool          # A and B intern to the same type_id
is_convertible[From, To]() -> bool   # `into[To]` is implemented for From, or From == To
implements[T, Trait]() -> bool   # T has an impl of Trait (kind-* trait only)
```

- `is_same[A, B]` is `type_table` id equality — the same notion of sameness [Higher-Kinded Traits](../../03-advanced/37-higher-kinded-traits.md) relies on for substitution normalization. It is reflexive, symmetric, and requires no bound on `A`/`B`.
- `is_convertible[From, To]` is `is_same[From, To] or implements[From, into[To]]()`; it does not attempt an actual conversion.
- `implements[T, Trait]` answers a coherence-table lookup at compile time — the same lookup `check.cpp`'s impl-coherence pass performs, exposed as a queryable boolean rather than only a pass/fail obligation on a bound list. It accepts only a kind-`*` trait name; a higher-kinded trait or a `concept` is diagnosed (concepts are structural by construction — see [Concepts](../../03-advanced/35-concepts.md) — so "does `T` implement concept `C`" is written as the bound `T: C` directly, never through `implements`).

## Type transformations

Transformations return a `type_expr` (see [Compile-Time Execution § Quoting and splicing](../../03-advanced/31-compile-time-execution.md#quoting-and-splicing)) rather than a value, and are spliced into type position with `~`:

```kira
remove_view[T]() -> type_expr    # &T' or &mut T' -> T'; T unchanged otherwise
add_view[T]() -> type_expr       # T -> &T
add_mut_view[T]() -> type_expr   # T -> &mut T
decay[T]() -> type_expr          # remove_view, then array[E, n] -> slice[E]
element_type[T]() -> type_expr   # array[E, _]/slice[E]/list[E]/option[E] -> E
```

```kira
def first[T](xs: &list[T]) -> option[~element_type[T]()]:
    ...
```

`decay[T]` composes `remove_view` and the array-to-slice step in one call because that pair is exactly what a generic function taking `items: &array[T, n]` and wanting to treat it uniformly with a `slice[T]` parameter needs — the two steps have no independent use case that would justify keeping them as separate calls.

## `type_category` — a single dispatch point

Rather than testing every predicate in turn, `type_category[T]() -> type_category` returns one value of a prelude sum type, for use in a `static if`/`match` dispatch:

```kira
type type_category =
    | @boolean
    | @character
    | @signed_integer
    | @unsigned_integer
    | @floating_point
    | @string
    | @array_like
    | @view
    | @struct_like
    | @sum_like
    | @function
    | @other

static pure def describe[T]() -> str:
    match type_category[T]():
        @boolean          => "bool"
        @signed_integer | @unsigned_integer => "integer"
        @floating_point   => "float"
        _                 => "other"
```

This mirrors `T.kind()` from [Meta Queries](60-meta-queries.md) but resolves the builtin/user-declaration split into one flat enumeration, so a consumer that only cares about "what broad shape is this" does not need to call both a builtin-name predicate and `T.kind()` and merge the results itself.

## See also

- [Compile-Time Execution](../../03-advanced/31-compile-time-execution.md) — the `static`/reflection substrate every predicate here is built from.
- [Numeric Limits](59-std-limits.md) — the numeric-specific counterpart (`min[T]()`, `bits[T]()`, ...) to this chapter's `is_integer`/`is_float`.
- [Meta Queries](60-meta-queries.md) — `T.kind()`, the declaration-shape primitive `is_struct`/`is_sum`/`type_category` are defined over.
- [Higher-Kinded Traits](../../03-advanced/37-higher-kinded-traits.md) — the substitution/id-equality model `is_same` reuses.
