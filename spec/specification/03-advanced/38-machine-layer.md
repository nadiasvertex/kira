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

Only these functions need the `machine` prefix — the unsafety is contained inside them. Ordinary, non-`machine` code is free to call them; a type built on `uninit[T, N]` calls them once, internally, and exposes a fully safe API to everyone else (`small_list[T, N]`, see [Small List](../04-stdlib/collections/47-small-list.md), is the motivating use). `uninit[T, N]`'s alignment tracks `T`'s natural alignment automatically — narrower than general per-field layout control, but sufficient to size and align a buffer to match a type the compiler already lays out.

As implemented, the buffer needs fewer operations than the sketch above: `buf[i]` and `buf[i] = v` cover `read_slot`/`write_slot`, `buf.as_mut_ptr()` covers `slot_ptr`, and `buf.len()` is `N`. See the implementation status below for what is and is not built.

`std.mem` is the worked example of this containment: every function in it is `machine` and a handful of lines long, and `std.list`'s `vector[T]` — a growable list owning its own heap storage — is written entirely against it with a fully safe public API.

### `packed` struct layout

`packed` is a struct-only modifier — fields laid back-to-back with no padding, final size rounded up to the widest field's alignment (or not rounded at all when packed) — and is available on any struct declaration, **not** gated by `machine`; see [Type Declarations](../01-core/10-type-declarations.md) for the full field-layout rules. Finer-grained per-field `layout`/`align`/`offset` control beyond `packed` remains aspirational.

## Implementation status

- **Implemented:**
  - `machine` parses and composes with the other function modifiers (`is_machine` on `ast::type_modifiers`, `token_kind::kw_machine`), and now **grants a real capability**: every raw-memory operation below is refused outside a `machine` function (`checker::require_machine_context`, `src/semantic/check.cpp`). Until this landed the modifier was syntax with nothing behind it.
  - `packed` on struct declarations, fully implemented and tested end-to-end (layout computed by `src/runtime/layout.{h,cpp}`, exercised by `src/testdata/codegen_test/packed_struct_layout.kira` and `src/testdata/semantic_check_test/accept_packed_struct.kira`/`report_packed_on_sum_type.kira`).
  - Raw pointer types `*T`/`*mut T` parse, intern (`type_kind::ptr_kind`), and lower on both backends.
  - `.as_ptr()` / `.as_mut_ptr()` on a `list[T]`, `slice`/`slice_mut[T]`, `str`, fixed `array[T, N]`, and `uninit[T, N]` — the address of element 0 of the receiver's data block (`hir_container_data`, read through the same `resolve_container_view` indexing uses, so the two cannot disagree about where elements start). A `*T` never permits a write through it; that is refused on the type rather than on the binding's `let`/`var`.
  - **Pointer arithmetic and raw read/write, spelled as indexing:** `p[i]` reads and `p[i] = v` writes at `p + i * size_of[T]()`, and `&p[i]` is the offset address. Unchecked — a raw pointer carries no length, which is the whole distinction from a `slice`. `*p` is `p[0]`.
  - `size_of[T]()` and `align_of[T]()`, folded at lowering time against `runtime::layout_of` — the same function both backends read every field offset and element stride from, so a `size_of[T]()` cannot disagree with the stride `T` actually occupies. `size_of` previously type-checked to `usize` and then failed to lower at all.
  - `ptr_cast[U](p)` — reinterprets a raw pointer's pointee type. No code: every raw pointer is an address. Mutability follows the operand, so a cast can never *gain* the right to write.
  - `uninit[T, N]` and genuine stack storage for it: an `alloca` in the LLVM tier's entry block, a statically-sized frame-local byte range (`op_stack_alloc`) in the bytecode tier. `buf[i]` is bounds-checked against `N` (a compile-time constant, so the check is free); `.len()` folds to `N`; `.as_mut_ptr()` hands out the buffer. Buffers are zero-filled, which `uninit` does not promise — it only removes the nondeterminism, so a read of an unwritten slot fails the same way every run. A function's `uninit` buffers together may use at most **1 MiB** of frame storage (`hir::k_max_frame_stack_bytes`); a larger frame is a compile error on every backend, naming each contributing buffer. Lambda bodies are frames of their own. A callee holding a buffer is never inlined (which would move the buffer into the caller's budget), and a function owning a buffer makes no tail calls (a callee may still point into it). On the LLVM tier every function carries inline stack probes, so running out of thread stack under the limit faults on the guard page instead of stepping past it.
  - Raw heap memory: the `rt_alloc`/`rt_realloc`/`rt_free` intrinsics (`src/runtime/allocator.h`), wrapped in typed, element-counted form by [`std.mem`](../04-stdlib/collections/43-list.md). The allocator is selectable at run time (`KIRA_ALLOCATOR=system|arena`).
- **Not implemented:**
  - The four named machine functions in the sketch above (`slot_ptr`, `write_slot`, `read_slot`, `drop_first`, `as_slice`) do not exist under those names. `slot_ptr`/`write_slot`/`read_slot` are subsumed by `buf.as_mut_ptr()` and `buf[i]`. `as_slice` has no equivalent: there is no way to form a `slice[T]` over the first `len` slots of a buffer. `drop_first` is blocked on destructors, which do not run anywhere (`../../todo.md` item 6).
  - `small_list[T, N]` ([Small List](../04-stdlib/collections/47-small-list.md)) is now *buildable* on `uninit[T, N]`, but has not been built.
  - `transmute` does not exist anywhere in the source tree.
  - SIMD intrinsics do not exist.
  - `asm_stmt` parses to an AST node and type-checks to `unit`, but has no HIR lowering on either backend — an `asm { ... }` block is accepted and silently does nothing.
  - `machine` does not relax any *other* rule: arithmetic overflow still traps, and the borrow/move checkers still run. It gates raw-memory access and nothing more.
  - Contracts (`pre`/`post`) on `machine` functions are not special-cased in any way.

## See also

- [Type Declarations](../01-core/10-type-declarations.md) — `packed` and general struct layout.
- [Small List](../04-stdlib/collections/47-small-list.md) — the motivating consumer of `uninit[T, N]`, now buildable on it.
- [List](../04-stdlib/collections/43-list.md) — `std.mem` and `vector[T]`, the first things built on this layer.
