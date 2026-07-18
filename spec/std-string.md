# std.string: UTF-8 string extension methods

## Overview

`std.string` provides real implementations of the string-manipulation methods
that currently exist only as "vaporware" entries in the compiler's hardcoded
builtin-method table (`builtin_method_result` in `src/semantic/check.cpp`).
Those methods type-check but have zero codegen in either backend — calling one
fails at lowering with a "no scalar representation"/"no byte view model" error.

This module makes them usable end-to-end, and does so as **UTF-8 from the
ground up**: every operation is defined in terms of the Unicode scalar values
encoded by the string's UTF-8 bytes, not raw bytes or ASCII. The
performance-critical primitives (substring search, case mapping, comparison,
reversal, trimming, replacement) are implemented once, efficiently, as native
`kira_rt_str_*` runtime intrinsics — reusing the existing UTF-8 codec in
`src/utf8/` and the intrinsic ABI already used by `std.fmt`. A thin
`extend str:` layer written in Kira source composes those primitives into the
public API, and is what exercises the `extend`-on-builtin resolution path
end-to-end.

Efficiency is a first-class goal here: the compiler teaches the language, but
the *programs it produces* must be fast. These methods use the best practical
algorithm for each operation (Boyer–Moore–Horspool / Two-Way search, O(1)
zero-copy sub-slicing, single-pass table-driven case mapping), not the naive
O(n·m) fallback the first draft assumed.

## String model

The model mirrors Rust's, which is the proven design for fast, correct UTF-8:

- **`str` is a sequence of UTF-8 bytes** behind a `{ len; data_ptr }` header
  (`src/runtime/fmt.cpp`), where `len` is the **byte** length.
- **Byte indices are the currency.** `str.len()` is the byte length (O(1));
  `find`/`rfind`/`split` report **byte offsets**; slicing is `s[a..b]` over
  byte offsets. Byte indexing is what makes slicing O(1) and copy-free — a
  range slice is pure pointer arithmetic over the shared buffer
  (`compile_range_index`, both backends), so sub-slices never allocate.
- **Iteration yields `char` (Unicode scalar values).** `for c in s:` already
  lowers to a UTF-8-decoding loop (`lower.cpp:2713`, element type `char`);
  scalar count is available via `rt_str_len_scalars` (`std.fmt`'s
  `scalar_len`). Byte length and scalar length are distinct and both exposed.
- **Every reported byte offset lands on a scalar boundary.** This is the
  self-synchronizing property of UTF-8: a valid UTF-8 needle can only match a
  valid UTF-8 haystack at scalar boundaries, so **byte-level substring search
  is automatically scalar-correct**. This is the theorem that lets search run
  at raw-byte speed while staying UTF-8-safe — we never pay for decoding in the
  hot path.
- **Case mapping is Unicode simple (1:1) case mapping.** Full/special casing
  (multi-scalar expansions like `ß`→`SS`, locale-tailored rules like Turkish
  dotless-i) is explicitly out of scope for v1; see Limitations.
- **`reversed` reverses by scalar**, not by grapheme cluster. Combining marks
  and ZWJ sequences are not kept adjacent to their base (that needs UAX #29
  segmentation); see Limitations.

## Architecture: two layers

### Layer 1 — runtime intrinsics (native, fast, UTF-8-aware)

New `kira_rt_str_*` symbols in a new `src/runtime/string.cpp` (+ `string.h`),
following the exact ABI of the existing `kira_rt_str_*`/`kira_rt_fmt_*`
intrinsics (`src/runtime/fmt.cpp`): every argument and result is an opaque heap
`uint64_t*`; strings are `{ len; data_ptr }` headers read with `view_of` and
built with `make_str`; scalars are boxed single-field structs (`box_usize`,
`box_bool`, `box_u32` from `src/std/fmt.kira`). They link the existing
`//src/utf8` codec.

| Intrinsic | Signature (Kira) | Algorithm |
|---|---|---|
| `rt_str_eq` | `(a: str, b: str) -> box_bool` | length check + `memcmp` |
| `rt_str_find` | `(h: str, n: str, from: box_usize) -> find_result` | Two-Way string matching over bytes; returns `{ found, byte_offset }` |
| `rt_str_rfind` | `(h: str, n: str) -> find_result` | reverse Two-Way; `{ found, byte_offset }` |
| `rt_str_to_upper` | `(s: str) -> str` | single pass: decode scalar → simple upper-map table → encode; `reserve`d builder |
| `rt_str_to_lower` | `(s: str) -> str` | single pass, simple lower-map table |
| `rt_str_reverse` | `(s: str) -> str` | single pass, emit scalars in reverse |
| `rt_str_trim` | `(s: str, mode: box_u8) -> str` | scan leading/trailing Unicode `White_Space` scalars; return zero-copy sub-slice (`mode` 0=both, 1=start, 2=end) |
| `rt_str_replace` | `(s: str, from: str, to: str) -> str` | BMH find in a loop into one `reserve`d builder |

