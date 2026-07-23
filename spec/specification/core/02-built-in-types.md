# 2. Built-in Types

**Status:** Implemented, with discrepancies from the old tutorial noted below

Covers the built-in scalar type set, numeric literal defaulting, integer overflow behavior, and conversion between types via constructor-call syntax.

## Type list

All built-in type names are lowercase identifiers, interned by `type_table` (`src/semantic/types.cpp`):

```
bool                                            # true or false
int8  int16  int32  int64  int128               # signed integers
uint8 uint16 uint32 uint64 uint128               # unsigned integers
float32  float64  float128                      # floating point
byte                                             # alias for uint8
str                                              # UTF-8 text (view type; see Strings)
char                                             # one Unicode scalar value
unit                                             # the "no value" type; its one value is also written `unit`
isize  usize                                     # pointer-sized signed/unsigned integers
never                                             # the uninhabited/bottom type
```

`int32` is the default integer type and `float64` the default floating-point type when no other context is available.

## Numeric literal defaulting

A numeric literal's type is resolved from context: `let b: int64 = 42` types `42` as `int64`. With no surrounding constraint, an integer literal defaults to `int32` and a decimal literal to `float64`. A literal that cannot fit the type required by context is a compile error, diagnosed at the literal, not deferred to a runtime failure.

```kira
let a = 42          # int32 by default
let b: int64 = 42   # int64 from context
let c: uint8 = 300  # compile error: 300 does not fit in uint8
```

## Integer overflow

Arithmetic on `+`, `-`, `*` is checked: a result that does not fit its type panics rather than wrapping silently or invoking undefined behavior. This is the same class of fault as an out-of-bounds index. `int32.min / -1`, negating `int32.min`, division by zero, and shifting by more than the type's bit width panic under the same principle.

```kira
let x: int32 = int32.max
let y = x + 1        # panics: int32 overflow
```

Every numeric type exposes `.min` and `.max` associated constants (`src/semantic/check.cpp`, handling of the `min`/`max` member names on numeric types).

Three explicit, always-available alternatives to the panicking operators exist as ordinary operator tokens (`plus_percent`, `minus_percent`, `plus_pipe`, ... in `src/parser/token.h`; lowered to `op_add_wrap`/`op_sub_wrap`/`op_mul_wrap` and the saturating opcodes in `src/bytecode/opcodes.h`), not gated behind `machine` code:

- Wrapping (modular): `+%`, `-%`, `*%` — and their compound-assignment forms `+%=`, `-%=`.
- Saturating: `+|`, `-|`, `*|` — clamp at the type's `.min`/`.max` instead of panicking.

```kira
let mixed = h *% 31 +% c     # wrapping — hashes, checksums, ring buffers
let level = volume +| gain   # saturating — clamps at the type's min or max
```

## Implementation status

The tutorial-era design additionally described `checked_add`/`checked_sub`/... methods returning `option[T]` as a third alternative to panicking, alongside wrapping and saturating operators:

```kira
match a.checked_add(b):
    @some(n) => n
    @none    => use_fallback()
```

No such methods exist anywhere in the compiler or standard library (`grep -rn checked_add src` finds only the *internal* panic-on-overflow VM helpers `checked_add_signed`/`checked_add_unsigned` in `src/bytecode/vm.cpp`, which implement the panicking `+` operator itself, not a callable `.checked_add()`). Treat the `option`-returning checked-arithmetic methods as **not implemented** — use wrapping, saturating, or a manual comparison against `.max`/`.min` instead.

## Converting between types

### Constructor-call conversion (checked)

Calling the target type like a constructor converts a value to it:

```kira
let n: int32   = 300
let x: float64 = float64(n)   # int32 -> float64
let b: uint8   = uint8(n)     # narrowing — panics: 300 does not fit in uint8
```

A conversion exists when the target type provides a `from` implementation for the source type; the built-in numeric types provide these for one another (`check_conversion_call` in `src/semantic/check.cpp`). Narrowing is checked the same way arithmetic is: a value that does not fit its destination panics rather than truncating silently. Attempting a conversion with no `from` available is diagnosed at the call site, naming both types:

```
no conversion from `str` to `float64`
  = A conversion exists when `float64` provides a `from` for the source
    type; the numeric types convert between one another.
```

The same `from` mechanism is invoked implicitly by `?` when propagating an error of a different type; see [Error Handling](11-error-handling.md).

### `as` (unchecked)

A separate `as` cast expression also exists (`cast_expr` in `spec/kira-grammar.ebnf`; `hir_cast` in `src/hir/lower.cpp`; `op_cast` in `src/bytecode/opcodes.h`), and is exercised throughout the test corpus (`src/testdata/codegen_stress/019_cast_int_widen_narrow.kira`, `020_cast_float_conversions.kira`) for ordinary numeric conversions, not only in `packed`/machine-layout code. Unlike constructor-call conversion, `expr as T` never panics: integer-to-integer truncates or sign/zero-extends, integer-to-float and float-to-integer follow C++ conversion semantics, and float-to-float widens or narrows — silently, per the `op_cast` doc comment in `opcodes.h`.

```kira
var big: int64 = 1000
var narrow = big as int8     # truncates silently — no panic
```

This contradicts the older tutorial prose's claim that "Kira has no cast operator" — that claim describes the checked constructor-call path only. Both mechanisms are real and coexist: prefer `T(x)` when a bad value should be a caught bug (panics on narrowing loss), and `as` when truncation/reinterpretation is the intended, silent behavior.

## See also

- [Error Handling](11-error-handling.md) — `?`'s use of `from` for error conversion.
- [Bindings](03-bindings.md) — type annotations that establish literal context.
