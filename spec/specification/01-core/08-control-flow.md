# 8. Control Flow

**Status:** Partial

Covers `if`/`elif`/`else` as an expression, the postfix `if/else` form, `while` (including `while let`), both forms of `for` (statement and comprehension), `break`/`continue`, and the `scope` block.

## Every construct is an expression

`if`, `match`, and block bodies all have a value and a type. `if`/`match` yield the value of the branch taken; a `for ... =>` comprehension yields a `list`; constructs that run only for effect — `while`, a plain `for` statement, an assignment — have type `unit`.

## `if`

`if_expr` in `spec/kira-grammar.ebnf`.

```kira
if score > 90:
    return "A"
elif score > 80:
    return "B"
else:
    return "C"
```

As an expression, `if`'s value is the last expression of the branch taken:

```kira
let grade =
    if score > 90: "A"
    elif score > 80: "B"
    else: "C"
```

For an inline choice between two values, the postfix form puts the likely value first:

```kira
let label = "pass" if score > 90 else "fail"
```

## `while`

`while_stmt`.

```kira
var n = 1
while n < 100:
    n = n * 2
```

### `while let`

`while_stmt`'s second form repeats the body as long as a pattern keeps matching a re-evaluated scrutinee, binding the pattern's names inside the body on each successful match:

```kira
while let @some(line) = reader.next():
    process(line)
```

The loop ends the first time the pattern fails to match (here, when `reader.next()` yields `@none`) without running the body for that iteration. See [Pattern Matching](09-pattern-matching.md#if-let-and-while-let) for the full rules shared with `if let`.

## `for`

`for_stmt` (statement form, runs for effect) and `for_expr` (expression form, uses `=>` to denote the yield expression — a distinct production from `for_stmt`, per the grammar's own note).

```kira
for i in 0..10:          # 0, 1, ..., 9
    println(i)

for i in 0..=10:         # 0, 1, ..., 10
    println(i)

for name in names:
    println("Hello, {name}")
```

A range with no upper bound (`0..`) is valid and never terminates on its own; pair it with `break`, or with an adapter like `take`/`take_while` when it feeds an iterator chain (`std.iter`), rather than driving a bare `for` to completion.

### The loop head destructures, it does not match

`for_vars` accepts any `pattern`, so a `for` head can destructure a tuple or struct the same way a `let` can:

```kira
for (x, y) in points:
    println("{x}, {y}")
```

This pattern must be **irrefutable** — guaranteed to match every element the iterable produces (see the note on irrefutability in `spec/kira-grammar.ebnf`). `for` has no "skip elements that don't match" mode the way `while let` does for a single scrutinee; a `for` head is a destructuring bind repeated per element, not a per-element test. To drop non-matching elements, filter with a guard instead:

```kira
for x in xs if keep(x):
    ...
```

or, when the source is naturally an iterator that eventually stops matching, drive it with `while let` against `.next()` directly rather than a `for` loop.

### `for` as an expression (comprehension)

`for ... => expr` collects results into a `list`. It supports a filter guard and multiple generators (comma-separated), producing the cross product:

```kira
let squares = for x in 1..=5 => x * x    # [1, 4, 9, 16, 25]
let evens   = for x in 0..20 if x % 2 == 0 => x
let pairs   = for x in 0..3, y in 0..3 => (x, y)
```

## `break` and `continue`

Inside a `while` or `for` *statement* loop, `break` exits the innermost enclosing loop and `continue` skips to the next iteration:

```kira
for line in lines:
    if line.is_empty():
        continue
    if line == "stop":
        break
    process(line)
```

Rules:

- Both require an indented block body to appear in. The single-line `if cond: expr` form takes an *expression*, and `break`/`continue` are statements — `if line.is_empty(): continue` does not parse.
- Neither carries a value, and neither takes a loop label: `break` inside a nested loop exits only the innermost one.
- A `for` *expression* (the `for x in xs => expr` comprehension) has no block body, so there is nowhere to put a `break`/`continue`; use a guard (`for x in xs if keep(x) => f(x)`) to drop elements instead.

## `scope`

`scope_stmt`. A bare block, introduced without a condition or subject, that exists only to bound the lifetime of the bindings inside it:

```kira
scope:
    let f = fs.open("build.log")
    f.write_all(summary)
# f is out of scope, and already dropped, by the time execution reaches here
process_next_step()
```

- A name bound with `let`/`var` inside a `scope` block is not visible past its `DEDENT` — the same rule that already applies to an `if`/`while`/`for`/`match` body, just without a loop or branch attached to it.
- Every live local declared directly in the block is dropped, in reverse declaration order, no later than the block's end — the same drop rule as any other scope (see [Shared Ownership and Drop](../02-intermediate/17-shared-ownership-and-drop.md)). This is what distinguishes `scope` from simply indenting code: it forces those drops to happen at that point rather than waiting for the enclosing function or loop body to end.
- `scope` is not a loop: `break` and `continue` pass through it and still target the nearest enclosing `while`/`for`. A `return` inside a `scope` block exits the function, unwinding through the block's drops on the way out, same as any other early return.
- Unlike Python's `with`, there is no separate enter/exit protocol to implement — `scope` is a plain block; whatever drop behavior its contents have is exactly the same `drop` a type already runs when any other scope ends. There's nothing to opt a type into beyond implementing `drop` itself.

## Implementation status

- `if`, `while` (including `while let`), both forms of `for`, and `break`/`continue` are implemented end-to-end.
- The irrefutability rule for a `for` head (and for `let`/`var`) is not yet enforced: the checker records a for-loop pattern's bindings without checking that it can't fail, and lowering (`hir/lower.cpp`) assumes it never does — passing a refutable pattern (e.g. `for @some(x) in xs:`) currently compiles instead of being rejected, and its runtime behavior on a non-matching element is unspecified. Treat this as a checker gap against the rule stated above, not a supported way to filter a loop.
- `scope` is design-only: there is no `scope` keyword, `scope_stmt` production, or parser/checker/lowering support yet. Everything in the `scope` section above describes the target design, layered on drop semantics that already work for every other kind of scope.

## See also

- [Pattern Matching](09-pattern-matching.md) — `match`, the other primary expression-valued control construct.
- [Bindings](03-bindings.md) — `var`, typically the loop counter in a `while`.
- [Shared Ownership and Drop](../02-intermediate/17-shared-ownership-and-drop.md) — the drop rules `scope` relies on to end a resource's lifetime early.