Notes:
- **Not-found signaling.** `rt_str_find`/`rt_str_rfind` return a two-field
  `find_result = { found: bool, pos: usize }` (a 2-slot heap struct); the Kira
  wrapper maps `found == false` to `@none`. A plain struct — rather than a
  magic `usize::MAX` sentinel or constructing an `option` in C++ — needs no
  reserved value and doesn't hard-code the sum-type layout in the runtime.
  Empty-needle results are defined by the intrinsic: `find` yields
  `{ true, from }`, `rfind` yields `{ true, len }`.
- **Case-mapping table.** Layer 1 needs a Unicode simple-case-mapping table
  (upper/lower fields of `UnicodeData.txt`). Generate a compact
  `src/runtime/unicode_case_table.inc` (sorted ranges + deltas, binary search)
  at build time from the checked-in Unicode data, or vendor the generated
  `.inc`. ASCII gets a branch-free fast path; the table covers the BMP scripts
  (Latin, Greek, Cyrillic, Armenian, fullwidth, …). This is the only new data
  dependency.
- **Why intrinsics, not Kira source, for these.** Efficient substring search
  and Unicode case mapping are exactly the operations a young stdlib should not
  hand-roll: the runtime can use `std::boyer_moore_horspool_searcher`, `memcmp`,
  and generated tables directly, and building result strings with an amortized
  `std::string` avoids the O(n²) allocation blowup that repeated `rt_str_concat`
  would cause. This mirrors how `std.fmt` already delegates formatting to
  `rt_fmt_*`.

### Layer 2 — `extend str` composition (Kira source)

`src/std/string.kira` declares the `intrinsic def`s above and the public API as
`extend str:` methods. These bodies are pure composition — this is the layer
that exercises `find_extend_method_for_builtin` (`check.cpp:7026`) end-to-end.

