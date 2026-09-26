# 52. `std.string`

**Status:** Implemented

Covers `std.string`, the UTF-8 method surface `extend`ed onto the built-in `str` view type.

## Model

- `str` is a byte sequence behind a `{ len; data_ptr }` header; `len` is a **byte** length. See [Built-in Types](../../01-core/02-built-in-types.md).
- Byte indices are the currency: `find`/`rfind`/`split` report byte offsets, and `s[a..b]` slices by byte offset. Slicing is O(1) and copy-free — pointer arithmetic over the shared buffer.
- Iteration yields `char` (Unicode scalar values), decoded from UTF-8 as the string is walked. Byte length and scalar length are distinct.
- Every offset these methods report lands on a scalar boundary. This follows from UTF-8's self-synchronization property: a valid UTF-8 needle can only match a valid UTF-8 haystack at scalar boundaries, so byte-level substring search is automatically scalar-correct — search runs at raw-byte speed with no decoding on the hot path.
- Case mapping is Unicode **full** default case mapping (`to_uppercase`/`to_lowercase`), including default multi-code-point expansions and the Final_Sigma context rule; case *folding* (`fold_case`/`eq_ignore_case`) is also full, not simple. See Limitations for what's still out of scope (locale-tailored casing, normalization, grapheme segmentation).
- `reversed` reverses by scalar, not by grapheme cluster.

## Architecture

Three layers:

- **Layer 1 — runtime intrinsics** (`rt_str_*`, `src/runtime/string_ops.h`). Native, UTF-8-aware primitives with no Unicode-table dependency: equality, forward/reverse find, reverse, trim, replace. Search uses Two-Way string matching (Crochemore–Perrin). Shared verbatim by both backends.

  Not-found is signaled by `find_result = { found: bool, pos: usize }`, not a sentinel value and not a native-side `option` construction.

- **Layer 2 — `std.unicode`/`std.unicode_tables`, pure Cinder** (`src/std/unicode.cn`, `src/std/unicode_tables.cn`). Case mapping and folding are *not* a native intrinsic — they're ordinary Cinder over generated Unicode Character Database (UCD) lookup tables. `tools/unicode/gen_case_tables.py` parses `UnicodeData.txt`/`CaseFolding.txt`/`SpecialCasing.txt` and emits `unicode_tables.cn`'s `static` arrays (simple 1:1 mappings, full multi-code-point mappings, full case-folding mappings, and the `cased`/`case-ignorable` code-point ranges Final_Sigma needs); `unicode.cn` decodes a `str`'s UTF-8 bytes by hand (`str.as_bytes()` plus a byte-cursor scalar decoder — see Implementation status for why not `for c in s`), binary-searches those tables (reusing `std.algo`'s `binary_search`/`partition_point`), and re-encodes results.

- **Layer 3 — `extend str` composition** (`src/std/string.cn`, Cinder source). Expresses the public API as composition over Layers 1 and 2: `starts_with`/`ends_with` are one anchored comparison each, `contains` is `find(...).is_some()`, `split` loops over `find` yielding zero-copy sub-slices, `to_uppercase`/`to_lowercase`/`fold_case` forward to `std.unicode`, `eq_ignore_case` folds both sides and compares.

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
    pub def fold_case(self) -> str
    pub def eq_ignore_case(self, other: str) -> bool
    pub def replace(self, from: str, to: str) -> str
    pub def split(self, sep: str) -> list[str]
