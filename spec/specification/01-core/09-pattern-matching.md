# 9. Pattern Matching

**Status:** Implemented

Covers `match`, guard clauses, destructuring of tuples and structs, and `if let`/`while let`.

## `match`

`match_expr`, `pattern` in `spec/kira-grammar.ebnf`. `match` is an expression: it produces a value, and its type is the unified type of all arms. Exhaustiveness is checked for sum types — an unhandled variant is a compile error (`check.cpp`'s exhaustiveness pass over sum-type discriminants).

```kira
let description = match score:
    100      => "perfect"
    90..=99  => "excellent"
    70..=89  => "good"
    _        => "needs work"
```

`match` is the required way to inspect a sum type (see [Type Declarations](10-type-declarations.md)):

```kira
type shape =
    | @circle(float64)
    | @rect(float64, float64)

def area(s: shape) -> float64:
    match s:
        @circle(r)    => 3.14159 * r * r
        @rect(w, h)   => w * h
```

## Guard clauses

An `if` condition after a pattern further restricts when that arm matches; the pattern must still match, and the guard must also hold. A guarded arm does not count toward exhaustiveness on its own — a following unguarded arm for the same shape is required (as `@circle(r)` is, above).

```kira
match s:
    @circle(r) if r <= 0.0 => 0.0
    @circle(r)             => 3.14159 * r * r
    @rect(w, h)            => w * h
```

## Destructuring

Patterns destructure tuples and structs positionally or by field name.

```kira
let point = (3.0, 4.0)
let (x, y) = point

type person = { name: str, age: int32 }
let { name, age } = someone
```

## `if let` and `while let`

`if let` matches a single pattern and binds on success, running its block only when the match holds:

```kira
if let @some(u) = find_user(42):
    println("found {u.name}")
```

`while let` repeats as long as the pattern keeps matching — for draining a source that eventually stops matching (e.g. yields `@none`):

```kira
while let @some(line) = reader.next():
    process(line)
```

## See also

- [Type Declarations](10-type-declarations.md) — sum types and structs, `match`'s primary subjects.
- [Error Handling](11-error-handling.md) — `option`/`result`, the sum types `if let`/`while let` are most often used with.