```kira
extend str:
    pub def is_empty(self) -> bool          # self.len() == 0
    pub def eq(self, other: str) -> bool     # rt_str_eq
    pub def starts_with(self, prefix: str) -> bool
    pub def ends_with(self, suffix: str) -> bool
    pub def contains(self, needle: str) -> bool     # self.find(needle).is_some()
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

Composition sketches (all UTF-8-correct because offsets are on scalar
boundaries and slices are byte-offset views):

- **`starts_with(prefix)`** → `self.len() >= prefix.len()` and
  `rt_str_eq(self[0..prefix.len()], prefix)`. One anchored `memcmp`, no scan.
- **`ends_with(suffix)`** → `self.len() >= suffix.len()` and
  `rt_str_eq(self[self.len()-suffix.len()..self.len()], suffix)`.
- **`find`/`rfind`** → wrap the intrinsic, mapping the sentinel to `@none`.
- **`split(sep)`** → loop `find` from a running byte offset; each piece is the
  **zero-copy** sub-slice `self[start..idx]`; advance `start = idx + sep.len()`.
  Empty `sep` returns `[self]` (no zero-width matches). Whole operation is
  O(n) search + O(pieces) header allocations, no content copying.
- **`replace`/`trim*`/`to_uppercase`/`to_lowercase`/`reversed`** → one intrinsic
  call each.

`eq` is provided because the `==` operator still has no `str` codegen; wiring
`==`/`!=` on `str` to dispatch to `rt_str_eq` in the checker+lowerer is a clean
optional follow-up, out of scope here.

## Method semantics

- **`is_empty()`** → `true` iff byte length is 0.
- **`eq(other)`** → `true` iff identical byte length and content.
- **`starts_with(prefix)` / `ends_with(suffix)`** → substring anchored at
  start/end; empty affix ⇒ `true`; affix longer than `self` ⇒ `false`.
- **`contains(needle)`** → `true` iff `needle` occurs anywhere; empty needle ⇒
  `true`.
- **`find(needle)`** → `@some(byte_offset)` of first occurrence, else `@none`;
  empty needle ⇒ `@some(0)`. Offset is a scalar boundary.
- **`rfind(needle)`** → `@some(byte_offset)` of last occurrence, else `@none`;
  empty needle ⇒ `@some(self.len())`.
- **`trim` / `trim_start` / `trim_end`** → remove leading/trailing scalars with
  the Unicode `White_Space` property (not just ASCII); returns a zero-copy
  sub-slice; returns `self` unchanged when nothing trims.
- **`reversed()`** → scalars emitted in reverse order (valid UTF-8 out).
- **`to_uppercase()` / `to_lowercase()`** → Unicode simple case mapping applied
  per scalar; unmapped scalars pass through.
- **`replace(from, to)`** → all non-overlapping occurrences of `from` replaced
  with `to`; empty `from` ⇒ `self` unchanged (no zero-width matches).
- **`split(sep)`** → pieces between occurrences of `sep`; consecutive separators
  yield empty pieces (`"a::b".split(":")` → `["a", "", "b"]`); empty `sep` ⇒
  `[self]`; no occurrence ⇒ `[self]`.

## Compiler integration

1. **Remove the vaporware entries** from `builtin_method_result`'s `str` case
   (`check.cpp:6577-6596`), keeping only `len` and `as_bytes` (the two with real
   lowering, `lower.cpp:942-967`). Removing `contains`, `starts_with`,
   `ends_with`, `is_empty`, `to_uppercase`, `to_lowercase`, `replace`, `trim`,
   `reversed`, `split` makes `builtin_method_result` return `k_unknown_type`,
   which is exactly the condition (`check.cpp:7012`) that lets the checker fall
   through to `find_extend_method_for_builtin` and discover this module's real
   `extend str` methods.

2. **Register the new intrinsics** in the single-source-of-truth table
   `src/intrinsics.h` — add the 8 names to `known_intrinsic_names` and their
   arities to `known_intrinsic_arities` (bump both `std::array` sizes from 23).
   Then wire the three consumers that read that table: the VM's native dispatch
   (`src/bytecode/vm.cpp`), the LLVM `declare` list (`src/llvm_codegen/`), and
   the runtime implementations (`src/runtime/string.cpp`/`.h`, linked into both
   the bytecode runtime and the AOT archive set in `src/driver/aot.cpp`).

3. **Ship and inject the module.** Add `src/std/string.kira`, add
   `"string.kira"` to the injection list in `inject_stdlib_prelude`
   (`src/driver/driver.cpp:237-239`, after `fmt.kira`), register it in the
   relevant Bazel `filegroup`/package rules so it lands under
   `share/kira/std/`, and confirm `find_stdlib_source_file` resolves it. Without
   this the `extend str` methods are never in the session and the fallback finds
   nothing.

4. **Add the Unicode case table** as a generated `.inc` (build genrule from
   checked-in Unicode data) or a vendored generated header, with a `//src/utf8`
   dependency on the runtime string target.

## Algorithms and complexity

- **Search (`find`/`rfind`/`contains`/`split`/`replace`):**
  **Two-Way string matching** (the Crochemore–Perrin algorithm — glibc
  `memmem`'s and Rust `str::find`'s choice): O(n + m) worst case, O(1) extra
  space, no precomputed table proportional to the alphabet. Byte-level and
  UTF-8-correct by self-synchronization. This choice is isolated to the two
  search intrinsics and can change without touching Layer 2. (The runtime may
  delegate to the platform `memmem` where one is available and known to be
  Two-Way-class; otherwise it uses a vendored Two-Way implementation so the
  linear worst-case guarantee holds on every target.)
- **`eq`/`starts_with`/`ends_with`:** O(min length) `memcmp` after an O(1)
  length check; no allocation (affixes are zero-copy slices).
- **`split`:** O(n) total search; pieces are O(1) zero-copy sub-slices.
- **`to_uppercase`/`to_lowercase`/`reverse`/`replace`:** single decode/encode
  pass into one length-`reserve`d buffer — O(n), one allocation, no O(n²)
  concatenation.
- **`trim*`:** O(leading+trailing) scalar scan; result is a zero-copy sub-slice.

## Limitations

- **Simple case mapping only.** `to_uppercase`/`to_lowercase` apply Unicode 1:1
  simple mappings. Full/special casing (`ß`→`SS`, final-sigma, locale-tailored
  Turkish/Azeri/Lithuanian rules) and case *folding* for caseless comparison are
  out of scope for v1. `eq`/search are code-point-exact, not
  case-insensitive or normalization-aware.