```

`impl add for str` (`src/std/string.cn`) also gives `str` a `+` operator backed by `rt_str_concat`, distinct from the `extend str` method table above. `eq` is a named method distinct from `==`; `==`/`!=` for `str` dispatch to it.

## Method semantics

- **`is_empty()`** — `true` iff byte length is 0.
- **`eq(other)`** — `true` iff identical byte length and content.
- **`starts_with(prefix)` / `ends_with(suffix)`** — substring anchored at start/end. Empty affix returns `true`; affix longer than `self` returns `false`.
- **`contains(needle)`** — `true` iff `needle` occurs anywhere. Empty needle returns `true`.
- **`find(needle)`** — `@some(byte_offset)` of the first occurrence, else `@none`. Empty needle yields `@some(0)`. Offset is a scalar boundary.
- **`rfind(needle)`** — `@some(byte_offset)` of the last occurrence, else `@none`. Empty needle yields `@some(self.len())`.
- **`trim` / `trim_start` / `trim_end`** — remove leading/trailing scalars with the Unicode `White_Space` property (not just ASCII). Returns a zero-copy sub-slice; returns `self` unchanged when nothing trims.
- **`reversed()`** — scalars emitted in reverse order (valid UTF-8 out).
- **`to_uppercase()` / `to_lowercase()`** — Unicode full default case mapping: a scalar with a default multi-code-point expansion (e.g. `ß` → `SS`, `İ` → `i` + combining dot above) produces all of them; otherwise the simple 1:1 mapping applies; an unmapped scalar passes through. `to_lowercase` additionally applies the Final_Sigma rule: a Greek capital sigma (`Σ`) at the end of a word lowercases to `ς` rather than `σ`.
- **`fold_case()`** — Unicode full case folding (potentially multi-code-point, e.g. `ß` folds to `ss`), for building a caseless comparison rather than for display.
- **`eq_ignore_case(other)`** — `true` iff `self.fold_case() == other.fold_case()`.
- **`replace(from, to)`** — all non-overlapping occurrences of `from` replaced with `to`. Empty `from` leaves `self` unchanged (no zero-width matches).
- **`split(sep)`** — pieces between occurrences of `sep`. Consecutive separators yield empty pieces (`"a::b".split(":")` → `["a", "", "b"]`). Empty `sep` yields `[self]`; no occurrence yields `[self]`.

## Algorithms and complexity

- **Search** (`find`/`rfind`/`contains`/`split`/`replace`): Two-Way string matching. O(n + m) worst case, O(1) extra space, no table proportional to alphabet size. Byte-level and UTF-8-correct by self-synchronization.
- **`eq`/`starts_with`/`ends_with`**: O(min length) after an O(1) length check; no allocation.
- **`split`**: O(n) total search; pieces are O(1) zero-copy sub-slices.
- **`reversed`/`replace`**: single decode/encode pass into one length-reserved buffer — O(n), one allocation.
- **`to_uppercase`/`to_lowercase`/`fold_case`**: O(n) decode into a scalar list, O(m log k) table lookups (`m` output scalars, `k` table size, binary search), O(n) re-encode; one or two allocations. `to_lowercase` additionally scans up to a handful of neighboring scalars per Final_Sigma-eligible `Σ`, not the whole string.
- **`eq_ignore_case`**: two `fold_case` passes plus an O(min length) comparison.
- **`trim*`**: O(leading + trailing) scalar scan; result is a zero-copy sub-slice.

## Example

```kira
let s = "  Hello, Cinder  "
s.trim()                        # "Hello, Cinder"
s.trim().to_uppercase()         # "HELLO, KIRA"
s.contains(" Cinder")              # true
s.find(" Cinder")                  # @some(9), a byte offset into s
"a::b".split(":")                # ["a", "", "b"]
"abc".replace("b", "XY")         # "aXYc"
"straße".to_uppercase()           # "STRASSE"
"STRASSE".eq_ignore_case("straße") # true
```

## Limitations

These are permanent v1 scope limits, not pending work (`spec/todo.md` tracks them explicitly):

- **Locale-tailored casing is out of scope.** `to_uppercase`/`to_lowercase`/`fold_case` apply Unicode's *default* casing algorithm only. The Turkish/Azeri dotted/dotless-I rules and Lithuanian dot-retention (`SpecialCasing.txt`'s locale-conditioned rows) are excluded — they'd need a locale parameter threaded through every casing call, a materially bigger feature than the default algorithm implemented here. `eq`/search remain code-point exact (not case-insensitive) — use `eq_ignore_case` for caseless comparison.
- **No normalization.** Canonically-equivalent but differently-composed strings (e.g. `é` as U+00E9 vs. `e` + U+0301) compare unequal and search independently.
- **Scalar-level, not grapheme-level.** `reversed` reverses scalars; `find` offsets are scalar boundaries, not grapheme boundaries. A combining mark can be separated from its base by `reversed`, or land mid-grapheme in a `split` piece. Grapheme segmentation (UAX #29) is out of scope.
- **`str` methods only.** No `char`-classification API (`is_alphabetic`, etc.) lives here.
- **No regex.** `split`/`replace` take literal substrings only.

## Implementation status

Both compiler backends (bytecode VM and LLVM/AOT) share the `rt_str_*` intrinsics from a single dispatch table (`src/intrinsics.h`), confirmed present in `src/runtime/string.{h,cpp}` and `src/bytecode/vm.cpp`. `to_uppercase`/`to_lowercase`/`fold_case`/`eq_ignore_case` are *not* in that dispatch table — they compile as ordinary Cinder (`src/std/unicode.cn`/`unicode.cn`'s tables), so both backends get them automatically, with no C++ counterpart to keep in sync. `rt_str_to_upper`/`rt_str_to_lower`, the hand-rolled ASCII/Latin-1/Greek/Cyrillic-only C++ intrinsics this chapter previously ran on, have been removed entirely. The `extend str` layer (`src/std/string.cn`) matches the public API above exactly, plus the additional `impl add for str` operator overload noted above, which is not part of `std-reference.md`'s original API table.

This chapter's case-mapping/folding rewrite needed one real compiler fix along the way: `std.unicode_tables`'s generated `static` lookup tables are reached only through `std.unicode`'s functions reading a global, never through a function call into `std.unicode_tables` itself (it declares no functions) — `hir::find_reachable_modules`'s dependency walker had no case for `hir_global_ref` at all, so a module reached *only* through a global reference silently never made it into the compiled program, and every reference to one of its globals failed at bytecode/AOT compile time despite type-checking cleanly. Fixed by giving `hir_global_ref` an `owner_module` field (mirroring `hir_local_ref`'s) and adding the missing walker case; regression test: `src/hir/link_test.cpp`'s `test_discovers_dependency_reached_only_through_a_global`.

Two further gaps were found but deliberately *not* fixed here, as out of scope for this chapter:

- **`for c in s` does not actually decode UTF-8.** Despite this chapter's own "Model" section (and `std.unicode`'s original design) assuming a `for`-loop over a `str` yields decoded Unicode scalars, `str` has no `next(mut self)` method for the general user-iterator desugaring to dispatch to, so it falls through to `hir::lowerer::lower_indexed_loop` — the same byte-indexed loop shape used for `array`/`list`/`slice` — which indexes raw bytes, not scalars. Confirmed by probing `for c in "straße": println(c as uint32)`, which prints `ß`'s two raw continuation bytes (195, 159) instead of its scalar value (223). `std.unicode` works around this by decoding UTF-8 by hand over `str.as_bytes()` instead of using `for`. Fixing the `for`-loop desugaring itself needs new HIR-lowering machinery and is tracked separately (`spec/todo.md`).
- **`\u{XXXX}` string escapes mis-trigger the interpolation scanner.** `has_interpolation`/`scan_interpolated_content` (`src/parser/interp_string.cpp`) skip exactly one character past a backslash for every escape, which is correct for `\n`/`\"`/etc. but wrong for the variable-width `\u{...}` escape: the `{` right after `\u` gets mistaken for the start of a real interpolation hole. A string literal containing `\u{...}` therefore fails to parse at all (reported as an "invalid escape sequence", or an unterminated interpolation, depending on what follows). Worked around in this chapter's own tests by typing non-ASCII characters directly into the `.cn` source files as UTF-8 rather than via the escape. Tracked separately (`spec/todo.md`).

## See also

- [`std.format`](53-std-format.md) — uses `std.string`'s scalar-length and truncation intrinsics for `str` padding.
