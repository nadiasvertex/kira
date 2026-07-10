# String Interpolation Formatting — Kira Language Design

## Design Goals

1. **Zero-cost by default.** Formatting a primitive with a literal spec is compiled to direct code — no runtime parsing, no heap allocation for the spec string, no hidden virtual calls.
2. **Compile-time checked.** Using `x` (hex) on a `str`, or requesting 500 digits of precision on an `int32`, is a compile error with a helpful message, not a runtime panic or silent truncation.
3. **Beginner-friendly.** Common cases look like Python/Rust: `{:08x}`, `{:.2f}`, `{:>10}`.
4. **Extensible.** User-defined types opt into formatting by implementing traits; the compiler validates that the requested style is supported.
5. **No ambiguity with the rest of the language.** The syntax must not collide with Kira's existing use of `:` for blocks, type annotations, or match arms, nor with `|` for bitwise OR.

---

## Syntax

Inside a string interpolation brace, an optional format specification follows a **double colon** `::`:

```kira
let msg = "value = {value :: 08x}"
let pi  = "{pi :: .2f}"
let hdr = "{title :: ^20}"
```

`::` is chosen because it is visually separating and unambiguous: Kira does not use `::` anywhere else in its syntax. (`:` is used for type annotations and block starters; `|` is bitwise OR; `->` is for return types and lambdas; `=>` is for match arms and lambdas; `~` is for compile-time splicing.)

A format spec is a compile-time string. It is **not** an expression — it is parsed by the compiler into a typed `format_request` value.

```kira
# These are equivalent — empty spec means "use show()"
let a = "{name}"
let b = "{name :: }"
```

---

## Format Specification Grammar

```
spec        := [fill_align][sign][#][0][width][.precision][type]
fill_align  := [fill_char](< | > | ^)
sign        := + | -
width       := decimal_integer | {expr}
precision   := decimal_integer | {expr}
type        := s | d | x | X | o | b | e | E | f | g | G | c | ?
fill_char   := any character except { } | \
```

**Notes:**
- `0` before width is shorthand for `fill_align` of `0>` (zero-pad, right-align). It is ignored if an explicit `fill_align` is present.
- `{expr}` in `width` or `precision` evaluates the expression at runtime and substitutes its value. The expression must yield a `usize`.

---

## Format Spec Reference

| Component | Meaning | Example | Output (for given input) |
|---|---|---|---|
| *(none)* | Default `show()` representation | `{"hello"}` | `hello` |
| `s` | String (same as default) | `{"hello" :: s}` | `hello` |
| `?` | Debug representation | `{p :: ?}` | `point { x: 1.0, y: 2.0 }` |
| `<` | Left-align | `{"hi" :: <5}` | `hi   ` |
| `>` | Right-align | `{"hi" :: >5}` | `   hi` |
| `^` | Center-align | `{"hi" :: ^5}` | ` hi  ` |
| `+` | Always show sign | `{-5 :: +}` | `-5`; `{5 :: +}` | `+5` |
| `-` | Sign only for negative (default) | `{-5 :: -}` | `-5`; `{5 :: -}` | `5` |
| `#` | Alternate form | `{255 :: #x}` | `0xff` |
| `0` | Zero-pad to width | `{42 :: 04}` | `0042` |
| `width` | Minimum field width | `{42 :: 6}` | `    42` |
| `.prec` | Precision | `{3.14159 :: .2f}` | `3.14` |
| `d` | Decimal integer | `{255 :: d}` | `255` |
| `x` | Hexadecimal, lowercase | `{255 :: x}` | `ff` |
| `X` | Hexadecimal, uppercase | `{255 :: X}` | `FF` |
| `o` | Octal | `{255 :: o}` | `377` |
| `b` | Binary | `{255 :: b}` | `11111111` |
| `e` | Scientific, lowercase | `{1234.0 :: .2e}` | `1.23e3` |
| `E` | Scientific, uppercase | `{1234.0 :: .2E}` | `1.23E3` |
| `f` | Fixed-point | `{3.14159 :: .2f}` | `3.14` |
| `g` | General (shorter of `f`/`e`) | `{1234.0 :: g}` | `1234` |
| `G` | General uppercase | `{1234.0 :: G}` | `1234` |
| `c` | Character (from integer codepoint) | `{65 :: c}` | `A` |

