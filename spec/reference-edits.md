# Pending mechanical edits to `kira-reference.md`

**Status: applied.** All `@`-constructor edits and the two example bug fixes
below have been made to `kira-reference.md`, and `kira-grammar.ebnf` has been
updated to match (`sum_variant`, `atomic_pattern`, a new `variant_expr`
production, and the reserved-keyword list). This file is now a historical
record of what was done, kept for reference.

Decided, low-judgment changes applied in a separate pass. Each item was
executable without design decisions.

---

## 1. `@` prefix on all sum-type constructors

**Decision:** every sum-type *variant constructor* is written with a leading
`@`, uniformly, in all three positions where it appears:

1. where it is **declared** (in the `type` declaration),
2. where a value is **constructed**, and
3. where it is **matched** (patterns, including `if let` / `while let`).

A bare lowercase identifier is therefore *always* a variable — a reference in
an expression, a fresh binding in a pattern.

### What DOES get `@`

Only sum-type variant constructors. Both nullary (`@none`, `@north`) and
those taking arguments (`@some(x)`, `@circle(r)`).

The constructor names that appear in the current spec:

- `circle`, `rect`, `point` — the `shape` type (note: the `point` here is the
  nullary *variant* of `shape`, **not** the `point` *struct* — only the variant
  gets `@`)
- `north`, `south`, `east`, `west` — `direction`
- `some`, `none` — `option`
- `ok`, `err` — `result`
- `file_not_found`, `permission_denied`, `parse_failed` — `app_error`
- `red`, `green`, `blue` — `color`
- `inc`, `scale`, `clamp` — the `op` type (Closure Types example)
- `cancelled` — used as an error variant value in `err(cancelled)` → `@cancelled`

### What DOES NOT get `@` (leave untouched)

- Type names: `option`, `result`, `list`, `array`, `shape`, `point` (the
  struct), `direction`, `color`, `expr`, `stmt`, etc.
- Struct literals and struct types: `point { x: 1.0, y: 2.0 }`,
  `type point = { x: float64, y: float64 }`, `vec2 { ... }`
- Built-in values: `unit` (e.g. `ok(unit)` becomes `@ok(unit)` — the `@` goes
  on `ok`, `unit` stays bare), `true`, `false`
- Method / function calls: `.map(...)`, `.show()`, `.push(...)`, `expr.lit(...)`,
  `T.fields()`
- Literal and range patterns: `100 => ...`, `90..=99 => ...`
- The wildcard `_`
- Trait, impl, concept, module names
- Type aliases produced by `static if` (e.g. `type word = uint64`)

### Representative before → after

```kira
# declaration
type direction = @north | @south | @east | @west
type option[T] = @some(T) | @none
type result[T, E] = @ok(T) | @err(E)
type shape =
    | @circle(float64)
    | @rect(float64, float64)
    | @point

# construction
let d: direction = @north
return @ok(parsed)

# match
match find_user(42):
    @some(u) => println("Found: {u.name}")
    @none    => println("Not found")

# if let / while let
if let @some(i) = index[n].try_from(raw_index): ...
while let @some(line) = await receiver.recv(): ...
```

Sections containing constructors to update: Pattern Matching, Type
Declarations, Error Handling, Traits (`largest`, `deriving`), Async/Cancellation
(`ok`/`err`), Dependent Types (`if let @some`), Channels (`while let @some`),
Higher-Kinded Traits (`monad` impl body), Integer Overflow (`some`/`none` in
the `checked_add` example), Ownership and Borrowing (`some`/`none`/`ok` in the
`largest`, closure, and `totals` examples), Programs and `main`
(`ok`/`some`/`none`).

---

## 2. Example bug fixes (unambiguous)

- **Range/result mismatch** (Control Flow, `for`-expression example): change
  `let squares = for x in 1..5 => x * x` to `for x in 1..=5 => x * x` so the
  stated result `[1, 4, 9, 16, 25]` is correct (`..` is exclusive, `..=` is
  inclusive).

- **Higher-kinded impl syntax** (Higher-Kinded Traits): change
  `impl monad[option]:` to `impl monad for option:` to match the
  `impl <trait> for <type>` form used everywhere else. (Constructor bodies in
  that impl also get `@` per item 1.)

---

## Design pass complete

All design items from the review are resolved and written into
`kira-reference.md`. The only remaining work is the mechanical `@`-constructor
pass and the two example-bug fixes in this file (items 1 and 2 above).

Resolved and written: arithmetic overflow, wrapping/saturating operators, the
module system (folders, `pub`/`module`/`file`, `pub use`, compile-time manifest),
second-class borrows / slices / closure capture, data-race freedom
(`send`/`share`, `shared`, `mutex`), entry point (`main`), casts →
constructor-style conversions, `?` error conversion (into-conversion, works on
`option`), `int`/`float` alias removal, the `largest` borrow inconsistency,
contract `return` naming, and modules-as-compile-time-values (`signature`,
parameterized modules, module reflection).
