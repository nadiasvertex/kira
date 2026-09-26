# 59. `std.limits` — Numeric Limits

**Status:** Implemented

Per-type compile-time numeric bounds and properties (`min[T]()`, `max[T]()`, `bits[T]()`, `epsilon[T]()`, ...), reinterpreting `std::numeric_limits` as a set of `static pure` queries over Cinder's builtin numeric types.

## Rationale

Every query below is defined only for `T` such that `is_numeric[T]()` holds ([`std.traits`](58-std-traits.md)); calling one with `T = point`, say, is a compile error at the call site — `static assert is_numeric[T](), "..."`. 

## Queries common to integers and floats

```cinder
bits[T]() -> usize          # bit width: 8, 16, 32, 64, 128
is_signed[T]() -> bool      # same predicate as std.traits.is_signed_integer, extended to floats (always true)
min[T]() -> T               # most negative representable value
max[T]() -> T               # most positive representable value
```

`min`/`max` return a `T`, not a `type_expr` — unlike [`std.traits`](58-std-traits.md)'s transformations, these produce ordinary compile-time *values* of the queried type, reified the same way any other `static` binding is (see [Compile-Time Execution § `static` bindings](../../03-advanced/31-compile-time-execution.md#static-bindings)):

```cinder
static INT16_MAX: int16 = max[int16]()   # 32767, baked into the binary
```

## Integer-only queries

```cinder
digits[T]() -> usize    # number of value bits, excluding the sign bit for a signed type: bits[T]() - (1 if is_signed[T]() else 0)
```

## Float-only queries

```cinder
epsilon[T]() -> T          # difference between 1.0 and the next representable value
infinity[T]() -> T         # +infinity
neg_infinity[T]() -> T     # -infinity
nan[T]() -> T              # a quiet NaN
digits[T]() -> usize       # mantissa precision in bits (24 for float32, 53 for float64, 113 for float128)
```

`epsilon`/`infinity`/`neg_infinity`/`nan` are diagnosed at the call site when `T` is an integer type. It does not define a value for a query that has no meaning for the type, per [Diagnostics](../../00-overview.md#diagnostics)'s "state what was expected, and why."

## Example

```cinder
def clamp_to_range[T](x: T, lo: T, hi: T) -> T:
    if x < lo: return lo
    if x > hi: return hi
    return x

def saturating_cast[From, To](x: From) -> To:
    static assert is_numeric[From]() and is_numeric[To]()
    if x as float64 > max[To]() as float64: return max[To]()
    if x as float64 < min[To]() as float64: return min[To]()
    return x as To
```

## See also

- [`std.traits`](58-std-traits.md) — `is_numeric`, `is_signed_integer`, `is_float`, which gate every query in this chapter.
- [Built-in Types](../../01-core/02-built-in-types.md) — the numeric type list and their literal/conversion rules.
- [Compile-Time Execution](../../03-advanced/31-compile-time-execution.md) — the `static`/`static if` machinery every query dispatches through.
