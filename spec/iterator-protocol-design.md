# Iterator Protocol Design Decision

Status: draft, first slice implemented. This resolves the open question
left by `spec/typed-ir-design.md`'s "Open questions to resolve before step
5 (comprehensions)": *"What is the iterator protocol `for` comprehensions
lower against (a trait? a builtin fixed shape)? Not yet decided anywhere
in `spec/`."*

## Decision: a builtin fixed shape now, a trait later

`for`/comprehensions lower against a **closed, compiler-recognized set of
iteration shapes** — not a general `iterator`/`next()` trait — for this
milestone. A real extensible protocol (a `trait iterator` a user type can
`impl`, the way `spec/kira-reference.md`'s "Higher-Kinded Traits" section
sketches `functor`/`monad`) is deferred until Kira's ownership/borrowing
model exists, for a concrete reason: any `next()`-shaped method needs to
mutate iterator state between calls (`&mut self`, or an equivalent), and
`spec/llm-compiler-roadmap.md`'s "What Not To Assume Yet" is explicit that
*"there is no settled ownership or borrow-checking implementation"* yet.
Designing a mutation-based trait against an unsettled mutation model would
mean re-deriving this decision once borrowing exists anyway — exactly the
risk `spec/typed-ir-design.md` already flagged for guessing the protocol
too early.

This is not a new gap being introduced: today, `src/semantic/check.cpp`'s
`element_type_of` already types `for`/comprehensions against a hardcoded
switch over exactly six shapes (`array[T,N]`, `list[T]`, `slice[T]`/
`slice_mut[T]`, `range[T]`, `option[T]`, and `str` → `char`), with no trait
or `find_method` dispatch involved anywhere. This decision keeps that
shape and gives *lowering* (not just checking) a concrete plan against it,
one iterable kind at a time.

## Why this doesn't need a runtime iterator value at all

Every one of the six shapes above has its length/bounds known through an
operation lowering can already reach directly — a range's own `start`/
`end`, or a container's `.len()` plus indexing. None of them need an
external "iterator object" with mutable internal position state:

- **`range` (`a..b` / `a..=b`)** — a counting loop: bind `start`/`end`
  once, loop while `index < end` (`<= end` for `..=`), incrementing
  `index` each pass.
- **`array`/`list`/`slice`/`slice_mut`** — the same counting loop, but
  bounded by `.len()` instead of a literal `end`, indexing the container
  at each `index` to produce the loop variable's value.
- **`str`** — the same shape as arrays/lists, yielding `char` per index
  (exactly what `element_type_of` already assumes).
- **`option[T]`** — not a loop at all: `for x in opt: body` is a single
  conditional, `match opt: @some(x) => body, @none => ()`, reusing pattern
  matching we already lower rather than inventing loop machinery for a
  0-or-1-element "sequence."

A user-defined iterable is the one shape that *would* need real mutable
iterator state (there's no way to special-case a `.len()`/index pair
against an arbitrary user type), so it stays unsupported until the trait
version of this decision is revisited.

## Implementation scope, sliced by shape

Each shape is its own increment, in roughly the order above (simplest
first). This iteration implements the first two slices:

1. **Range-literal `for` statements** (`for x in a..b: body`, `for x in
   a..=b: body`) — implemented. Requires the iterable to be written
   as a range expression directly in the loop header (`ast::binary_expr`
   with `op` `Range`/`RangeInclusive`); a range value stored in a variable
   and iterated later is not recognized (there's no field-access path to
   its `start`/`end` yet) and still rejects. Only a single, plain
   (non-destructuring) loop variable is supported.
2. **`array`/`list`/`slice`/`slice_mut`/`str` iteration** — implemented.
   Same counting-loop shape as (1), but bounded by a length instead of a
   literal: an `array[T, N]`'s length is statically known (`N`, from the
   checked type itself), so it lowers to a plain `hir_literal`; the other
   four don't have a static length, so a new dedicated node —
   `hir_container_len` — represents "this container's element count" (not
   a synthesized `.len()` method call: there's no callee to resolve or
   arguments to check, just a well-defined operation, the same reasoning
   as `hir_tuple_index`). `str` yields `char` elements, matching
   `element_type_of`'s existing assumption. Only a single, plain loop
   variable is supported, same restriction as (1).
3. `option` iteration via a single match — not yet implemented.
4. `for` **expressions** (comprehensions, `for x in a..b => expr`,
   yielding `list[T]`) — deferred regardless of iterable shape. Building
   the result requires constructing and mutating a fresh `list[T]` value
   (an empty-list literal plus repeated `.push` calls), which is separate,
   not-yet-built capability on top of whichever iterable shape backs the
   loop. `spec/typed-ir-design.md`'s own milestone breakdown already lists
   comprehensions last for this reason.
5. `while let` — a related but distinct gap (see `spec/typed-ir-design.md`
   and `src/hir/lower.cpp`'s `while_stmt` case): repeatedly re-testing a
   pattern each iteration and falling out of the loop on the first
   mismatch. Not needed for the range/indexed-container shapes above
   (which use ordinary numeric comparisons, not pattern tests), but would
   be a natural way to express iteration once a real external-iterator
   trait exists (`while let @some(x) = it.next(): ...`).
6. A `break`/`continue` requirement never arises for this design as
   written: those keywords aren't tokenized or parsed anywhere in this
   codebase today (a separate, pre-existing gap), so a `for`/`while`
   guard's "skip this iteration" case is expressed as an ordinary
   conditional wrapping the loop body, not a jump. If `break`/`continue`
   are added to the language later, revisit this.

## Lowering shape for the implemented slices

`for x in a..b: body` (and `..=`) lowers to (reusing only node kinds that
already exist — `hir_let`, `hir_while`, `hir_binary`, `hir_assign`,
`hir_if`, `hir_block`; no new HIR node kinds needed):

```
let <for start> = a           // evaluated once
let <for end>   = b           // evaluated once
var <for index> = <for start>
while <for index> < <for end>:      // <= for `..=`
    let x = <for index>
    body                              // or: if guard: body
    <for index> += 1
```

`for x in xs: body` over `array`/`list`/`slice`/`slice_mut`/`str` lowers
the same way, just bounded by a length instead of a literal, and reading
each element by index instead of using the index itself as the value:

```
let <for container> = xs                          // evaluated once
var <for index>: usize = 0
while <for index> < N:                             // array[T, N]: literal N
                                                    // otherwise: <for container>'s
                                                    // hir_container_len
    let x = <for container>[<for index>]
    body                                            // or: if guard: body
    <for index> += 1
```

Both shapes share their loop-body assembly (the loop-variable binding,
the guard-or-plain body, and the increment) through one helper
(`lowerer::build_for_loop_body` in `src/hir/lower.cpp`); only how the
start/end bounds and the per-iteration value are built differs between
them.

A guard (`for x in a..b if cond: body`) wraps `body` in `if cond: body`
inside the loop, rather than anything resembling `continue` — see point 6
above for why that's sufficient. The loop variable's scope is limited to
the loop body, matching normal block-scoping.
