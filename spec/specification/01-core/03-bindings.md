# 3. Bindings

**Status:** Implemented

Covers `let` and `var` bindings, shadowing, and compound assignment.

## `let` and `var`

`let` introduces an immutable binding; `var` introduces a mutable one (`let_stmt`, `var_stmt` in `spec/kira-grammar.ebnf`). Both infer their type from the initializer expression when no annotation is given, and accept an explicit `: type_expr` annotation.

```kira
let name = "Alice"      # immutable — name cannot be reassigned
var count = 0           # mutable — count can change
count = count + 1

let score: float64 = 0.0
var remaining: int32 = 10
```

Reassigning a `let` binding (`name = ...` after its declaration) is a compile error; only `var` may be the target of `assign_stmt`.

## Shadowing

A second `let` with the same name in the same scope replaces the first: the name now refers to the new binding, and the old value is no longer reachable under that name (its lifetime otherwise ends normally).

```kira
let x = 1
let x = x + 1    # x is now 2; original x is gone
```

## Compound assignment

A `var` may be updated with a compound assignment operator — `+=`, `-=`, `*=`, `/=`, and the rest of `assign_op` — as shorthand for `target = target <op> value`.

```kira
var total = 0
total += 5       # same as total = total + 5
```

The wrapping (`+%=`) and saturating compound forms exist for the same operators as their non-assigning counterparts; see [Built-in Types](02-built-in-types.md#integer-overflow).

## See also

- [Built-in Types](02-built-in-types.md) — literal type defaulting under an annotation.
- [Control Flow](08-control-flow.md) — `while`'s typical use of a `var` loop variable.
