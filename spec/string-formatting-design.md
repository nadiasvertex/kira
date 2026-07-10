# String Interpolation Formatting — Kira Language Design

> Supersedes the `format_with`/single-`format_spec` sketch previously in
> `spec/stdlib.md` ("`std.format`") and the earlier, now-deleted
> `spec/kira-string-formatting-design.md`. This document is authoritative for
> `std.format`'s contents; `spec/stdlib.md` only states what stays true of
> the module boundary and points here for the rest.

## Design Goals

1. **Zero-cost by default.** Formatting a primitive with a literal spec
   compiles to direct code — no runtime parsing of the spec string, no heap
   allocation beyond the result, no hidden virtual calls.
2. **Compile-time checked, never a surprise panic.** Requesting hex
   formatting (`x`) on a `str`, or `.precision` on an `int32`, is a compile
   error with a helpful message. Whether a type supports a given format
   style is decided by ordinary trait-bound resolution — the same mechanism
   that already rejects `T: eq` when `T` has no `eq` impl — not by a runtime
   fallback that can panic. See "Why per-capability traits" below for why
   this is a hard requirement, not just a preference.
3. **Python-f-string expressiveness.** Any expression can be embedded
   directly: `"{user.name}"`, `"{a + b}"`, `"{items[i]}"`. There is no
   separate argument list to keep in sync with the template — the
   expression *is* the argument, exactly as it works today
   (`spec/kira-reference.md`, "Strings").
4. **Self-documenting values, Python-3.8-style.** `"{total=}"` renders as
   `"total=42"` — the source text of the expression, `=`, then its
   formatted value. Useful for quick debug prints without hand-writing the
   label.
5. **Extensible via small, focused traits.** User-defined types opt into
   each formatting capability (`show`, `debug`, `hex`, `octal`, `binary`)
   independently. A type that implements only `show` still works everywhere
   a default-style interpolation is used.
6. **No ambiguity with the rest of the language.** `:` already separates a
   named call argument from its value (`greet("Bob", loud: true)`,
   `spec/kira-reference.md:220-227`) and never appears as a bare top-level
   token inside an `expr` production (`spec/kira-grammar.ebnf`) — so reusing
   a single `:` as the format-spec separator inside `{...}` introduces no
   new ambiguity. (See "Why a single `:`" for the full argument.)

---

## Syntax

```kira
let name  = "Alice"
let total = 1234.5
let pi    = 3.1415926535

let a = "{name}"              # direct embedding — today's behavior, unchanged
let b = "{total :.2f}"        # embedding + format spec
let c = "{total=}"            # self-documenting: "total=1234.5"
let d = "{total=:.2f}"        # self-documenting + spec: "total=1234.50"
```

Grammar (see also `spec/kira-grammar.ebnf`, `INTERP_EXPR`/`FORMAT_SPEC`):

```
INTERP_EXPR = "{" expr [ "=" ] [ ":" format_spec ] "}" ;
```

- `expr` is any expression, exactly as today — no new argument-passing
  mechanism. (The previous proposal considered a separate named/positional
  argument list, Rust-`format!`-style; that was deliberately dropped in
  favor of keeping the existing direct-embedding model, per design
  discussion — see "Why not external named arguments" below.)
- A trailing `=` (before the optional `:`) requests the self-documenting
  form: the compiler captures `expr`'s exact source span as a string
  literal and renders `"<source-text>=<formatted-value>"`.
- `:` introduces the format spec, described next. It is a **compile-time
  constant** — never an expression — parsed by the compiler into a typed
  `format_spec` value, same as the rejected proposal's approach (that part
  was sound).

Empty spec still means "use `show()`":

```kira
let a = "{name}"        # equivalent —
let b = "{name :}"      # empty spec means "use show()"
```

---

## Format Specification Grammar

