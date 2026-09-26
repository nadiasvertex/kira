# 60. Meta Queries

**Status:** Implemented

A single `type_kind` classification primitive and a small set of member-enumeration queries over Cinder's existing reflection surface.

## Scope

A way to ask *what kind of thing* a reflected type or member *is* before deciding which of those existing queries applies. A generic function both structs and enums, for example, has to ask first. This chapter adds utilities that make it easy to see what the intent of the user query is, without having to write the same code over and over again.

## `type_kind`

```cinder
type type_kind =
    | @struct_kind
    | @sum_kind
    | @scalar_kind
    | @view_kind
    | @array_kind
    | @fn_kind
    | @trait_kind
    | @module_kind

T.kind() -> type_kind
```

`T.kind()` never fails — it is the dispatch primitive every conditional query in this chapter and in [`std.traits`](58-std-traits.md) (`is_struct`, `is_sum`, `type_category`) is built from, and it is the fix for the gap above:

```cinder
static def field_names_or_variant_names[T]() -> list[str]:
    match T.kind():
        @struct_kind => for f in T.fields()   => f.name
        @sum_kind    => for v in T.variants() => v.name
        _            => panic("field_names_or_variant_names: T has no members")
```

## Member queries

Building on the existing per-field/per-variant/per-module-member descriptors ([Compile-Time Execution](../../03-advanced/31-compile-time-execution.md#compile-time-reflection), [Modules as Compile-Time Values](../../03-advanced/36-modules-as-compile-time-values.md#reflecting-on-a-module)):

```cinder
member.is_data_member() -> bool   # true for a struct_field descriptor, false for a function descriptor
member.type_of() -> type_expr     # the member's declared type, as a splice-ready type_expr
```

`member.type_of()` closes a real gap noted in the existing reflection chapter: a struct field descriptor today carries only `type_name: str` (a rendered string, not a usable type — `render_type_expr` collapses generic arguments to `"[..]"` and non-`named_type` shapes to `"<type>"`). `type_of()` instead returns the field's `type_expr` fragment directly from the already-parsed declaration syntax, splice-ready with `~`, sidestepping string rendering (and its precision loss) entirely:

```cinder
static def zero_value[T]() -> expr:
    let parts = for f in T.fields() =>
        `(~(expr.ident(f.name)): zero_value[~(f.type_of())]())`
    `(~(expr.ty[T]()) { ~(expr.list(parts)) })`
```

## Trait/impl queries

```cinder
T.traits() -> list[str]           # names of every trait T has an impl for
Trait.requires() -> list[str]     # names of traits a trait's own `requires` clause lists
```

`T.traits()` is the read-side counterpart to [`std.traits`](58-std-traits.md)'s `implements[T, Trait]` — `implements` answers one yes/no question against a coherence-table lookup; `T.traits()` enumerates the whole set, for code that wants to print or fold over it (a generic `describe[T]()` that lists every capability a type has) rather than test one trait at a time.

## What's deliberately out of scope

- **Splicers and `std::meta::info` handles.** Cinder's quote/splice fragments (`expr`, `type_expr`, `def_expr`, `stmt`) already play this role; introducing a second, more general reflection-handle type alongside them would be two mechanisms doing one job.
- **`define_aggregate` / synthesizing a struct's field list from reflected data.** No Cinder construct emits a new type declaration at compile time today (only new `impl`s and functions, via `def_expr`); adding one is a much larger feature than this chapter's scope and belongs, if ever built, in [Compile-Time Execution](../../03-advanced/31-compile-time-execution.md).
- **Annotations.** C++26 attaches arbitrary compile-time values to a declaration for later reflection; Cinder has no attribute syntax to attach them through, and no forcing use case in the existing stdlib.
- **Namespace/access-specifier reflection.** Cinder's only visibility axis is `pub`/private, already exposed on function and type descriptors as `is_pub` ([Modules as Compile-Time Values](../../03-advanced/36-modules-as-compile-time-values.md#reflecting-on-a-module)); there is no separate namespace concept to reflect over.

## Implementation status

Nothing in this chapter exists yet.

- `T.kind()` requires one addition to `src/comptime/reflect.cpp`: given a resolved type declaration, return `@struct_kind`/`@sum_kind` for a user `type` (already distinguished internally by `decl.definition->kind`, the same check `T.fields()`/`T.variants()` use to gate themselves), `@scalar_kind` for a builtin name, and so on for the remaining variants — no new subsystem, just a new query alongside the four that already exist there.
- `member.type_of()` requires extending `make_field_descriptor` to carry the field's `type_expr` node (not just its rendered `type_name` string) into the returned descriptor, and extending the `struct_instance` value representation to hold an AST-fragment field the way a `type_expr` quote value already does.
- `T.traits()`/`Trait.requires()` require exposing `check.cpp`'s coherence table and trait-declaration `requires` list to the comptime evaluator, which today has no view into semantic-analysis-phase data structures at all — this is the same missing linkage [`std.traits`](58-std-traits.md)'s `implements[T, Trait]` needs, and should land once, shared by both.

## See also

- [Compile-Time Execution](../../03-advanced/31-compile-time-execution.md) — `T.fields()`, `T.variants()`, quoting/splicing, all of which this chapter extends rather than replaces.
- [Modules as Compile-Time Values](../../03-advanced/36-modules-as-compile-time-values.md) — the module-level reflection (`M.functions()`, `M.types()`) this chapter's member queries are symmetric with.
- [`std.traits`](58-std-traits.md) — `is_struct`, `is_sum`, `type_category`, and `implements[T, Trait]`, all defined in terms of `T.kind()`/`T.traits()`.
