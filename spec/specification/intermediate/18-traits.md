# 18. Traits

**Status:** Implemented

Trait definition and `impl`, default methods, `requires` trait dependencies, and `deriving`.

## Defining and Implementing a Trait

A trait defines a set of capabilities a type can have — "this function works for any type that supports X."

```kira
trait show:
    def show(self) -> str

type point = { x: float64, y: float64 }

impl show for point:
    def show(self) -> str:
        "({self.x}, {self.y})"
```

Any function accepting `T: show` can call `.show()` on a value of type `T`.

Traits can provide default implementations, overridable per `impl`:

```kira
trait greet:
    def name(self) -> str

    def greeting(self) -> str:           # default — may be overridden
        "Hello, {self.name()}!"
```

## `requires` — Trait Dependencies

A trait can require that implementing types also implement other traits:

```kira
trait ord requires eq:
    def cmp(self, other: &self) -> ordering
```

Any type implementing `ord` must first implement `eq`. Given a bound `T: ord`, `eq` methods on `T` are callable without adding `T: eq` — `ord` implies it.

## Deriving Common Traits

Common trait implementations can be generated automatically:

```kira
type color = @red | @green | @blue
    deriving eq, ord, show, hash

type point = { x: float64, y: float64 }
    deriving eq, show
```

`deriving` generates the implementation the same way it would be written by hand. Specific methods may be overridden while the rest stay derived.

## Implementation status

Trait declarations, `impl`, default methods, `requires` (checked as `trait_decl::requires_bound` in `src/semantic/check.cpp`, which both validates that an implementing type satisfies the required trait and lets bound code use the required trait's methods without a separate bound), and `deriving` (validated against unknown derive names, e.g. `test_reports_unknown_deriving` in `src/semantic/check_test.cpp`) are all implemented and exercised by the semantic test suite.

## See also

- [Generics and Inference](19-generics-and-inference.md) — bounds (`[T: show]`) built on traits.
- [Coherence and the Orphan Rule](21-coherence-and-orphan-rule.md) — the at-most-one-impl guarantee `T: show` relies on.
- [Extension Methods](22-extension-methods.md) — adding methods without claiming trait conformance.
- [Trait Objects](24-trait-objects.md) — `box[trait]`, the dynamic counterpart to a generic bound.
