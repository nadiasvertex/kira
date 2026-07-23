# 38. The `machine` Layer

**Status:** Partial

`machine` is a function-modifier prefix granting access to low-level machine details — raw pointers, pointer arithmetic, explicit layout, SIMD, unsafe casts, and inline assembly — inside which the compiler makes none of its usual safety guarantees.

## Syntax

`machine` is one of the `func_modifier` alternatives (`spec/kira-grammar.ebnf`, `func_prefix`/`func_modifier`), composing with `pure`, `async`, and `static`; canonical order is `static` before `pure` before `async` before `machine`. Raw pointer types use `ptr_type = "*" type_expr | "*" "mut" type_expr`. Inline assembly is a statement, `asm_stmt = "asm" "{" ASM_CONTENT "}" NEWLINE`.

```kira
machine def fast_sum(data: slice[float32], len: usize) -> float32:
    let p = data.as_ptr()
    var sum: float32 = 0.0
    for i in 0..len:
        sum += *(p + i)
    return sum
```

Prefixes compose:

```kira
async machine def dma_transfer(src: *byte, dst: *mut byte, n: usize) -> result[unit, dma_error]:
    ...

pure machine def read_le_u32(p: *uint32) -> uint32:
    *p
```

## Semantics

Outside a `machine` function the compiler assumes no pointer aliasing and no invalid memory; `machine` is where that assumption is lifted. Contracts (`pre`/`post`) are permitted on `machine` functions; all checks within a `machine` body are runtime-only, not statically verified.

Facilities available inside `machine` functions, per the reference design:

- Raw pointer types `*T` and `*mut T`, and pointer arithmetic.
- Explicit memory layout via `packed` (see below — not `machine`-gated).
- SIMD intrinsics.
- Unsafe casts (`transmute`).
- Inline assembly (`asm { ... }`).
- `uninit[T, N]`, a fixed-capacity, alignment-correct buffer for `N` slots of `T` that carries no guarantee any slot holds a valid `T`:

```kira
pub type uninit[T, N: usize]     # opaque; N slots, sized/aligned for T

machine def slot_ptr[T, N: usize](buf: &uninit[T, N], i: usize) -> *mut T: ...
machine def write_slot[T, N: usize](buf: &mut uninit[T, N], i: usize, value: T) -> unit: ...
machine def read_slot[T, N: usize](buf: &mut uninit[T, N], i: usize) -> T: ...
    # moves the value out of slot i — the caller must not treat it as initialized afterward
machine def drop_first[T, N: usize](buf: &mut uninit[T, N], len: usize) -> unit: ...
    # runs T's drop over slots 0..len only; slots len..N are assumed never initialized
machine def as_slice[T, N: usize](buf: &uninit[T, N], len: usize) -> slice[T]: ...
    # claims slots 0..len are initialized and hands back an ordinary view over them
```

Only these four functions need the `machine` prefix — the unsafety is contained inside them. Ordinary, non-`machine` code is free to call them; a type built on `uninit[T, N]` calls them once, internally, and exposes a fully safe API to everyone else (`small_list[T, N]`, see [Small List](../stdlib/collections/47-small-list.md), is the motivating use). `uninit[T, N]`'s alignment tracks `T`'s natural alignment automatically — narrower than general per-field layout control, but sufficient to size and align a buffer to match a type the compiler already lays out.

### `packed` struct layout

`packed` is a struct-only modifier — fields laid back-to-back with no padding, final size rounded up to the widest field's alignment (or not rounded at all when packed) — and is available on any struct declaration, **not** gated by `machine`; see [Type Declarations](../core/10-type-declarations.md) for the full field-layout rules. Finer-grained per-field `layout`/`align`/`offset` control beyond `packed` remains aspirational.

## Implementation status

- **Implemented:** `machine` parses and composes with the other function modifiers (`is_machine` on `ast::type_modifiers`/function modifiers, `token_kind::kw_machine`); `packed` on struct declarations is fully implemented and tested end-to-end (layout computed by `src/runtime/layout.{h,cpp}`, exercised by `src/testdata/codegen_test/packed_struct_layout.kira` and `src/testdata/semantic_check_test/accept_packed_struct.kira`/`report_packed_on_sum_type.kira`); raw pointer types `*T`/`*mut T` parse, intern (`type_kind::ptr_kind`), and lower on both backends for dereference (`*p`) and assignment through a dereferenced pointer (`*p = value`).
- **Not implemented:**
  - `machine` grants no additional capability at check time — there is no gating that requires a `machine` function to use raw pointers, and no diagnostic for using them outside one. The modifier is currently syntactic only.
  - Pointer arithmetic (`p + i`) has no operator support in the checker.
  - `.as_ptr()` and any other pointer-producing conversion method do not exist.
  - `transmute` does not exist anywhere in the source tree.
  - SIMD intrinsics do not exist.
  - `asm_stmt` parses to an AST node and type-checks to `unit`, but has no HIR lowering on either backend — an `asm { ... }` block is accepted and silently does nothing.
  - `uninit[T, N]` and its four machine functions (`slot_ptr`, `write_slot`, `read_slot`, `drop_first`, `as_slice`) do not exist; `small_list[T, N]` in the standard library is not yet buildable on top of them.

## See also

- [Type Declarations](../core/10-type-declarations.md) — `packed` and general struct layout.
- [Small List](../stdlib/collections/47-small-list.md) — the motivating consumer of `uninit[T, N]`, once implemented.
