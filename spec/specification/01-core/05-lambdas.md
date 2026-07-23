# 5. Lambdas

**Status:** Implemented

Covers anonymous inline function syntax (`lambda_expr` in `spec/kira-grammar.ebnf`), its multi-line form, and typical use.

## Syntax

A lambda is `params => expr` or `params -> ReturnType => expr`. A single untyped parameter needs no parentheses; multiple parameters, or an explicit type, require them.

```kira
let double = x => x * 2
let add    = (x, y) => x + y

# With type annotations
let add = (x: int32, y: int32) -> int32 => x + y
```

## Multi-line form

`params =>:` introduces an indented block whose last expression is the lambda's result, the same rule as a `def` body.

```kira
let process = x =>:
    let y = transform(x)
    y * 2
```

## Typical use

Lambdas are most commonly passed as arguments to collection methods such as `map` and `filter`:

```kira
let numbers = [1, 2, 3, 4, 5]
let doubled = numbers.map(x => x * 2)
let evens   = numbers.filter(x => x % 2 == 0)
```

A parameter's type, when omitted, is inferred the same way an unannotated function parameter is — see [Functions](04-functions.md).

## See also

- [Functions](04-functions.md) — parameter/return inference rules, `fn(...) -> ...` values.
- [Collections](06-collections-list-array.md) — `map`/`filter`/`find` and the other methods lambdas are commonly passed to.