- **No normalization.** Canonically-equivalent but differently-composed strings
  (e.g. `é` as U+00E9 vs `e`+U+0301) compare unequal and search independently.
  NFC/NFD normalization is a separate future module.
- **Scalar-level, not grapheme-level.** `reversed` reverses scalars; `find`
  offsets are scalar boundaries, not grapheme boundaries. A combining mark can
  be separated from its base by `reversed` or land mid-grapheme in a `split`
  piece. Grapheme segmentation (UAX #29) is out of scope.
- **`str` methods only.** This module extends `str`; no `char`-classification
  API (`is_alphabetic`, etc.) here.
- **No regex.** `split`/`replace` take literal substrings only.

## Testing

Fixtures in `src/testdata/std_test/`, each with ASCII, multi-byte UTF-8, and
edge cases (empty string, empty needle/sep, no match, needle longer than
haystack). Every case is checked on **both** backends (bytecode VM and AOT) via
the existing `std_test` harness.

- `string_eq.kira` — equality incl. differing-length and multi-byte content.
- `string_starts_ends_contains.kira` — affix/substring detection, multi-byte
  affixes, empty affixes.
- `string_find.kira` — forward/backward byte offsets; multi-byte needles;
  offsets verified to fall on scalar boundaries; not-found → `@none`.
- `string_split.kira` — consecutive separators, multi-byte separator, empty sep,
  no-occurrence; pieces reassemble to the original.
- `string_trim.kira` — ASCII and Unicode `White_Space` (e.g. U+00A0, U+2003);
  no-op case returns input.
- `string_case.kira` — ASCII plus Greek/Cyrillic simple case mapping; non-cased
  scalars pass through; a documented full-casing case (`ß`) asserts the v1
  simple-mapping behavior.
- `string_replace.kira` — multiple/adjacent matches, empty `from`, multi-byte
  `from`/`to`, growth and shrink.
- `string_reversed.kira` — multi-byte scalars reverse to valid UTF-8; a
  combining-mark case documents the scalar-level (non-grapheme) behavior.

## Appendix A — `src/intrinsics.h` change

Eight names and arities added; both `std::array` sizes go 23 → 31. Insert the
names after the `rt_fmt_*` block (before the `std.platform` group) and the
arities at the matching position, so index order stays consistent across the
name table, the arity table, the VM dispatch table, and the LLVM `declare`
list.

```diff
-inline constexpr std::array<std::string_view, 23> known_intrinsic_names = {{
+inline constexpr std::array<std::string_view, 31> known_intrinsic_names = {{
@@ after "rt_fmt_char_from_codepoint", @@
     "rt_fmt_char_from_codepoint",
+    // `std.string` runtime intrinsics (spec/std-string.md). All operate on
+    // UTF-8 `str` values ({len; data_ptr}); substring search is Two-Way
+    // (linear worst case) over bytes, scalar-correct by UTF-8 self-
+    // synchronization. `to_upper`/`to_lower` apply Unicode simple (1:1) case
+    // mapping via a generated table (src/runtime/unicode_case_table.inc).
+    "rt_str_eq",
+    "rt_str_find",
+    "rt_str_rfind",
+    "rt_str_to_upper",
+    "rt_str_to_lower",
+    "rt_str_reverse",
+    "rt_str_trim",
+    "rt_str_replace",
     // `std.platform` runtime introspection intrinsics (spec/std-platform.md).
     "rt_uname",
@@ arities array (same 23 -> 31, values aligned to the names above) @@
-inline constexpr std::array<uint8_t, 23> known_intrinsic_arities = {{
+inline constexpr std::array<uint8_t, 31> known_intrinsic_arities = {{
     1, // rt_fmt_char_from_codepoint
+    2, // rt_str_eq        (a, b)
+    3, // rt_str_find      (haystack, needle, from)
+    2, // rt_str_rfind     (haystack, needle)
+    1, // rt_str_to_upper  (s)
+    1, // rt_str_to_lower  (s)
+    1, // rt_str_reverse   (s)
+    2, // rt_str_trim      (s, mode)
+    3, // rt_str_replace   (s, from, to)
     0, // rt_uname
```

The three downstream consumers then need matching entries, all keyed by the
same index:
- **VM dispatch** (`src/bytecode/vm.cpp`): 8 native handlers calling the new
  `kira_rt_str_*` symbols.
- **AOT `declare`** (`src/llvm_codegen/`): reads `known_intrinsic_arities`, so
  the LLVM signatures come for free once the arities are added; only the link
  set in `src/driver/aot.cpp` needs the new `src/runtime:string` archive (and
  its `//src/utf8` dep).
- **Runtime impls** (`src/runtime/string.cpp` + `string.h`): the eight
  `kira_rt_str_*` functions, using `view_of`/`make_str`/`make_box` exactly like
  `src/runtime/fmt.cpp`, plus a `make_slots(2)` helper for `find_result`.

## Appendix B — `src/std/string.kira` (draft)

Do not add this to `inject_stdlib_prelude` until the intrinsics above are
registered — an injected module with an unrecognized `intrinsic def` fails
`intrinsic`-name validation and breaks every compile.

```kira
module std.string

# Single-field wrappers so scalars stay heap-representable across the intrinsic
# ABI boundary (same convention as std.fmt's box_* types / std.io's raw_fd).
# Each module declares the boxes it needs.
type box_usize = { v: usize }
type box_bool = { v: bool }
type box_u8 = { v: uint8 }

# Substring-search result. `pos` is a byte offset (always on a scalar
# boundary); it is meaningful only when `found` is true. Avoids a magic
# not-found sentinel value.
type find_result = { found: bool, pos: usize }

# --- Layer 1: native intrinsics (src/runtime/string.cpp) ---
# Search is Two-Way (linear worst case) over UTF-8 bytes, scalar-correct by
# self-synchronization. Case mapping is Unicode simple (1:1). trim `mode`:
# 0 = both, 1 = start, 2 = end. See spec/std-string.md.
intrinsic def rt_str_eq(a: str, b: str) -> box_bool
intrinsic def rt_str_find(haystack: str, needle: str, from: box_usize) -> find_result
intrinsic def rt_str_rfind(haystack: str, needle: str) -> find_result
intrinsic def rt_str_to_upper(s: str) -> str
intrinsic def rt_str_to_lower(s: str) -> str
intrinsic def rt_str_reverse(s: str) -> str
intrinsic def rt_str_trim(s: str, mode: box_u8) -> str
intrinsic def rt_str_replace(s: str, from: str, to: str) -> str

# --- Layer 2: public API (thin composition over the intrinsics) ---
extend str:
    pub def is_empty(self) -> bool:
        return self.len() == 0

    pub def eq(self, other: str) -> bool:
        return rt_str_eq(self, other).v

    pub def starts_with(self, prefix: str) -> bool:
        let plen = prefix.len()
        if self.len() < plen:
            return false
        return rt_str_eq(self[0 .. plen], prefix).v

    pub def ends_with(self, suffix: str) -> bool:
        let slen = suffix.len()
        let n = self.len()
        if n < slen:
            return false
        return rt_str_eq(self[n - slen .. n], suffix).v

    pub def find(self, needle: str) -> option[usize]:
        let r = rt_str_find(self, needle, box_usize { v: 0 })
        if r.found:
            return @some(r.pos)
        return @none

    pub def rfind(self, needle: str) -> option[usize]:
        let r = rt_str_rfind(self, needle)
        if r.found:
            return @some(r.pos)
        return @none

    pub def contains(self, needle: str) -> bool:
        return rt_str_find(self, needle, box_usize { v: 0 }).found

    pub def trim(self) -> str:
        return rt_str_trim(self, box_u8 { v: 0 })

    pub def trim_start(self) -> str:
        return rt_str_trim(self, box_u8 { v: 1 })

    pub def trim_end(self) -> str:
        return rt_str_trim(self, box_u8 { v: 2 })

    pub def reversed(self) -> str:
        return rt_str_reverse(self)

    pub def to_uppercase(self) -> str:
        return rt_str_to_upper(self)

    pub def to_lowercase(self) -> str:
        return rt_str_to_lower(self)

    pub def replace(self, from: str, to: str) -> str:
        return rt_str_replace(self, from, to)

    # Each piece is a zero-copy byte-offset sub-slice; total work is O(n)
    # search + O(pieces) header allocations, no content copying.
    pub def split(self, sep: str) -> list[str]:
        var pieces: list[str] = []
        if sep.len() == 0:
            pieces.push(self)
            return pieces
        let n = self.len()
        var start: usize = 0
        while true:
            let r = rt_str_find(self, sep, box_usize { v: start })
            if not r.found:
                pieces.push(self[start .. n])
                return pieces
            pieces.push(self[start .. r.pos])
            start = r.pos + sep.len()
```