```
format_spec := [fill_align] [sign] [#] [0] [width] [.precision] [type]
fill_align  := [fill_char] (< | > | ^)
sign        := + | - | ' '
width       := decimal_integer | {expr}
precision   := decimal_integer | {expr}
type        := s | ? | d | x | X | o | b | e | E | f | g | G | c
fill_char   := any character except { } : = \
```

Two corrections relative to the earlier (rejected) proposal:

- **`sign` now has three values, not two: `+`, `-`, and `' '` (space).**
  The old proposal's `format_spec.sign_mode` runtime type had an `@space`
  variant with no spec syntax that could ever produce it — a dead enum
  case. `' '` now means "reserve a column for the sign: `-` for negative,
  a literal space for non-negative" (matches Python's `' '` sign flag).
- **`.precision` on the default/`s` string style truncates to N Unicode
  scalar values.** The old proposal left this undefined. `"{name :.3}"`
  with `name = "Alice"` renders `"Ali"`.
- `{expr}` in `width`/`precision` is unchanged from the old proposal: an
  ordinary runtime expression, evaluated and substituted; it must produce
  `usize` or it's a compile error.
- `0` before `width` is **not** sugar for `fill_align` `0>`. It is its own
  flag (`format_spec.zero_pad: bool`) with dedicated sign/prefix-aware
  padding semantics — see "Padding, alignment, and zero-padding" below. (The
  old proposal's claim that `0` is "shorthand for `fill_align` of `0>`" is
  incorrect once a sign or `#`-prefix is in play; this is the single most
  concrete correctness fix in this revision.)

---

## Format Spec Reference

| Component | Meaning | Example | Output |
|---|---|---|---|
| *(none)* | Default `show()` representation | `{"hello"}` | `hello` |
| `s` | String (same as default) | `{"hello" :s}` | `hello` |
| `?` | Debug representation (`debug` trait) | `{p :?}` | `point { x: 1.0, y: 2.0 }` |
| `<` | Left-align | `{"hi" :<5}` | `"hi   "` |
| `>` | Right-align | `{"hi" :>5}` | `"   hi"` |
| `^` | Center-align | `{"hi" :^5}` | `" hi  "` |
| `+` | Always show sign | `{-5 :+}` → `-5`; `{5 :+}` | `+5` |
| `-` | Sign only if negative (default) | `{-5 :-}` → `-5`; `{5 :-}` | `5` |
| `' '` | Reserve column for sign | `{5 : }` | `" 5"` (leading space) |
| `#` | Alternate form | `{255 :#x}` | `0xff` |
| `0` | Zero-pad (sign/prefix-aware) | `{-42 :06}` | `-00042` |
| `width` | Minimum field width | `{42 :6}` | `"    42"` |
| `.prec` (str) | Truncate to N scalar values | `{"hello" :.3}` | `hel` |
| `.prec` (float) | Digits after the decimal point | `{3.14159 :.2f}` | `3.14` |
| `d` | Decimal integer *(builtin only)* | `{255 :d}` | `255` |
| `x` | Hex, lowercase (`hex` trait) | `{255 :x}` | `ff` |
| `X` | Hex, uppercase (`hex` trait, case-flipped) | `{255 :X}` | `FF` |
| `o` | Octal (`octal` trait) | `{255 :o}` | `377` |
| `b` | Binary (`binary` trait) | `{255 :b}` | `11111111` |
| `e` / `E` | Scientific *(builtin only)* | `{1234.0 :.2e}` | `1.23e3` |
| `f` | Fixed-point *(builtin only)* | `{3.14159 :.2f}` | `3.14` |
| `g` / `G` | General, shorter of `f`/`e` *(builtin only)* | `{1234.0 :g}` | `1234` |
| `c` | Character from codepoint *(builtin only)* | `{65 :c}` | `A` |

