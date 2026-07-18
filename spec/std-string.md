# std.string: String extension methods

## Overview

`std.string` provides real, Kira-source implementations of string manipulation methods that currently exist only as "vaporware" entries in the compiler's hardcoded builtin-method table (`builtin_method_result` in `src/semantic/check.cpp`). These methods type-check but have zero codegen in either backend — attempting to call them fails at lowering/codegen with a "no scalar representation" error.

By providing actual Kira-source `extend str:` implementations, this module makes these methods usable end-to-end. All implementations work in terms of primitives that *do* have real codegen support: `.as_bytes()`, `str[a..b]` slicing, `byte` equality/comparison, and `rt_str_concat` for string concatenation.

## Compiler integration

Making user-defined `extend str` methods take effect requires a small compiler change: remove the vaporware entries from `builtin_method_result`'s `str` case in `src/semantic/check.cpp:6577-6596`, keeping only `len` and `as_bytes` (the two with real lowering support). This allows the checker's existing fallback logic (`infer_method_call`, `check.cpp:7025-7026`) to discover the real implementations in this module.

Entries to remove:
- `contains`, `starts_with`, `ends_with`, `is_empty`
- `to_uppercase`, `to_lowercase`, `replace`, `trim`, `reversed`
- `split`

## API surface

```kira
extend str:
    pub def is_empty(self) -> bool
    pub def eq(self, other: str) -> bool
    pub def starts_with(self, prefix: str) -> bool
    pub def ends_with(self, suffix: str) -> bool
    pub def contains(self, needle: str) -> bool
    pub def find(self, needle: str) -> option[usize]
    pub def rfind(self, needle: str) -> option[usize]
    pub def trim(self) -> str
    pub def trim_start(self) -> str
    pub def trim_end(self) -> str
    pub def reversed(self) -> str
    pub def to_uppercase(self) -> str
    pub def to_lowercase(self) -> str
    pub def replace(self, from: str, to: str) -> str
    pub def split(self, sep: str) -> list[str]
```

### Method descriptions

- **`is_empty()`** → `bool`: returns `true` if `self.len() == 0`, else `false`.

- **`eq(other: str)`** → `bool`: byte-for-byte equality; use this instead of the `==` operator, which has no codegen support for `str`. Returns `true` only if `self` and `other` have identical length and content.

- **`starts_with(prefix: str)`** → `bool`: returns `true` if `self` begins with the substring `prefix`, else `false`. Empty prefix always returns `true`. Returns `false` if `prefix` is longer than `self`.

- **`ends_with(suffix: str)`** → `bool`: returns `true` if `self` ends with the substring `suffix`, else `false`. Empty suffix always returns `true`. Returns `false` if `suffix` is longer than `self`.

- **`contains(needle: str)`** → `bool`: returns `true` if `needle` occurs anywhere within `self`, else `false`. Empty needle always returns `true`.

- **`find(needle: str)`** → `option[usize]`: returns `@some(i)` where `i` is the byte-index of the first occurrence of `needle` in `self`, or `@none` if `needle` does not occur. Empty needle returns `@some(0)` (match at the start).

- **`rfind(needle: str)`** → `option[usize]`: returns `@some(i)` where `i` is the byte-index of the last occurrence of `needle` in `self`, or `@none` if `needle` does not occur. Empty needle returns `@some(self.len())` (match at the end).

- **`trim()`** → `str`: returns a new string with leading and trailing ASCII whitespace (space, tab, newline, carriage return, form feed, vertical tab) removed. Returns `self` unchanged if no trimming is needed.

- **`trim_start()`** → `str`: returns a new string with leading ASCII whitespace removed.

- **`trim_end()`** → `str`: returns a new string with trailing ASCII whitespace removed.

- **`reversed()`** → `str`: returns a new string with bytes in reverse order. No semantic understanding of Unicode grapheme clusters — strictly reverses byte-by-byte.

- **`to_uppercase()`** → `str`: returns a new string with ASCII lowercase letters (`a-z`) converted to uppercase (`A-Z`). Non-ASCII bytes and non-letter characters are unchanged. Behavior is purely ASCII; full Unicode case folding is out of scope.

- **`to_lowercase()`** → `str`: returns a new string with ASCII uppercase letters (`A-Z`) converted to lowercase (`a-z`). Non-ASCII bytes and non-letter characters are unchanged.

- **`replace(from: str, to: str)`** → `str`: returns a new string with all non-overlapping occurrences of the substring `from` replaced with `to`. If `from` does not occur in `self`, returns `self` unchanged. Empty `from` substring is treated as "no match" and returns `self` unchanged (no infinite-loop behavior with zero-width matches).

- **`split(sep: str)`** → `list[str]`: splits `self` on occurrences of the literal substring `sep`, returning a list of the pieces. Consecutive separators each produce an empty-string piece (e.g., `"a::b".split(":")` returns `["a", "", "b"]`). If `sep` is empty, returns a single-element list containing `self` unchanged (no zero-width match behavior). If `sep` does not occur, returns a single-element list containing `self`.

## Implementation notes

- All byte-level scanning uses a shared private helper `index_of_from(haystack: slice[byte], needle: slice[byte], from: usize) -> option[usize]`, which implements a naive O(n·m) substring search sufficient for v1's use cases.

- `starts_with` and `ends_with` reduce to a length check plus one anchored comparison call to `index_of_from`, not a full scan.

- `contains` and `find` scan from the start using `index_of_from` repeatedly.

- `rfind` either scans from the end backwards or uses repeated `find` calls, tracking the last match found.

- `to_uppercase`, `to_lowercase`, and `reversed` build new strings by concatenating one character at a time via `rt_str_concat`. Each character is obtained by slicing the original string — e.g., `s[i..i+1]` yields a one-character `str` — then either passed through or case-folded (via arithmetic on the byte value) before concatenation.

- `replace` internally uses `split(from)` to break the string into pieces, then joins them back with `to` using `rt_str_concat` in a loop.

- Whitespace in `trim`/`trim_start`/`trim_end` is defined as ASCII bytes: space (0x20), tab (0x09), newline (0x0a), carriage return (0x0d), form feed (0x0c), vertical tab (0x0b).

## Guarantees and limitations

- **No Unicode support**: all methods work at the byte level, with no understanding of UTF-8 encoding or Unicode grapheme clusters. Slicing a multi-byte UTF-8 sequence produces invalid UTF-8 in the result.
- **ASCII-only case folding**: `to_uppercase` and `to_lowercase` only recognize ASCII letters; non-ASCII bytes pass through unchanged.
- **Byte indices, not character indices**: `find`, `rfind`, and indices returned by `split` refer to byte positions, not character or grapheme positions.
- **Naive substring search**: O(n·m) performance; fine for v1's expected string sizes and use cases, but no optimizations like Boyer-Moore or rolling-hash employed.
- **No regex support**: `split` and `replace` only accept literal substrings, not patterns.

## Testing

Test fixtures in `src/testdata/std_test/` cover each method with at least one representative case plus edge cases (empty strings, empty needles, no matches, etc.):
- `string_eq.kira` — equality checks
- `string_starts_ends_contains.kira` — prefix/suffix/substring detection
- `string_find.kira` — forward and backward searches
- `string_split.kira` — splitting on separators
- `string_trim.kira` — whitespace trimming
- `string_case.kira` — uppercase/lowercase conversion
- `string_replace.kira` — substring replacement
- `string_reversed.kira` — byte-level reversal
