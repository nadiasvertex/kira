# 52. `std.string`

**Status:** Implemented

Covers `std.string`, the UTF-8 method surface `extend`ed onto the built-in `str` view type.

## Model

- `str` is a byte sequence behind a `{ len; data_ptr }` header; `len` is a **byte** length. See [Built-in Types](../../core/02-built-in-types.md).
- Byte indices are the currency: `find`/`rfind`/`split` report byte offsets, and `s[a..b]` slices by byte offset. Slicing is O(1) and copy-free — pointer arithmetic over the shared buffer.
- Iteration yields `char` (Unicode scalar values), decoded from UTF-8 as the string is walked. Byte length and scalar length are distinct.
- Every offset these methods report lands on a scalar boundary. This follows from UTF-8's self-synchronization property: a valid UTF-8 needle can only match a valid UTF-8 haystack at scalar boundaries, so byte-level substring search is automatically scalar-correct — search runs at raw-byte speed with no decoding on the hot path.
- Case mapping is Unicode **simple** (1:1) case mapping; see Limitations.
- `reversed` reverses by scalar, not by grapheme cluster.

## Architecture

Two layers:

- **Layer 1 — runtime intrinsics** (`rt_str_*`, `src/runtime/string_ops.h`). Native, UTF-8-aware primitives: equality, forward/reverse find, upper/lower case mapping, reverse, trim, replace. Search uses Two-Way string matching (Crochemore–Perrin); case mapping uses a generated Unicode simple-case-mapping table. Shared verbatim by both backends.

  Not-found is signaled by `find_result = { found: bool, pos: usize }`, not a sentinel value and not a native-side `option` construction.

- **Layer 2 — `extend str` composition** (`src/std/string.kira`, Kira source). Expresses the public API as composition over Layer 1: `starts_with`/`ends_with` are one anchored comparison each, `contains` is `find(...).is_some()`, `split` loops over `find` yielding zero-copy sub-slices.

## Public API

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

`impl add for str` (`src/std/string.kira`) also gives `str` a `+` operator backed by `rt_str_concat`, distinct from the `extend str` method table above. `eq` is a named method distinct from `==`; `==`/`!=` for `str` dispatch to it.

## Method semantics

- **`is_empty()`** — `true` iff byte length is 0.
- **`eq(other)`** — `true` iff identical byte length and content.
- **`starts_with(prefix)` / `ends_with(suffix)`** — substring anchored at start/end. Empty affix returns `true`; affix longer than `self` returns `false`.
- **`contains(needle)`** — `true` iff `needle` occurs anywhere. Empty needle returns `true`.
- **`find(needle)`** — `@some(byte_offset)` of the first occurrence, else `@none`. Empty needle yields `@some(0)`. Offset is a scalar boundary.
- **`rfind(needle)`** — `@some(byte_offset)` of the last occurrence, else `@none`. Empty needle yields `@some(self.len())`.
- **`trim` / `trim_start` / `trim_end`** — remove leading/trailing scalars with the Unicode `White_Space` property (not just ASCII). Returns a zero-copy sub-slice; returns `self` unchanged when nothing trims.
- **`reversed()`** — scalars emitted in reverse order (valid UTF-8 out).
- **`to_uppercase()` / `to_lowercase()`** — Unicode simple case mapping applied per scalar; unmapped scalars pass through.
- **`replace(from, to)`** — all non-overlapping occurrences of `from` replaced with `to`. Empty `from` leaves `self` unchanged (no zero-width matches).
- **`split(sep)`** — pieces between occurrences of `sep`. Consecutive separators yield empty pieces (`"a::b".split(":")` → `["a", "", "b"]`). Empty `sep` yields `[self]`; no occurrence yields `[self]`.

## Algorithms and complexity

- **Search** (`find`/`rfind`/`contains`/`split`/`replace`): Two-Way string matching. O(n + m) worst case, O(1) extra space, no table proportional to alphabet size. Byte-level and UTF-8-correct by self-synchronization.
- **`eq`/`starts_with`/`ends_with`**: O(min length) after an O(1) length check; no allocation.
- **`split`**: O(n) total search; pieces are O(1) zero-copy sub-slices.
- **`to_uppercase`/`to_lowercase`/`reversed`/`replace`**: single decode/encode pass into one length-reserved buffer — O(n), one allocation.
- **`trim*`**: O(leading + trailing) scalar scan; result is a zero-copy sub-slice.

## Example

```kira
let s = "  Hello, Kira  "
s.trim()                        # "Hello, Kira"
s.trim().to_uppercase()         # "HELLO, KIRA"
s.contains("Kira")              # true
s.find("Kira")                  # @some(9), a byte offset into s
"a::b".split(":")                # ["a", "", "b"]
"abc".replace("b", "XY")         # "aXYc"
```

## Limitations

These are permanent v1 scope limits, not pending work (`spec/todo.md` item 2 defers them explicitly, pending additional generated Unicode tables):

- **Simple case mapping only.** `to_uppercase`/`to_lowercase` apply Unicode 1:1 simple mappings. Full/special casing (`ß`→`SS`, final-sigma, locale-tailored Turkish/Azeri/Lithuanian rules) and case *folding* for caseless comparison are out of scope. `eq`/search are code-point exact, not case-insensitive or normalization-aware.
- **No normalization.** Canonically-equivalent but differently-composed strings (e.g. `é` as U+00E9 vs. `e` + U+0301) compare unequal and search independently.
- **Scalar-level, not grapheme-level.** `reversed` reverses scalars; `find` offsets are scalar boundaries, not grapheme boundaries. A combining mark can be separated from its base by `reversed`, or land mid-grapheme in a `split` piece. Grapheme segmentation (UAX #29) is out of scope.
- **`str` methods only.** No `char`-classification API (`is_alphabetic`, etc.) lives here.
- **No regex.** `split`/`replace` take literal substrings only.

## Implementation status

Both compiler backends (bytecode VM and LLVM/AOT) share the `rt_str_*` intrinsics from a single dispatch table (`src/intrinsics.h`), confirmed present in `src/runtime/string.{h,cpp}`, `src/bytecode/vm.cpp`, and `src/llvm_codegen/codegen.cpp`. The `extend str` layer (`src/std/string.kira`) matches the public API above exactly, plus the additional `impl add for str` operator overload noted above, which is not part of `std-reference.md`'s original API table.

## See also

- [`std.format`](53-std-format.md) — uses `std.string`'s scalar-length and truncation intrinsics for `str` padding.