`d | e | E | f | g | G | c` are marked **builtin only**: they are satisfied
exclusively by the compiler's intrinsic formatting for the built-in numeric
and `char` types, never by a user trait impl. See "Why builtin-only numeric
styles" below for the reasoning; the short version is that these are
inherently tied to native float/integer/codepoint representations, and
scoping them out keeps the trait surface small (design goal 5) without
losing anything a user type would plausibly want (a user type that wants
`f`-style precision-aware rendering can implement `show` and do it itself).

---

## Traits

Five traits, each a single `def <name>(self) -> str` method — the same
shape as the existing `show` trait, so each is independently usable as a
`box[trait]` object with no new object-safety rules
(`spec/kira-reference.md:1078-1097` already establishes `box[show]` works;
the same reasoning applies unchanged to `box[debug]`, `box[hex]`, etc.).

### `show` (existing, unchanged)

```kira
trait show:
    def show(self) -> str
```

Already implemented (`src/std/traits.kira:21-22`). Used for the default
(no type char) and explicit `s` styles. Every embedded expression that
doesn't request `?`/`x`/`X`/`o`/`b` must satisfy `T: show`.

### `debug` (new)

```kira
trait debug:
    def debug(self) -> str
```

Used for `?`. Add `debug` to the semantic checker's derivable-trait
allowlist (currently `eq, ord, show, hash` — `src/semantic/check.cpp:5847`)
so `deriving show, debug` works. A derived `debug` renders
`type_name { field1: value1.debug(), field2: value2.debug(), ... }`,
recursively — the same shape `kira-reference.md`'s `derive_show` example
already builds for `show` (`kira-reference.md:1422-1436`), just calling
`.debug()` on each field instead of `.show()`.

```kira
type point = { x: float64, y: float64 }
    deriving show, debug

let p = point { x: 1.0, y: 2.0 }
println("{p}")     # (1.0, 2.0) — via show
println("{p :?}")  # point { x: 1.0, y: 2.0 } — via debug
```

### `hex`, `octal`, `binary` (new)

```kira
trait hex:
    def hex(self) -> str      # lowercase digit string, no "0x" prefix, no sign

trait octal:
    def octal(self) -> str    # digit string, no "0o" prefix, no sign

trait binary:
    def binary(self) -> str   # digit string, no "0b" prefix, no sign
```

Each returns the **unsigned digit string only** — no sign, no `#`-prefix,
no padding. The compiler-generated glue (see "Desugaring") supplies sign,
prefix, and padding uniformly via `std.fmt.pad_integral`, so an
implementation never has to think about spec handling at all:

```kira
type flags = { bits: uint16 }

impl binary for flags:
    def binary(self) -> str: bits_to_binary_str(self.bits)

let f = flags { bits: 0b1010_0110 }
println("{f :#010b}")   # "0b10100110"
```

`X` (uppercase hex) is **not** a separate trait. Rust's `LowerHex`/
`UpperHex` split exists so a type can render hex digits specially in each
case; Kira trades that flexibility for a smaller trait surface (design goal
5, "beginner friendly") — the compiler uppercases the ASCII result of
`hex()` for the `X` style. This is a deliberate simplification; if a type
ever needs genuinely different uppercase digit shapes, it can still
implement `show`/`?` and format itself by hand.

**No single `format(spec)` trait.** This is the central structural change
from the rejected proposal. See "Why per-capability traits, not a single
`format(spec)`" below.

---

## `std.fmt` — `format_spec` and the padding helpers

```kira
module std.fmt

pub type align_mode = @left | @right | @center

pub type sign_mode = @always | @negative_only | @space

pub type format_spec = {
    fill:      char,               # default ' '
    align:     option[align_mode], # none = type's default (left for str/show/debug, right for numeric)
    sign:      sign_mode,          # default @negative_only
    alternate: bool,                # '#' flag
    zero_pad:  bool,                # '0' flag — see pad_integral
    width:     option[usize],
    precision: option[usize],
}

pub def pad_str(s: str, spec: format_spec) -> str
pub def pad_integral(negative: bool, prefix: str, digits: str, spec: format_spec) -> str
```

