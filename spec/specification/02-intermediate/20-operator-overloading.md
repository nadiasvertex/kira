# 20. Operator Overloading

**Status:** Implemented

Operators as traits, and the `type output` associated-type mechanism for their result type.

## Operators Are Traits

To make a type work with `+`, implement the `add` trait:

```cinder
trait add:
    type output
    def add(self, other: self) -> self.output

type vec2 = { x: float64, y: float64 }
    deriving show, eq

impl add for vec2:
    type output = vec2
    def add(self, other: vec2) -> vec2:
        { x: self.x + other.x, y: self.y + other.y }

let a = vec2 { x: 1.0, y: 2.0 }
let b = vec2 { x: 3.0, y: 4.0 }
let c = a + b    # vec2 { x: 4.0, y: 6.0 }
```

`type output` is an associated type: each `impl` fixes what `self.output` means for that type, so `add`'s signature can describe "the result type of adding two `self`s" once, generically, without a type parameter on the trait itself.

An operator passes its operands as the method's parameters. The left operand is the receiver, a `self` borrow, so `a` stays usable after `a + b`. The right operand is the `other` parameter, which takes its argument by value, so a `b` that is not `copy` (any struct, under [Ownership and Borrowing](14-ownership-and-borrowing.md)) is moved and cannot be used after `a + b`.

## Equality on tuples, arrays, `option` and `result`

Two tuples, two arrays, two `option`s or two `result`s of the same type are equal when what they hold is equal. `==` compares them part by part with each part's own `==`, and `!=` is its negation:

- a tuple or an `array[T, n]` element by element, in order;
- an `option` is equal when both are `@none`, or both are `@some` with equal values;
- a `result` is equal when both are the same variant with equal payloads.

```cinder
let xs: array[int32, 3] = [1, 2, 3]
let ys: array[int32, 3] = [1, 2, 4]
(1, "a") == (1, "a")                  # true
xs != ys                              # true
@some((1, 2)) == @none                # false
```

The comparison is an ordinary `eq` impl in `std.traits` (`src/std/traits.structural.cn`), bounded on its parts: `impl[T: eq] eq for option[T]`, `impl[T0: eq, T1: eq] eq for (T0, T1)`, and so on. So a type built from these is `eq`, and satisfies a `T: eq` bound, exactly when every type it holds is. `==` on a tuple holding a function value is an error that names the function type, and so is passing that tuple where `T: eq` is required. Tuples have an `eq` impl for 2 to 12 elements; the language has no variadic generics, and a longer tuple has no `==`.

These types have no order: `<`, `<=`, `>` and `>=` on them are errors.

## See also

- [Traits](18-traits.md) — trait/`impl` mechanics this builds on.
- [Coherence and the Orphan Rule](21-coherence-and-orphan-rule.md) — why `a + b` has exactly one meaning for a given type.
