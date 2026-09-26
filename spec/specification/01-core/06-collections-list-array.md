# 6. Collections: `list` and `array`

**Status:** Implemented

Covers the two built-in prelude sequence types, `list[T]` and `array[T, n]`, their literal syntax and core operations, and the distinction between them. Other collections (`map[K, V]`, `set[T]`, etc.) are standard library types, not prelude built-ins — see [Collections](../04-stdlib/collections/) in the standard library section.

## `list[T]`

A resizable, heap-allocated sequence, available without any import.

```cinder
let names: list[str] = ["Alice", "Bob", "Carol"]
let first = names[0]           # "Alice"
let count = names.len()        # 3

var items: list[int32] = []
items.push(1)
items.push(2)
```

Standard operations include `map`, `filter`, `find` (returns `option[T]`), `any`, `all`:

```cinder
names.map(n => n.to_uppercase())      # produces a new list
names.filter(n => n.len() > 3)        # keeps elements matching predicate
names.find(n => n.starts_with("A"))   # returns option[str]
names.any(n => n == "Bob")            # returns bool
names.all(n => n.len() > 0)           # returns bool
```

## `array[T, n]`

A fixed-size sequence whose length `n` is part of its type and known at compile time. A `[value; n]` literal repeats `value` `n` times.

```cinder
let zeros: array[float64, 4] = [0.0; 4]    # four zeros
let rgb:   array[uint8,   3] = [255, 0, 0] # red
```

## `list` vs. `array`

`list` is heap-allocated and growable; its length is a runtime property. `array` is fixed-size; its length is part of the type (`array[T, n]` is a distinct type per `n`) and requires no heap allocation. For general-purpose code, `list` is the default choice; `array` is for when the size is fixed and known at compile time.

### What an unannotated literal is

A sequence literal with no expected type is a **`list`** — for both spellings, including the repeat form:

```cinder
let xs = [1, 2, 3]                        # list[int32]
let zeros = [0.0; 4]                      # list[float64]
let rgb: array[uint8, 3] = [255, 0, 0]    # array, because it was asked for
```

One rule covers both forms deliberately. "A literal with a constant repeat count is an array, otherwise a list" would make `[0; 4]` and `[0, 0, 0, 0]` different types — a distinction nothing else in the language draws, and one a reader would have to memorize rather than derive. An `array` is what you ask for, matching the default this section states.

### Indexing and iteration are traits

`v[i]`, `v[i] = x` and `for x in v` are not reserved for the built-in sequences. A user type provides them by implementing `std.traits.index`, `std.traits.index_set`, and `std.iter.into_iterator`; a literal builds one through `std.traits.from_array`. See [The `machine` Layer](../03-advanced/38-machine-layer.md) and `spec/list-migration-design.md` — `std.list`'s `vector[T]` is a growable sequence written entirely in Cinder over exactly those traits.

## See also

- [Strings](07-strings.md) — `str` is a view type with its own method surface, not a `list`.
- Collections (Standard Library) — `map[K, V]`, `set[T]`, and the rest of the collection types outside the prelude.
