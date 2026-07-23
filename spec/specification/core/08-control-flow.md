# 8. Control Flow

**Status:** Implemented

Covers `if`/`elif`/`else` as an expression, the postfix `if/else` form, `while`, both forms of `for` (statement and comprehension), and `break`/`continue`.

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

## See also

- [Pattern Matching](09-pattern-matching.md) — `match`, the other primary expression-valued control construct.
- [Bindings](03-bindings.md) — `var`, typically the loop counter in a `while`.
