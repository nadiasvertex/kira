# 18. Traits

**Status:** Implemented

Trait definition and `impl`, default methods, `requires` trait dependencies, and `deriving`.

## Defining and Implementing a Trait

A trait defines a set of capabilities a type can have — "this function works for any type that supports X."

```cinder
trait show:
    def show(self) -> str

type point = { x: float64, y: float64 }

impl show for point:
    def show(self) -> str:
        "({self.x}, {self.y})"
```

Any function accepting `T: show` can call `.show()` on a value of type `T`.

Traits can provide default implementations, overridable per `impl`:

```cinder
trait greet:
    def name(self) -> str

    def greeting(self) -> str:           # default — may be overridden
        "Hello, {self.name()}!"
```

## `requires` — Trait Dependencies

A trait can require that implementing types also implement other traits:

```cinder
trait ord requires eq:
    def cmp(self, other: &self) -> ordering
```

Any type implementing `ord` must first implement `eq`. Given a bound `T: ord`, `eq` methods on `T` are callable without adding `T: eq` — `ord` implies it.

## Deriving Common Traits

Common trait implementations can be generated automatically:

```cinder
type color = @red | @green | @blue
    deriving eq, ord, show, hash

type point = { x: float64, y: float64 }
    deriving eq, show
```

`deriving` generates the implementation the same way it would be written by hand. Specific methods may be overridden while the rest stay derived.

## `deref`

`deref` lets a handle type expose the members of the value it refers to:

```cinder
trait deref:
    type target
    def deref(self) -> &target
```

- Field access and method calls first look for the member on the receiver's own type. If none is found and the receiver type implements `deref`, lookup is retried on `target`.
- A member on the receiver wins over a member of the same name on `target`.
- Lookup through `deref` happens only once the receiver type is fully known. It is never used to guess the type of a receiver that is still being inferred.
- `deref` returns a shared reference. There is no `deref_mut`, so a field reached through `deref` cannot be assigned and a `mut self` method on `target` cannot be called.
- The reference returned by `deref()` borrows the receiver and cannot outlive it.
- `*h` on a value whose type implements `deref` reads the target: it means `*h.deref()`. Like a member reached through `deref`, it is read-only, so `*h = v` and `&mut *h` are errors, and a non-`copy` target cannot be moved out of it. `&*h` borrows the target for as long as `h` lives.
- `*` on any other value that is not a reference, raw pointer, or cell is an error.

## `clone`

`clone` produces a second owner of a value from a reference to the first:

```cinder
trait clone:
    def clone(self) -> Self
```

`clone` is declared in `std.traits` and is in the prelude (see [The Prelude](../04-stdlib/42-prelude.md)). `copy` types do not receive automatic `clone` implementations. `shared[T]` is the first type intended to implement it (see [Shared Ownership and Drop](17-shared-ownership-and-drop.md)).

## Implementation status

`deref` and `clone` are implemented. Both are declared in `src/std/traits.cn`. The checker retries field access, dotted paths, and method calls through `deref` when the receiver's own type lacks the member, records the steps it took (`checked_types::deref_adjustments`), and lowering inserts the `deref()` calls. The `*` operator on a handle records the same single step for its operand. Writing through `deref` (assignment, `&mut`, a `mut self` method) is rejected with a dedicated diagnostic, and the ownership checker treats a member reached through `deref` as borrowed from the handle, so it cannot be moved out or outlive the handle. Only struct, sum, and opaque handle types are looked through, and a receiver whose type is still being inferred is never looked through. `shared[T]` implements `deref` (see [shared-t-type.md](../../shared-t-type.md)).

Trait declarations, `impl`, default methods, `requires` (checked as `trait_decl::requires_bound` in `src/semantic/check.cpp`, which both validates that an implementing type satisfies the required trait and lets bound code use the required trait's methods without a separate bound), and `deriving` (validated against unknown derive names, e.g. `test_reports_unknown_deriving` in `src/semantic/check_test.cpp`) are all implemented and exercised by the semantic test suite.

`deriving` produces a real, runnable method body — not merely a type-check-only rule — for `show`, `eq`, `debug`, and `ord`, on a concrete (non-generic), struct-shaped type. `checker::resolve_deriving_traits` splices in the corresponding `derive_<trait>[T]()` from `std.derive` (`src/std/deriving.cn`), which reflects over the type's own field list at compile time; the derived `ord` is a lexicographic, declaration-order, field-by-field comparison. Two gaps remain:

- `hash` still has no real derivation — no builtin scalar implements `.hash()` and there is no hash-combining primitive to fold field hashes with — so `deriving hash` types the call and does not lower.
- A generic type or a sum-shaped type falls back to the same type-check-only path for *every* derived trait, because the derivation is written against `T.fields()`. `deriving` on such a type still records the conformance, but calling the derived method fails at lowering.

## See also

- [Generics and Inference](19-generics-and-inference.md) — bounds (`[T: show]`) built on traits.
- [Coherence and the Orphan Rule](21-coherence-and-orphan-rule.md) — the at-most-one-impl guarantee `T: show` relies on.
- [Extension Methods](22-extension-methods.md) — adding methods without claiming trait conformance.
- [Trait Objects](24-trait-objects.md) — `box[trait]`, the dynamic counterpart to a generic bound.
