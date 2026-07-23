# 6. Collections: `list` and `array`

**Status:** Implemented

Covers the two built-in prelude sequence types, `list[T]` and `array[T, n]`, their literal syntax and core operations, and the distinction between them. Other collections (`map[K, V]`, `set[T]`, etc.) are standard library types, not prelude built-ins — see [Collections](../stdlib/collections/) in the standard library section.

## `list[T]`

A resizable, heap-allocated sequence, available without any import.

```kira
let names: list[str] = ["Alice", "Bob", "Carol"]
let first = names[0]           # "Alice"
let count = names.len()        # 3

var items: list[int32] = []
items.push(1)
items.push(2)
```

Standard operations include `map`, `filter`, `find` (returns `option[T]`), `any`, `all`:

```kira
names.map(n => n.to_uppercase())       # produces a new list
names.filter(n => n.len() > 3)        # keeps elements matching predicate
names.find(n => n.starts_with("A"))   # returns option[str]
names.any(n => n == "Bob")            # returns bool
names.all(n => n.len() > 0)           # returns bool
```

## `array[T, n]`

A fixed-size sequence whose length `n` is part of its type and known at compile time. A `[value; n]` literal repeats `value` `n` times.

```kira
let zeros: array[float64, 4] = [0.0; 4]    # four zeros
let rgb:   array[uint8,   3] = [255, 0, 0] # red
```

## `list` vs. `array`

`list` is heap-allocated and growable; its length is a runtime property. `array` is fixed-size; its length is part of the type (`array[T, n]` is a distinct type per `n`) and requires no heap allocation. For general-purpose code, `list` is the default choice; `array` is for when the size is fixed and known at compile time.

## See also

- [Strings](07-strings.md) — `str` is a view type with its own method surface, not a `list`.
- Collections (Standard Library) — `map[K, V]`, `set[T]`, and the rest of the collection types outside the prelude.