---

## Traits

Three traits govern formatting. All are in the prelude.

### `show`

Already defined. Produces the user-facing string representation.

```kira
trait show:
    def show(self) -> str
```

### `debug`

Produces a programmer-facing representation, typically including type names and field values. Derivable.

```kira
trait debug:
    def debug(self) -> str
```

```kira
type point = { x: float64, y: float64 }
    deriving show, debug

let p = point { x: 1.0, y: 2.0 }
println("{p}")       # user: (1.0, 2.0) — via show
println("{p :: ?}")  # debug: point { x: 1.0, y: 2.0 } — via debug
```

### `format`

For types that need fine-grained control over padding, alignment, and type-specific styles. The compiler parses the spec at compile time and passes it as a value.

```kira
type align_mode    = @left | @right | @center
type sign_mode     = @always | @negative_only | @space
type number_style  = @decimal | @hex | @upper_hex | @octal | @binary
type float_style   = @fixed | @scientific | @general
type format_style  =
    | @string
    | @debug
    | @number(number_style)
    | @float(float_style)
    | @char

type format_spec = {
    fill:      char,           # default ' '
    align:     align_mode,      # default @right for numbers, @left for strings
    sign:      sign_mode,       # default @negative_only
    alternate: bool,            # # flag
    zero_pad:  bool,            # 0 flag (redundant with fill='0', align=@right)
    width:     option[usize],
    precision: option[usize],
    style:     option[format_style],
}

trait format:
    def format(self, spec: format_spec) -> str
```

**Default implementations.** A type that implements `show` automatically gets a default `format` that handles `style: @none` and `style: @string` by calling `show()` and then applying padding/alignment. A type that implements `debug` gets a default for `style: @debug`. If a spec requests a style the type does not support, the compiler rejects it at compile time — there is no runtime fallback.

---

## Examples

### Basic formatting

```kira
let name = "Alice"
let age  = 30
let pi   = 3.1415926535

println("Name: {name :: >10}")          # "Name:      Alice"
println("Age:  {age :: 03}")            # "Age:  030"
println("Pi:   {pi :: .2f}")           # "Pi:   3.14"
println("Hex:  {255 :: 04x}")          # "Hex:  00ff"
println("Bin:  {255 :: 08b}")          # "Bin:  11111111"
println("Sci:  {1234.0 :: .2e}")       # "Sci:  1.23e3"
```

### Debug formatting

```kira
type config = { host: str, port: int32 }
    deriving debug

let cfg = config { host: "localhost", port: 8080 }
println("{cfg :: ?}")
# config { host: "localhost", port: 8080 }
```

### Dynamic width and precision

```kira
let width = 8
let prec  = 3
let val   = 12.3456

println("{val :: {width}.{prec}f}")
# Equivalent at runtime to: val formatted with width=8, precision=3, fixed-point
# Output: "  12.346"
```

The inner `{width}` and `{prec}` are ordinary interpolation expressions evaluated at runtime. They must produce `usize`. Non-`usize` types are a compile error.

### User-defined formatting

```kira
type money = { cents: int64 }

impl show for money:
    def show(self) -> str:
        let dollars = self.cents / 100
        let remainder = self.cents.abs() % 100
        if self.cents < 0:
            return "-${dollars.abs()}.{remainder:02}"
        return "${dollars}.{remainder:02}"

impl format for money:
    def format(self, spec: format_spec) -> str:
        # money supports only string-style formatting
        match spec.style:
            @none | @string =>:
                let s = self.show()
                return pad_and_align(s, spec)
            _ => panic("money does not support numeric format styles")

let price = money { cents: 12345 }
println("{price}")          # $123.45
println("{price :: >10}")  # "    $123.45"
```

---

## Compile-Time Checking

The compiler validates every format spec at compile time.

### Unknown format type

```kira
let s = "hello"
let bad = "{s :: x}"
```

