# 53. `std.format`

**Status:** Implemented

Covers string interpolation's format-specification syntax, the `show`/`debug`/`hex`/`octal`/`binary` formatting traits, and `std.fmt`'s `format_spec` type and padding helpers.

## Syntax

```kira
let a = "{name}"              # direct embedding — unchanged base behavior
let b = "{total :.2f}"        # embedding + format spec
let c = "{total=}"            # self-documenting: "total=1234.5"
let d = "{total=:.2f}"        # self-documenting + spec: "total=1234.50"
```

Grammar (see `spec/kira-grammar.ebnf`, `INTERP_EXPR`/`FORMAT_SPEC`):

```
INTERP_EXPR = "{" expr [ "=" ] [ ":" format_spec ] "}" ;
```

- `expr` is any expression — the same direct-embedding model interpolation already uses; there is no separate positional/named argument list to keep in sync with the template.
- A trailing `=` (before the optional `:`) requests the self-documenting form: the compiler captures `expr`'s exact source span as a string literal and renders `"<source-text>=<formatted-value>"`.
- `:` introduces the format spec. It is a **compile-time constant**, parsed by the compiler into a typed `format_spec` value — never an expression itself, though `width`/`precision` inside it may each be a dynamic `{expr}`.
- An empty spec (`"{name :}"`) is equivalent to no spec (`"{name}"`): both mean "use `show()`".

## Format specification grammar

```
format_spec := [fill_align] [sign] [#] [0] [width] [.precision] [type]
fill_align  := [fill_char] (< | > | ^)
sign        := + | - | ' '
width       := decimal_integer | {expr}
precision   := decimal_integer | {expr}
type        := s | ? | d | x | X | o | b | e | E | f | g | G | c
fill_char   := any character except { } : = \
```

## Format spec reference

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
| `' '` | Reserve column for sign | `{5 : }` | `" 5"` |
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

`d | e | E | f | g | G | c` are **builtin only**: satisfied exclusively by intrinsic formatting for the built-in numeric and `char` types, never by a user trait impl. `X` is not a separate trait from `x` — the compiler uppercases the ASCII result of `hex()` for the `X` style.

## Traits

Five traits, each a single `def <name>(self) -> str` method — the same shape as `show`, so each is usable as a `box[trait]` object.

```kira
trait show:                  # existing (src/std/traits.kira)
    def show(self) -> str

trait debug:                 # used for `?`; derivable via `deriving show, debug`
    def debug(self) -> str

trait hex:                   # used for `x`/`X`
    def hex(self) -> str      # lowercase digit string, no "0x" prefix, no sign

trait octal:                 # used for `o`
    def octal(self) -> str    # digit string, no "0o" prefix, no sign

trait binary:                 # used for `b`
    def binary(self) -> str   # digit string, no "0b" prefix, no sign
```

All five are declared `pub` in `src/std/traits.kira`. Each of `hex`/`octal`/`binary` returns the unsigned digit string only — no sign, no `#`-prefix, no padding; the compiler-generated glue supplies sign, prefix, and padding uniformly via `pad_integral` (below), so an implementation never handles the spec itself.

A derived `debug` renders `type_name { field1: value1.debug(), field2: value2.debug(), ... }`, recursively, the same shape a derived `show` builds.

There is no single `format(spec)` trait — see "Why per-capability traits" below.

## `std.fmt`: `format_spec` and the padding helpers

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

`src/std/fmt.kira`'s `format_spec` matches this exactly, field for field.

### Padding, alignment, and zero-padding

Every formatting call site — builtin or user-trait-backed — goes through exactly one of the two padding functions:

- **`pad_str(s, spec)`** — for `show`/`debug`/`hex`/`octal`/`binary` output and for `str` itself. Pads `s` with `spec.fill` on the side(s) `spec.align` (or the type's default) selects, truncating first to `spec.precision` scalar values if the source is `str` and precision is set.

- **`pad_integral(negative, prefix, digits, spec)`** — for every numeric style (`d`, `x`/`X`, `o`, `b`, `e`/`E`, `f`, `g`/`G`), builtin or via a `hex`/`octal`/`binary` impl. Composition order:
  1. Sign character: `-` if `negative`; else `+` if `spec.sign == @always`; else `' '` if `spec.sign == @space`; else nothing.
  2. Prefix: `prefix` (e.g. `"0x"`) if `spec.alternate`, else empty.
  3. If `spec.zero_pad` and no explicit `fill_align` was written: zeros go *between* the prefix and the digits — `sign + prefix + zeros + digits` — so `{-42 :06}` → `-00042` (not `000-42`) and `{255 :#010x}` → `0x000000ff` (not `000x000ff`). An explicit align always wins over `0`.
  4. Otherwise: the whole `sign + prefix + digits` string is padded as a unit via `spec.fill`/`spec.align`, exactly like `pad_str`.

A `hex`/`octal`/`binary` implementation never sees a `format_spec` — it returns digits only, and `pad_integral` (called once, by compiler-generated code) composes sign/prefix/zero-padding for every numeric style uniformly.

### Fixed-signature entry points

`src/std/fmt.kira` exposes one function per builtin type/style combination that `hir::lower_interpolated_string` calls directly, so lowering never constructs control flow (bool → `"true"`/`"false"`, sign/magnitude splitting) by hand — it only lowers the embedded value, builds a `format_spec` literal, and calls one of these:

| Function | Purpose |
|---|---|
| `fmt_show_str(s, spec)` | `str`, default/`s` style |
| `fmt_debug_str(s, spec)` | `str`, `?` style (quoted and escaped) |
| `fmt_show_bool(b, spec)` | `bool`, default style (`"true"`/`"false"`) |
| `fmt_show_char(c, spec)` / `fmt_char_codepoint(cp, spec)` | `char` / codepoint, `c` style |
| `fmt_show_i64(v, spec)` / `fmt_show_u64(v, spec)` | signed/unsigned 64-bit, decimal |
| `fmt_radix_i64(v, radix, uppercase, prefix, spec)` / `fmt_radix_u64(...)` | signed/unsigned 64-bit, arbitrary radix (backs `x`/`X`/`o`/`b`) |
| `fmt_float_fixed(v, precision, spec)` | `f` style |
| `fmt_float_sci(v, precision, uppercase, spec)` | `e`/`E` style |
| `fmt_float_general(v, precision, spec)` | `g`/`G` style |

These sit on five `rt_fmt_*`/`rt_str_*` intrinsics (`rt_fmt_radix_digits`, `rt_fmt_f64_fixed`, `rt_fmt_f64_sci`, `rt_fmt_f64_general`, `rt_fmt_char_from_codepoint`, plus `rt_str_len_scalars`/`rt_str_repeat_char`/`rt_str_truncate_scalars`/`rt_str_concat` for the padding helpers), listed in the single shared intrinsic table `src/intrinsics.h`.

## Compile-time checking

Every interpolation segment is checked at the point it's parsed:

1. `expr` type-checks normally, giving a static type `T`.
2. The format spec's type char selects a required capability:
   - *(none)* / `s` → `T: show`
   - `?` → `T: debug`
   - `x` / `X` → `T` is a builtin integer, **or** `T: hex`
   - `o` → `T` is a builtin integer, **or** `T: octal`
   - `b` → `T` is a builtin integer, **or** `T: binary`
   - `d` / `e` / `E` / `f` / `g` / `G` / `c` → `T` must be the matching builtin primitive category (no trait dispatch)
3. An unsatisfied requirement is an ordinary unsatisfied-trait-bound diagnostic, the same machinery a `T: show + eq` bound check uses.
4. `.precision` is rejected on integer-flavored styles (`d`, `x`/`X`, `o`, `b`) syntactically, independent of whether the precision is a literal or a dynamic `{expr}`.
5. A dynamic `{expr}` in `width`/`precision` must type-check as `usize`.

## Desugaring

`"{name}: {total :.2f}"` desugars via quoting and splicing into a block expression that builds through `std.fmt.builder`:

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

Each `@formatted` segment's `~value_expr` is the user's original embedded expression (hygienic — it does not capture names from the macro's own scope). `std.fmt.builder.append` is generic over the trait the resolved format style needs, calling `.show()`/`.debug()`/`.hex()`/etc. as chosen during compile-time checking, then routing the result through `pad_str`/`pad_integral`. For a literal, fully-static format spec, the call chain monomorphizes and inlines — no runtime spec parsing.

## Example

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

```kira
type flags = { bits: uint16 }

impl show for flags:
    def show(self) -> str: "flags({self.bits})"

impl hex for flags:
    def hex(self) -> str: to_hex_digits(self.bits)

let f = flags { bits: 0xBEEF }
println("{f}")            # "flags(48879)"
println("{f :#06x}")      # "0xbeef"
```

## Errors

Diagnostics use the project's nested `error:`/`help:`/`note:` shape:

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

## Why per-capability traits, not a single `format(spec)`

An earlier design used a single `format` trait taking a runtime `format_spec` and matching on `spec.style` inside the body. Two structural problems ruled it out: (1) there is no static contract the compiler can check ("this type supports style A but not style B") without either analyzing the method body or hardcoding the check for built-ins only — so the promised compile error degraded to a runtime `panic` for unsupported styles. (2) alignment became something each `format` impl had to remember to apply itself, with no enforcement. Splitting into `show`/`debug`/`hex`/`octal`/`binary` turns "does `T` support this style" into an ordinary `T: hex`-shaped trait-bound check at the call site, and moves padding entirely into the two centralized `pad_str`/`pad_integral` helpers. No runtime panic path exists for an unsupported style — it is a compile error.

## Implementation status

Both backends share the `rt_fmt_*`/`rt_str_*` intrinsics that back this chapter, via the single dispatch table `src/intrinsics.h`; confirmed present in `src/runtime/fmt.{h,cpp}` and `src/bytecode/vm.cpp`, and reachable from `src/llvm_codegen/codegen.cpp` through the same generic intrinsic-declaration path every intrinsic uses (unlike `rt_str_eq`, `rt_fmt_*` needs no LLVM-side special-casing). `println`/`print`/`eprintln`/`eprint` and the interpolation/format-spec surface described here run end-to-end on both the bytecode VM and LLVM/AOT tiers. `format_spec` and the padding helpers in `src/std/fmt.kira` match this chapter's reference exactly, field for field and function for function.

## See also

- [`std.string`](52-std-string.md) — `str`'s scalar-length/truncation/concatenation intrinsics used by `pad_str`.
