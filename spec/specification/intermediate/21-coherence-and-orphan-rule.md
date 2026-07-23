# 21. Coherence and the Orphan Rule

**Status:** Implemented

The at-most-one-implementation guarantee, the orphan rule that keeps it enforceable across separately compiled packages, and the newtype workaround.

## Coherence

For any pair of a trait and a type, a program contains **at most one** implementation, and implementations may not overlap — no two can apply to the same concrete type. A specific `impl show for point` and a blanket `impl[T: show] show for list[T]` coexist because they never apply to the same type; two impls of `show` for `point` do not.

This single-implementation guarantee is what lets `T: show` mean one unambiguous thing, and the standard library depends on it: `map[K, V]` relies on there being one canonical `hash` and `ord` for `K`, or two modules could corrupt the same map by hashing its keys differently.

## The Orphan Rule

To keep the guarantee enforceable across separately compiled packages, an `impl` is allowed only where you own one of the two sides: **`impl Trait for Type` is allowed only if the trait or the type is defined in your package**. Because every impl is anchored to a package that owns one side, two packages can never implement the same trait for the same type, so a conflict can only arise inside one package, where the compiler sees it. `deriving` is always allowed — it implements a trait for your own type.

## Newtype Workaround

When you need a trait you don't own on a type you don't own, wrap the type:

```kira
type my_id = { inner: foreign_id }     # a newtype you own
impl show for my_id:                    # allowed — my_id is yours
    def show(self) -> str: self.inner.show()
```

## See also

- [Traits](18-traits.md) — trait/`impl` mechanics.
- [Extension Methods](22-extension-methods.md) — the unrestricted counterpart for adding methods without claiming conformance.
