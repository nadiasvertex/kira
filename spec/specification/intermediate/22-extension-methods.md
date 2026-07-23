# 22. Extension Methods

**Status:** Implemented

`extend` blocks, how they differ from `impl`, and method resolution priority.

## `extend`

Adding a *method* to a type is different from making it implement a trait, and it is not restricted. An `extend` block adds methods to any type — including one you do not own — without claiming any conformance:

```kira
extend str:
    def is_palindrome(self) -> bool:
        self == self.reversed()

"racecar".is_palindrome()      # true
```

## Rules

- An `extend` method is not an `impl`: it does not make the type satisfy any trait, so generic code bounded by `T: some_trait` still requires a real `impl`.
- Because it makes no global claim, `extend` needs neither coherence nor the orphan rule — foreign types may be extended freely.
- Extensions are visible only where their module is `use`d.
- A call like `s.is_palindrome()` compiles to a direct static call, never a virtual one.
- Resolution priority: inherent and trait methods take priority; extensions fill in only when no method matches. Two conflicting in-scope extensions are an ordinary call-site ambiguity, resolved by imports.

Use `impl` to say a type *is* something (coherent, orphan-restricted); use `extend` to *add convenience* to a type (unrestricted, import-scoped).

## Implementation status

Implemented in `src/semantic/check.cpp`: an inherent/trait method table is consulted first, and `find_extend_method_for_builtin`-style lookup runs only when it answers nothing — `str`'s `contains`, `starts_with`, `split`, and similar are genuine `extend str` methods in `std.string` resolved this way.

## See also

- [Traits](18-traits.md) — the `impl` mechanism `extend` is deliberately not.
- [Coherence and the Orphan Rule](21-coherence-and-orphan-rule.md) — the restriction `extend` sidesteps.
