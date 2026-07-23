# 7. Strings

**Status:** Implemented

Covers the built-in `str` type, string literal interpolation and escape sequences, and `char`. This chapter covers only the built-in type and its literal syntax; the method surface (`.contains`, `.split`, `.replace`, etc.) is specified in [std.string](../stdlib/strings-and-formatting/52-std-string.md) — not duplicated here.

## `str`

Kira strings are UTF-8. `str` literals support interpolation with `{...}`, evaluated and formatted in place:

```kira
let name = "World"
let msg  = "Hello, {name}!"          # "Hello, World!"
let calc = "2 + 2 = {2 + 2}"         # "2 + 2 = 4"
```

An interpolation brace is written literally by doubling it:

```kira
let literal = "{{x}} stays literal"    # "{x} stays literal" — {{ is {, }} is }
```

`str` methods (`.len()`, `.to_uppercase()`, `.contains()`, `.split()`, ...) are specified in [std.string](../stdlib/strings-and-formatting/52-std-string.md), including the byte-vs-character-count distinction for `.len()`.

## Escape sequences

String and char literals use a backslash escape. Supported escapes: `\"`, `\\`, `\n`, `\t`, `\r`, `\0`.

```kira
let quoted = "She said \"hi\"\n"
```

## `char`

`char` is a single Unicode scalar value, written in single quotes.

```kira
let ch = 'a'    # a char; also '\n', '♥'
```

## See also

- [std.string](../stdlib/strings-and-formatting/52-std-string.md) — the `str` method surface.
- [Collections](06-collections-list-array.md) — `list`/`array`, the other built-in sequence types.
