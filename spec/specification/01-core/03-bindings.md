# 3. Bindings

**Status:** Implemented

Covers `let` and `var` bindings, shadowing, and compound assignment.

## `let` and `var`

`let` introduces an immutable binding; `var` introduces a mutable one (`let_stmt`, `var_stmt` in `spec/cinder-grammar.ebnf`). Both infer their type from the initializer expression when no annotation is given, and accept an explicit `: type_expr` annotation.

```cinder
let name = "Alice"      # immutable — name cannot be reassigned
var count = 0           # mutable — count can change
count = count + 1

let score: float64 = 0.0
var remaining: int32 = 10
```

Reassigning a `let` binding (`name = ...` after its declaration) is a compile error; only `var` may be the target of `assign_stmt`.

## Shadowing

A second `let` with the same name in the same scope replaces the first: the name now refers to the new binding, and the old value is no longer reachable under that name (its lifetime otherwise ends normally).

```cinder
let x = 1
let x = x + 1    # x is now 2; original x is gone
```

A local binding also shadows a module of the same name: if `s` is a local, `s.name` is always field access on it, never a path into a module `s`. Module-scope values follow a stricter rule — see [Dotted Names](12-modules-and-imports.md#dotted-names).

## Compound assignment

A `var` may be updated with a compound assignment operator — `+=`, `-=`, `*=`, `/=`, and the rest of `assign_op` — as shorthand for `target = target <op> value`.

```cinder
var total = 0
total += 5       # same as total = total + 5
```

The wrapping (`+%=`) and saturating compound forms exist for the same operators as their non-assigning counterparts; see [Built-in Types](02-built-in-types.md#integer-overflow).

## `where` bindings

A `let` or `var` initializer can name helper values with a trailing `where` clause. The helpers are visible only inside that initializer and end with the statement.

```cinder
let v = x + y where x = 40, y = x / 20
```

Longer clauses use an indented block:

```cinder
let v = do_something(a, b, c) where:
    a = find_a_thing(v1, v2)
    b = substr(v2, 5)
    c = compute_a_thing(v, 10)
```

- Bindings evaluate top to bottom, and a later one can use an earlier one. They are not mutually recursive.
- They shadow outer names inside the initializer. After the statement they are gone.
- A `where` binding with a `drop` impl drops when the statement finishes, unless the initializer moved it out.
- The inline form must fit on one line. The `where` keyword in a function signature (`where T: add`) is a separate construct.

For a bare block that bounds the lifetime of several statements, see [`scope`](08-control-flow.md#scope).

## See also

- [Built-in Types](02-built-in-types.md) — literal type defaulting under an annotation.
- [Control Flow](08-control-flow.md) — `while`'s typical use of a `var` loop variable.
