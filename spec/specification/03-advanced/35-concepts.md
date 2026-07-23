# 35. Concepts

**Status:** Implemented

`concept` declarations, structural satisfaction (no `impl` required), composition with `+`, and the concepts-vs-traits distinction.

## Syntax

```kira
concept sortable[T]:
    T: ord + show
    size_of[T]() <= 64    # compile-time value constraint

concept network_value[T]:
    T: show + eq + hash + into[bytes] + from[bytes]
```

A `concept_decl` (`ast.h:1698`) names a type parameter list (`concept_param`) and a list of constraint clauses (`concept_constraint`). Each clause is either a trait/concept bound on a subject type, or a compile-time boolean expression.

## Semantics

- A concept is satisfied **structurally**: a type satisfies `sortable` if it already provides everything the concept demands (an `ord` impl, a `show` impl, a size within budget) — there is no `impl sortable for point` to write.
- Use a concept as a bound, exactly where a trait bound would go:

```kira
def sort_and_display[T: sortable](items: &mut list[T]) -> unit:
    items.sort()
    for x in items: println(x.show())
```

- Concepts compose with `+`, and mix freely with trait bounds in the same list:

```kira
def process[T: sortable + network_value](val: T): ...
```

- `concept` is a distinct symbol kind (`semantic_symbol_kind::concept_symbol`, `symbols.cpp:58`) that competes with types, traits, and submodules for a unique name in scope, and resolves through the same trait/concept bound-list machinery as trait bounds (`resolution.cpp:719`, "Validates each `+`-joined term of a trait/concept bound list").
- A compile-time value constraint inside a concept body (`size_of[T]() <= 64`) is checked the same way any other `static` boolean expression is — see [Compile-Time Execution](31-compile-time-execution.md) — and, where it involves an unknown quantity rather than a closed one, is a candidate for the reasoning solver described in [Dependent and Refinement Types](33-dependent-and-refinement-types.md).

## Traits vs. concepts

| | Trait | Concept |
|---|---|---|
| Opt-in | Yes — a type must write `impl trait for type` | No — satisfaction is automatic if the type already qualifies |
| Defines | Behavior a type *can do* (method bodies, associated types) | A constraint a generic function *requires* (bounds, value facts) |
| Written on a type | `impl show for point: ...` | never — no `impl concept for type` form exists |
| Written on a generic parameter | `def f[T: show](...)` | `def f[T: sortable](...)` — same bound-list syntax |
| Composes with `+` | Yes | Yes, and freely with trait bounds in the same list |

## Example

```kira
concept sortable[T]:
    T: ord + show
    size_of[T]() <= 64

def sort_and_display[T: sortable](items: &mut list[T]) -> unit:
    items.sort()
    for x in items: println(x.show())
```

Any type already implementing `ord` and `show`, and no larger than the size budget, satisfies `sortable[T]` at the call site — no declaration is required from that type's own module.

## Implementation status

Implemented: parsing (`parse_concept_decl`, `parser.h:655`, exercised by `src/testdata/parser_stress/032_concept_basic.kira`, `033_concept_higher_kinded.kira`, `085_concept_value_expr.kira`), symbol registration (`symbols.cpp:223`, `types.cpp:847`), module-level duplicate/name resolution alongside types and traits (`resolution.cpp:1561`), and constraint checking (`check_concept_decl`, `check.cpp:14229`), which resolves each constraint's subject type and, for a boolean-expression clause, infers it as an ordinary compile-time expression. Exercised by `check_test.cpp`'s `test_accepts_concept_bound`, backed by `src/testdata/semantic_check_test/accept_concept_bound.kira` (the `sortable[T]`/`size_of[T]() <= 64` example above, verbatim).

## See also

- [Compile-Time Execution](31-compile-time-execution.md) — `static` value constraints inside a concept body.
- [Dependent and Refinement Types](33-dependent-and-refinement-types.md) — the solver that a concept's value constraints are intended to route through for non-closed cases.
