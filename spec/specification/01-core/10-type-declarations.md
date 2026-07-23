# 10. Type Declarations

**Status:** Implemented

Covers struct types, sum types, struct field layout (default padding and `packed`), and type aliases. `type_decl` in `spec/kira-grammar.ebnf`.

## Structs

A struct groups named fields.

```kira
type point = { x: float64, y: float64 }

let p = point { x: 1.0, y: 2.0 }
let d = p.x * p.x + p.y * p.y
```

## Sum types

A sum type (enum / tagged union) represents a value that is exactly one of several variants. Variant constructors are written `@lowercase_name`, optionally with payload types.

```kira
type direction = @north | @south | @east | @west

type shape =
    | @circle(float64)           # holds a radius
    | @rect(float64, float64)    # holds width and height
    | @point                     # holds nothing
```

Constructors are lowercase. A sum type is inspected with `match` (see [Pattern Matching](09-pattern-matching.md)); the checker enforces exhaustiveness over its variants.

## Struct layout: padding and `packed`

By default a struct's fields are laid out as in a systems language's C struct: each field sits at its own natural alignment (padding inserted before it as needed), and the whole struct's size rounds up to its widest field's alignment.

```kira
type mixed = { a: bool, b: int32, c: bool }
# a @ offset 0 (1 byte), 3 bytes padding, b @ offset 4 (4 bytes),
# c @ offset 8 (1 byte), 3 bytes trailing padding -> size 12, align 4
```

`packed`, prefixed on the declaration, byte-packs fields back to back with no padding — for matching a fixed C struct layout, a hardware register block, or a wire-protocol frame:

```kira
packed type header = { magic: uint16, flags: byte, len: uint32 }
# magic @ offset 0 (2 bytes), flags @ offset 2 (1 byte),
# len @ offset 3 (4 bytes) -> size 7, align 1
```

`packed` only applies to struct types; writing it before a sum type, type alias, or refinement type is diagnosed as an error (`` `packed` only applies to struct types; `{}` is {} ``, `src/semantic/check.cpp`), since those have no field layout to pack.

Both the default and `packed` layout rules are implemented end to end, including codegen: `src/runtime/layout.cpp` computes offsets from `ast::type_modifiers::is_packed`, and `src/testdata/codegen_test/packed_struct_layout.kira` / `padded_struct_layout.kira` exercise both through the compiled program, not just the type checker.

## Type aliases

```kira
type meters   = float64
type name_map = list[(str, str)]
```

## See also

- [Pattern Matching](09-pattern-matching.md) — `match` over sum types, destructuring struct patterns.
- [Error Handling](11-error-handling.md) — `option`/`result` are ordinary sum types built from this same construct.
