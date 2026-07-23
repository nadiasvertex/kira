# 20. Operator Overloading

**Status:** Implemented

Operators as traits, and the `type output` associated-type mechanism for their result type.

## Operators Are Traits

To make a type work with `+`, implement the `add` trait:

```kira
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

## See also

- [Traits](18-traits.md) — trait/`impl` mechanics this builds on.
- [Coherence and the Orphan Rule](21-coherence-and-orphan-rule.md) — why `a + b` has exactly one meaning for a given type.