### Padding, alignment, and zero-padding

There are exactly two padding code paths, and every formatting call site —
builtin or user-trait-backed — goes through one of them. This
centralization is deliberate: it is what makes correctness a
prove-it-once property instead of something every `show`/`hex`/... impl has
to get right independently (the rejected proposal asked each `format` impl
to call a shared `pad_and_align` helper itself, which an impl could simply
forget to do — and its own worked example forgot, silently dropping
alignment support for negative numeric styles).

**`pad_str(s, spec)`** — for `show`/`debug`/`hex`/`octal`/`binary` output,
and for the string type itself. Generic fill/align: pads `s` with
`spec.fill` on the side(s) `spec.align` (or the type's default) selects,
truncating first to `spec.precision` scalar values if the source is `str`
and precision is set.

**`pad_integral(negative, prefix, digits, spec)`** — for every numeric
style (`d`, `x`/`X`, `o`, `b`, `e`/`E`, `f`, `g`/`G`), builtin or via a
`hex`/`octal`/`binary` impl. This is the fix for the rejected proposal's
zero-pad bug. Composition order:

1. Determine the sign character: `-` if `negative`; else `+` if
   `spec.sign == @always`; else `' '` if `spec.sign == @space`; else
   nothing.
2. Determine the prefix: `prefix` (e.g. `"0x"`) if `spec.alternate`, else
   empty.
3. If `spec.zero_pad` **and no explicit `fill_align` was written** (the
   spec parser records this — an explicit align always wins, matching the
   grammar note that `0` is "ignored if an explicit `fill_align` is
   present"): the padding goes *between* the prefix and the digits, e.g.
   `sign + prefix + zeros + digits`, so `{-42 :06}` → `-00042` (not
   `000-42`) and `{255 :#010x}` → `0x000000ff` (not `000x000ff`).
4. Otherwise: the whole `sign + prefix + digits` string is padded as a
   unit using `spec.fill`/`spec.align`, exactly like `pad_str`.

This means a `hex`/`octal`/`binary` implementation never sees a
`format_spec` at all — it just returns digits, and `pad_integral` (called
once, by compiler-generated code, never by user code) gets sign/prefix/zero
composition right in one place for every numeric style, builtin or
user-defined.

---

## Compile-Time Checking

Every interpolation segment is checked at the point it's parsed:

1. `expr` type-checks normally, giving a static type `T`.
2. The format spec's type char selects a required capability:
   - *(none)* / `s` → `T: show`
   - `?` → `T: debug`
   - `x` / `X` → `T` is a builtin integer, **or** `T: hex`
   - `o` → `T` is a builtin integer, **or** `T: octal`
   - `b` → `T` is a builtin integer, **or** `T: binary`
   - `d` / `e` / `E` / `f` / `g` / `G` / `c` → `T` must be the matching
     builtin primitive category (no trait dispatch — see "Why builtin-only
     numeric styles")
3. An unsatisfied requirement is an ordinary unsatisfied-trait-bound
   diagnostic — the same machinery `T: show + eq` bound checking already
   uses (`spec/kira-reference.md:945-948`), not a new error class.
4. `.precision` is rejected on integer-flavored styles (`d`, `x`/`X`, `o`,
   `b`) **syntactically**, independent of whether the precision is a
   literal or a dynamic `{expr}` — the presence of a `.precision`
   component is known at parse time regardless of the value inside it.
5. A dynamic `{expr}` in `width`/`precision` must type-check as `usize`.

Diagnostics use the project's real nested `error:`/`help:`/`note:` shape
(`src/parser/diagnostic.h:498-536` — plain `<level>: <message>` header, no
bracketed code, children rendered recursively), matching the illustrative
`error[E0xxx]` convention `kira-reference.md`'s own examples already use
(e.g. `kira-reference.md`'s error-code examples at lines with `error[E0012]`,
`error[E0013]`, `error[E0021]`):

```
error[E0090]: `str` does not implement `hex`
  --> src/main.kira:14:16
   |
14 |     let bad = "{s :x}"
   |                   ^ `str` does not support hexadecimal formatting
   |
   help: `str` supports the default/`s` style (`show`) and `?` (`debug`)
   note: implement `hex for str` yourself if you want `s :x` to mean
         something specific — Kira does not derive it, since there's no
         single obvious hex rendering of arbitrary text
```

```
error[E0091]: `.precision` is not allowed with `x`
  --> src/main.kira:16:16
   |
16 |     let bad = "{42 :.2x}"
   |                    ^^ precision is only meaningful for `s`, `?`, `f`, `e`/`E`, `g`/`G`
   |
   help: for integer styles, use width and zero-padding instead: "{42 :04x}"
```

```
error[E0092]: format width must be `usize`, found `str`
  --> src/main.kira:15:20
   |
15 |     let bad = "{42 :{w}}"
   |                       ^ expected usize, found str
   |
   note: the expression inside `{...}` in a dynamic width must evaluate to `usize`
```

---

## Desugaring via Quoting and Splicing

At the syntax level, `"{name}: {total :.2f}"` desugars the same way
`deriving` is *documented* to work — quoting the pieces and splicing them
into a block expression (`spec/kira-reference.md:1399-1449`,
"Quoting and Splicing"):

```kira
static def desugar_interpolated_string(segments: list[interp_segment]) -> expr:
    let parts = for seg in segments =>
        match seg:
            @literal(text) =>
                `(std.fmt.builder.append_literal(~(expr.lit(text))))`
            @formatted(value_expr, spec) =>
                `(std.fmt.builder.append(~value_expr, ~(expr.lit(spec))))`
    `{
        let mut __b = std.fmt.builder.new()
        ~(stmt.seq(parts))
        __b.build()
    }`
```

Each `@formatted` segment's `~value_expr` is the user's original embedded
expression (hygienic — it does not capture names from the macro's own
scope, per the default hygiene rule at `kira-reference.md:1447`), and
`std.fmt.builder.append` is generic over the trait the resolved format
style needs, calling `.show()`/`.debug()`/`.hex()`/etc. as chosen during
step 2 of compile-time checking, then routing the result through
`pad_str`/`pad_integral` as described above. This is "the macro builds an
efficient formatter chain": for a literal, fully-static format spec, the
call chain monomorphizes and inlines exactly like any other generic
function call — no runtime spec parsing, matching design goal 1.

---

## Implementation Staging

This cannot literally run through the quote/splice interpreter yet, for the
same reason `deriving` — despite being described in exactly this vocabulary
in `kira-reference.md` — doesn't either: **nothing in the compiler lowers
`quote_expr`/`splice_expr` to HIR today**, and `static` bindings/`static
for` are type-checked but not evaluated (`src/semantic/check.cpp:6327` only
checks the pieces; `src/hir/lower.cpp` has no handling for any of these
node kinds). `deriving` is actually implemented as a hardcoded compiler
special case (`check.cpp:3162-3178`, `5838-5854`), not by running the
`derive_show`-style quote/splice code the reference doc shows as
illustration.

String-interpolation formatting is a **lighter lift than `deriving`**: it
needs no compile-time reflection (no `T.fields()`), only (a) splitting a
string literal into literal/expression segments — new lexer/parser work,
since today the whole string is one `string_lit` token
(`src/parser/lexer.h:946-947`, `src/parser/text_escape.h:21-24`) — and (b)
ordinary per-segment trait-bound resolution. Recommended v1, mirroring how
`deriving` actually shipped:

1. **Parser**: split each `string_lit` into a new `interpolated_string_expr`
   AST node — a sequence of literal-text segments and `(expr, self_doc,
   format_spec)` segments — instead of one opaque token. Parse
   `format_spec` text into the `std.fmt.format_spec` shape at this stage
   (it's a compile-time constant, never re-parsed later).
2. **Semantic**: resolve each embedded `expr` normally; run the
   capability check (see "Compile-Time Checking") against its static type;
   attach the resolved trait method to the segment.
3. **HIR/codegen (both backends)**: lower `interpolated_string_expr`
   directly to a sequence of `std.fmt` builder/`pad_str`/`pad_integral`
   calls — the same shape the quote/splice desugaring above describes, just
   emitted directly by the compiler instead of by an interpreted macro.

Once quote/splice and reflection are real, this v1 pass can be re-expressed
as an actual library macro without changing the surface language — the
desugaring section above is written as the target shape for exactly that
reason, not because v1 needs it.

---

## Examples

### Basic formatting

```kira
let name = "Alice"
let age  = 30
let pi   = 3.1415926535

println("Name: {name :>10}")     # "Name:      Alice"
println("Age:  {age :03}")       # "Age:  030"
println("Pi:   {pi :.2f}")       # "Pi:   3.14"
println("Hex:  {255 :04x}")      # "Hex:  00ff"
println("Bin:  {255 :08b}")      # "Bin:  11111111"
println("Sci:  {1234.0 :.2e}")   # "Sci:  1.23e3"
```

### Self-documenting

```kira
let width = 8
let total_cost = 142.50

println("{width=}")             # "width=8"
println("{total_cost=:.2f}")    # "total_cost=142.50"
```

### Dynamic width and precision

```kira
let width = 8
let prec  = 3
let val   = 12.3456

println("{val :{width}.{prec}f}")
# "  12.346"
```

### User-defined `hex`/`binary`

```kira
type flags = { bits: uint16 }

impl show for flags:
    def show(self) -> str: "flags({self.bits})"

impl hex for flags:
    def hex(self) -> str: to_hex_digits(self.bits)

let f = flags { bits: 0xBEEF }
println("{f}")            # "flags(48879)"
println("{f :#06x}")      # "0xbeef"
println("{f :x}")         # error[E0090]: `d` was never requested — this line is fine, shown for contrast
```

### Compile-time rejection

```kira
let s = "hello"
println("{s :x}")   # error[E0090]: `str` does not implement `hex`
```

---

## Design Rationale

### Why per-capability traits, not a single `format(spec)`

The rejected proposal's `format` trait took a runtime `format_spec` value
and matched on `spec.style` inside the body:

```kira
impl format for money:
    def format(self, spec: format_spec) -> str:
        match spec.style:
            @none | @string => pad_and_align(self.show(), spec)
            _ => panic("money does not support numeric format styles")
```

Two problems, both structural, not fixable by better wording: (1) there is
no static contract the compiler can check "money supports @string but not
@number" without either analyzing the body of `format` (not something Kira
does for ordinary trait methods) or hardcoding the check for built-ins
only — which is exactly what happened: the design goal promised a compile
error, but the worked example's only recourse for an unsupported style was
a **runtime `panic`**, directly contradicting design goal 2. (2) alignment
was something each `format` impl had to remember to apply by calling
`pad_and_align` itself — the `money` example does; nothing enforces that
every future impl will. Splitting into `show`/`debug`/`hex`/`octal`/
`binary` — one trait per capability — turns "does `T` support this style"
into an ordinary `T: hex`-shaped trait-bound check at the specific call
site (resolved the same way `T: show + eq` already is,
`kira-reference.md:945-948`), and moves padding entirely out of user code
into the two centralized `pad_str`/`pad_integral` helpers described above.
No runtime panic path exists for "unsupported style" — it's a compile
error, full stop.

### Why a single `:`, not `::`

The rejected proposal used `::` on the theory that a single `:` inside
`{...}` would be ambiguous with "an expression that happens to contain a
type annotation." That ambiguity doesn't exist: Kira's `expr` grammar never
produces a bare top-level `:` (`spec/kira-grammar.ebnf`'s only `:` uses are
in statement/declaration contexts — block openers, `let`/`var`
annotations, struct-literal fields, and named call arguments — never inside
a raw `expr` production). Named call arguments already use exactly this
shape, `IDENT ":" expr` (`kira-reference.md:220-227`,
`kira-grammar.ebnf:525`), so a single `:` for format specs is not a new
convention — it's the existing one. It also matches `spec/stdlib.md`'s
original (pre-this-document) sketch of `{value:>10.2}`, so this document
doesn't introduce yet a third colon convention into the project's history.

### Why not external named arguments

An alternative considered (and explicitly declined, per design discussion
during this proposal) was a Rust-`format!`-style external argument list —
`format("{a} plus {b}", a: x, b: y)` — decoupling placeholder names from
the expressions that fill them, so a translated/localized template could
reorder placeholders independent of computation order. Kira keeps direct
embedding only: every `{...}` contains the exact expression to render,
exactly as it works today. This keeps the feature surface small (one
mechanism, not two) and matches Python f-strings, which is the explicit
model this proposal targets. If reorderable, name-decoupled templates
become a real need later (e.g. for i18n string tables), that is a separate,
additive proposal — it does not need to reopen this one.

### Why builtin-only numeric styles

`d`, `e`/`E`, `f`, `g`/`G`, and `c` are inherently about a native
representation: decimal digits of an integer, IEEE-754 bit patterns, a
Unicode scalar value from a codepoint. A user-defined type that wants
`f`-style precision-aware rendering can already do so by hand inside
`show`/`debug` — there is no loss of expressiveness, only a smaller trait
surface (five traits instead of five-plus-N). This mirrors how Rust's
`Display`/`Debug` are the only traits that get real precision/width access
via a `Formatter`; the numeric radix traits (`Binary`/`Octal`/`LowerHex`/
`UpperHex`) are the only "structural" ones Rust exposes for user types, and
this document follows that same split.

---

## Cross-References

- `spec/stdlib.md`, "`std.format`" — module boundary and what `std.fmt`
  exports; points back here for the full design.
- `spec/stdlib.md`, "Compiler and Language Changes Required" — tracks the
  `{expr:spec}` interpolation-splitting work item this document depends on.
- `spec/kira-reference.md`, "Strings" — today's `{expr}` interpolation,
  unchanged by this proposal except for the new optional `=`/`:spec`
  suffixes.
- `spec/kira-reference.md`, "Quoting and Splicing" (lines 1399-1449) — the
  macro vocabulary the desugaring section above is written in.
- `spec/kira-reference.md`, "Traits" (lines 900-948) and "Trait Objects"
  (lines 1078-1097) — trait-bound and `box[trait]` mechanics this design
  reuses unchanged.
- `spec/kira-grammar.ebnf`, `INTERP_EXPR`/`FORMAT_SPEC` — the formal
  grammar, kept in sync with this document (the previous proposal did not
  update the grammar file; this one does).

---

## Summary of Additions to the Language

| Addition | Location | Description |
|---|---|---|
| `= ` / `: spec` suffixes on `{expr}` | Layer 1 — Strings | Self-documenting and format-spec syntax inside interpolation braces |
| `trait debug` | Prelude | Programmer-facing representation; add to derivable-trait allowlist |
| `trait hex`, `trait octal`, `trait binary` | Prelude | Per-radix digit-string traits for user types |
| `type format_spec`, `type align_mode`, `type sign_mode` | `std.fmt` | Compile-time-parsed descriptor and its enums |
| `pad_str(str, format_spec) -> str` | `std.fmt` | Generic fill/align/truncate |
| `pad_integral(bool, str, str, format_spec) -> str` | `std.fmt` | Sign/prefix-aware zero-padding and fill/align for numeric styles |
