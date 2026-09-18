# 42. The Prelude

**Status:** Implemented

The prelude is the set of names available in every module without a `use` declaration. It is injected by the driver for every file unless the file opts out.

## Contents

**Types:** `bool`, `char`, `str`, `unit`, `byte`, all numeric types (`int8`..`int128`, `uint8`..`uint128`, `float32`/`float64`/`float128`, `isize`, `usize`), `array`, `slice`, `mut slice`, `option`, `result`, `list`, `box`, `cell`, `cell_mut`.

**Traits:** `eq`, `ord`, `hash`, `show`, `from`, `into`, `add`, `sub`, `mul`, `div`, `rem`, `neg`, `drop`, `index`, `index_mut`, `index_set`, `from_array`, and the remaining arithmetic operator traits.

`index`/`index_set` are what make `v[i]` and `v[i] = x` work on a user type, and `from_array` is what makes `let v: my_type = [1, 2, 3]` work — the operators that used to be reserved for the built-in sequences. `index_mut` is declared but not yet wired (`../../todo.md` item 19).

**Concepts:** `send`, `share`.

**Functions:** `println`, `print`, `panic`, `assert`, `size_of`, `align_of`, `ptr_cast`, `uninit`, `args`, `env`, `drop`.

`drop` is the one entry here that does not exist: no `def drop` is defined outside the `drop` trait itself, and a call to `drop(x)` type-checks but then fails to lower (see [Shared Ownership and Drop](../02-intermediate/17-shared-ownership-and-drop.md), Implementation status). The `drop` *trait* listed above is genuinely prelude-reachable.

Four of these are not ordinary functions but compiler-answered forms, each taking its argument in brackets:

- `size_of[T]()` and `align_of[T]()` fold at lowering time to the bytes a `T` occupies and the boundary it must start on, answered from `runtime::layout_of` — the one function both backends read every field offset and element stride from. Note that a struct, sum, `list` or other heap-referenced type answers 8: what such a value occupies *as a binding, field or element* is a pointer. That is the number a collection needs to stride its storage by. `size_of(expr)` is also accepted, asking about the expression's type; the expression is type-checked but never evaluated.
- `ptr_cast[U](p)` reinterprets a raw pointer as pointing at a `U`. Mutability follows the operand, so a cast can never gain the right to write.
- `uninit[T, N]()` is a fixed-capacity, alignment-correct buffer of `N` slots sized for `T`, allocated in the enclosing frame rather than on the heap.

The last three are raw-memory operations and are refused outside a `machine` function — see [The `machine` Layer](../03-advanced/38-machine-layer.md). `size_of`/`align_of` are not gated: they only ask about a type.

Each prelude type/trait/function is specified in full in its owning chapter — this list is an index, not the normative definition of any of them. See [Built-in Types](../01-core/02-built-in-types.md), [Traits](../02-intermediate/18-traits.md), [Error Handling](../01-core/11-error-handling.md) (`option`/`result`), [Views](../02-intermediate/15-views.md) (`slice`, `cell`), [Trait Objects](../02-intermediate/24-trait-objects.md) (`box`), and [Data-Race Freedom](../02-intermediate/30-data-race-freedom.md) (`send`/`share`).

## Opting out

A file that declares `no_prelude` after its `module` line receives none of the above and must `use` everything it needs, including basic operator traits:

```kira
module my_module
no_prelude
```

This is intended for low-level or embedded code that wants full control over what's in scope.

## See also

- [Overview](../00-overview.md)
