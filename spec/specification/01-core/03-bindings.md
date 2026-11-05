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

A `let` binding also cannot be changed in place. Writing to one of its fields or elements (`xs[0] = 4`), borrowing it with `&mut`, and calling a `mut self` method on it (`xs.push(4)`) are compile errors, in the function itself and in a lambda that captures it. The bindings a destructuring `let` makes (`let (a, b) = pair`) follow the same rule. A `let` that holds a reference changes what it points at, not itself, so `let r = &mut xs` followed by `r.push(4)` is allowed, and so is destructuring through one (`let (a, b) = &mut pair`).

```cinder
let xs = [1, 2, 3]
xs.push(4)          # error: `push` changes its receiver, and `xs` is bound with `let`
var ys = [1, 2, 3]
ys.push(4)          # fine
```

## Parameters

A parameter taken by value belongs to the function, so it can change like a `var`: it can be reassigned, written in place, borrowed with `&mut`, and have `mut self` methods called on it. The same holds for the bindings of a by-value parameter's destructuring pattern. The caller is not affected, because the value was moved or copied in.

A parameter taken by reference or pointer can change what it points at only when it is `&mut T` or `*mut T`. Through a `&T` or a `*T`, every write, `&mut` borrow, and `mut self` call is a compile error. This applies to `self` too: a method declared with `self` cannot change its receiver, and one declared with `mut self` can.

```cinder
def grow(xs: list[int64]) -> list[int64]:
    xs.push(4)                  # fine: `xs` is the function's own
    return xs

def peek(xs: &list[int64]) -> unit:
    xs.push(4)                  # error: `xs` is behind a `&list[int64]`

def fill(xs: &mut list[int64]) -> unit:
    xs.push(4)                  # fine
```

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