```
error[E00xx]: unsupported format style for str
  --> src/main.kira:14:5
   |
14 |     let bad = "{s :: x}"
   |                  ^^^^^^ `str` does not support hexadecimal formatting
   |
   = `str` supports: s (default), ? (debug)
   = hint: use "{s}" for default display, or "{s :: ?}" for debug output
```

### Type mismatch in dynamic spec

```kira
let w = "10"
let bad = "{42 :: {w}}"
```

```
error[E00xx]: format width must be usize, found str
  --> src/main.kira:15:5
   |
15 |     let bad = "{42 :: {w}}"
   |                       ^ expected usize, found str
   |
   = the expression inside `{...}` in a format width must evaluate to usize
```

### Precision on integer

```kira
let bad = "{42 :: .2}"
```

```
error[E00xx]: precision not allowed for int32
  --> src/main.kira:16:5
   |
16 |     let bad = "{42 :: .2}"
   |                  ^^^^^^^ `.precision` is only valid for floating-point types
   |
   = for integers, use width padding instead: "{42 :: 02}"
```

---

## Design Rationale

### Why `::` instead of `:` or `|`?

Kira uses `:` to start blocks (`def foo():`, `if x:`) and in type annotations (`name: str`). Inside an interpolation brace, `{name: str}` would be ambiguous: is it "format `name` with spec `str`" or "interpolate the expression `name: str`"? Using `|` would collide with bitwise OR: `{a | b}` is either "format a with spec b" or "interpolate a | b". `::` is completely unused in Kira syntax and reads naturally as "format as".

### Why a mini-DSL instead of named arguments?

An alternative is explicit, typed formatting:

```kira
"{value :: width: 8, pad: '0', base: @hex}"
```

This is more verbose and harder to scan for common cases. The mini-DSL `08x` is a widely understood convention (Python, Rust, C). The compiler parses it at compile time into the same typed `format_spec` struct, so there is no runtime cost or loss of type safety.

### Why separate `debug` from `show`?

`show` is for user-facing output: it might omit private fields, localize numbers, or use human-readable units. `debug` is for programmer-facing output: it should be unambiguous, round-trippable, and stable. Keeping them separate follows the precedent of Rust's `Display`/`Debug` and Python's `__str__`/`__repr__`.

### Why `format_spec` as a runtime struct?

The spec is parsed at compile time, but the resulting descriptor is passed as a value at runtime. This lets a single `format` implementation handle all variations (width, alignment, precision) without requiring dozens of trait methods. For primitive types, the compiler may inline and specialize the formatting call, eliminating the struct entirely in release builds.

### Why not require explicit method calls?

You can always write explicit formatting:

```kira
"value = {value.to_hex(width: 8, pad: '0')}"
```

This is fully explicit and type-safe. The interpolation format spec is **syntactic sugar** for the common case. It does not replace explicit methods; it complements them. When the format is static and simple, the spec is clearer. When the format is dynamic or complex, explicit method calls are preferred.

---

## Integration with Existing Kira Features

### `pure` functions

`format` is typically a `pure` function — same inputs produce same output, no side effects. The compiler may evaluate literal format expressions at compile time:

```kira
static GREETING: str = "Hello, {name :: >10}!"
# If `name` is static, the entire string is computed at compile time.
```

### `static assert` and contracts

Format specs can appear in `static assert` diagnostics:

```kira
static assert size_of[usize]() == 8, "expected 64-bit, got {size_of[usize]() :: d}-bit"
```

### No interaction with `machine`

String formatting is a high-level, safe operation. It never appears in `machine` code.

---

## Summary of Additions to the Language

| Addition | Location | Description |
|---|---|---|
| `:: spec` syntax | Layer 1 — Strings | Format specification inside interpolation braces |
| `trait debug` | Prelude | Programmer-facing representation |
| `trait format` | Prelude | Fine-grained formatting control |
| `type format_spec` | `std.fmt` (auto-imported when used) | Compile-time parsed descriptor |
| `type align_mode`, `sign_mode`, `format_style` | `std.fmt` | Enumerations for spec components |
| `pad_and_align(str, format_spec) -> str` | `std.fmt` | Utility for default padding behavior |
