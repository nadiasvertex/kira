# 42. The Prelude

**Status:** Implemented

The prelude is the set of names available in every module without a `use` declaration. It is injected by the driver for every file unless the file opts out.

## Contents

**Types:** `bool`, `char`, `str`, `unit`, `byte`, all numeric types (`int8`..`int128`, `uint8`..`uint128`, `float32`/`float64`/`float128`, `isize`, `usize`), `array`, `slice`, `mut slice`, `option`, `result`, `list`, `box`, `cell`, `cell_mut`.

**Traits:** `eq`, `ord`, `hash`, `show`, `from`, `into`, `add`, `sub`, `mul`, `div`, `rem`, `neg`, `drop`, and the remaining arithmetic operator traits.

**Concepts:** `send`, `share`.

**Functions:** `println`, `print`, `panic`, `assert`, `size_of`, `args`, `env`, `drop`.

Each prelude type/trait/function is specified in full in its owning chapter — this list is an index, not the normative definition of any of them. See [Built-in Types](../core/02-built-in-types.md), [Traits](../intermediate/18-traits.md), [Error Handling](../core/11-error-handling.md) (`option`/`result`), [Views](../intermediate/15-views.md) (`slice`, `cell`), [Trait Objects](../intermediate/24-trait-objects.md) (`box`), and [Data-Race Freedom](../intermediate/30-data-race-freedom.md) (`send`/`share`).

## Opting out

A file that declares `no_prelude` after its `module` line receives none of the above and must `use` everything it needs, including basic operator traits:

```kira
module my_module
no_prelude
```

This is intended for low-level or embedded code that wants full control over what's in scope.

## See also

- [Overview](../00-overview.md)
