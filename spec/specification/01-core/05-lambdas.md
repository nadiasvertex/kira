# 5. Lambdas

**Status:** Partial

Covers anonymous inline function syntax (`lambda_expr` in `spec/kira-grammar.ebnf`), its multi-line form, explicit capture lists, and typical use.

## Syntax

A lambda is `params => expr` or `params -> ReturnType => expr`. A single untyped parameter needs no parentheses; multiple parameters, or an explicit type, require them.

```kira
let double = x => x * 2
let add    = (x, y) => x + y

# With type annotations
let add = (x: int32, y: int32) -> int32 => x + y
```

## Explicit capture lists

By default a lambda captures every outer-scope name its body uses (`capture` in [Closures and Capture](../02-intermediate/16-closures-and-capture.md)). A capture list, written `[...]` immediately before the parameters, overrides this: only the listed names may be used from the enclosing scope, and each name's capture mode is stated explicitly rather than inferred.

- `name` — capture by the default rule (borrow, or move if the closure escapes; `move` forces move for every bare name in the list).
- `&name` — capture by borrow, regardless of escape.
- `&mut name` — capture by mutable borrow.
- `[]` — capture nothing; the body may reference only its own parameters and globals.

Referencing an outer-scope name that is not in the capture list is an error, the same as referencing an undeclared name.

```kira
let factor = 3
let offset = 10
let scaled = numbers.map([factor] x => x * factor)   # `offset` is not in scope here

var total = 0
let accumulate = [&mut total] x =>:
    total = total + x

let pure_double = [] x => x * 2   # no outer state reachable
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

## Implementation status

- Lambda expressions, both single-expression and multi-line block form, are implemented and exercised throughout the semantic and codegen stress corpora.
- Explicit capture lists are implemented end to end. All three entry forms parse (`parser.cpp`'s `parse_capture_list`, reached from a leading `[` via the `at_capture_list` lookahead), the restriction is enforced by the checker (a reference to an outer local not in the list is rejected with a diagnostic naming the list), and both modes lower on both backends — see [Closures and Capture](../02-intermediate/16-closures-and-capture.md) for the mode semantics and the remaining `move` gap.
- One caveat on the multi-line form: `params =>:` requires the block to start on the *next* line, indented. The single-line spelling `x =>: total = total + x` used in an example above does not parse, and never did; write it as a `=>:` followed by an indented block.

## See also

- [Functions](04-functions.md) — parameter/return inference rules, `fn(...) -> ...` values.
- [Collections](06-collections-list-array.md) — `map`/`filter`/`find` and the other methods lambdas are commonly passed to.
- [Closures and Capture](../02-intermediate/16-closures-and-capture.md) — the borrow-vs-move capture rule that governs bare names in a capture list.
