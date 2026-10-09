# Implementation Plan: `shared[T]` Type

This document outlines the phases for implementing `shared[T]` as a real stdlib type, replacing the builtin name stub. The work requires three new language features (atomic primitives, `clone` trait, and `deref` trait) before the type itself lands.

Each phase is a complete, testable unit that can be committed separately.

## Phase 0: Spec updates

Update the specification to define the new concepts before they arrive in code.

**Changes:**
- Add `deref` trait to ch18 (Traits): `trait deref: type target; def deref(self) -> &target`. Field and method lookup falls back to `deref()` after trying the receiver type's own members. There is no `deref_mut`; this enforces `shared` as read-only.
- Add `clone` trait to ch18 and ch42 (Prelude): `trait clone: def clone(self) -> Self`.
- Fix the false claim in ch30 (Data-Race Freedom) that says `shared[T]` is "implemented as an ordinary single-threaded reference-counted wrapper". Update it to document the phases and current status.

**Status:** Spec only. No compiler changes.

## Phase 1: Atomic primitives

Add `atomic_fetch_add` and `atomic_fetch_sub` machine-layer intrinsics on `*mut usize`, with acquire/release ordering. These are the minimal substrate `shared` needs to increment and decrement a reference count.

**Language side:**
- Add to `std.mem` as `machine def` functions.
- Tests: a `codegen_stress` file with `# expect:` to catch wrong semantics on either backend.

**LLVM:**
- Emit `atomicrmw add` and `atomicrmw sub` with `acquire` and `release` ordering.

**VM:**
- Add opcodes for atomic add and atomic sub with `acquire` and `release` ordering. Then emit them as needed. 

**Definition:** Passes the minimal-intrinsics test (spec/CONVENTIONS.md) because Cinder cannot express atomic operations.

**Status:** Done. `std.mem.atomic_fetch_add`/`atomic_fetch_sub` wrap the inline intrinsics `rt_atomic_fetch_add`/`rt_atomic_fetch_sub`, which have no runtime symbol: the VM uses `op_atomic_fetch_add`/`op_atomic_fetch_sub` and LLVM emits `atomicrmw`. Both operations use `acq_rel` ordering, which covers acquire and release for either use. Test: `src/testdata/codegen_stress/132_atomic_fetch_add_sub.cn`.

## Phase 2: `clone` trait

Add `trait clone` to the stdlib. This trait is a precondition for phase 3 and 4.

**Changes:**
- Add `pub trait clone: def clone(self) -> Self` to `src/std/traits.cn`.
- Re-export from prelude.
- Implement for `shared[T]` in phase 4. Whether `copy` types get automatic `clone` impls is deferred.

**Tests:** Basic call and type-checking of clone. Deferred: `shared` instances.

**Status:** Trait exists; only `shared` implements it for now.

## Phase 3: `deref` trait and auto-deref resolution

This is the largest phase, adding language-level field and method resolution fallback.

**Trait definition:**

```
pub trait deref:
    type target
    def deref(self) -> &target
```

**Checker changes:**

Field and method lookup are modified to try the receiver type's own members first, then retry on the target of `deref()` if the type implements it.

- Lookup runs only after the receiver type is fully known (after deferred-receiver machinery settles).
- If both the receiver and its deref target have a member, the receiver's wins.
- This is why `shared[T]` implements `clone` — it provides `clone()` before looking through `T.clone()`, ensuring cloning the handle (incrementing the count) is the default behavior.

Deferred-capable sites (generic bodies, lambdas, literals) must not use deref for guessing; lookup happens only with a settled type.

**Lowering changes:**

When a field or method requires a deref, the lowerer inserts the `deref()` call before the ordinary access.

**Ownership changes:**

A call to `deref()` produces a borrow of the receiver. Call-result borrow provenance should cover this (the target is a type parameter of the impl), but verify with a reject test where the borrow outlives the handle.

**Diagnostics:**

- Assigning through a deref (e.g., `config.port = 1`) gets a dedicated error explaining `shared` is read-only and pointing to `mutex[T]` as the alternative.
- Lookup failures mention that both the handle type and its deref target were searched.
- Deref occurs in the note if the chosen member required it.

**Tests:**

- `accept_deref_field_access.cn`: accessing a field through `deref()`.
- `accept_deref_method_call.cn`: calling a method on the deref target.
- `accept_deref_precedence.cn`: receiver's own member wins over target's.
- `accept_deref_borrow_lifetime.cn`: the borrow from `deref()` is short-lived.
- `reject_assign_through_deref.cn`: cannot assign to a field reached through deref.
- `reject_deref_borrow_outlived.cn`: deref result cannot outlive the receiver.

## Phase 4: `shared[T]` in stdlib

Add a new `std.shared` module with the `shared[T]` type, replacing the builtin name.

**Type definition:**

```
pub type shared[T] = {
    inner: *mut shared_block[T]
}

machine type shared_block[T] = {
    count: usize
    value: T
}
```

**Methods:**

- `pub machine def new(value: T) -> shared[T]`: allocate and initialize.
- `impl clone for shared[T]`: atomic increment of count.
- `impl deref for shared[T]`: return `&value` from the block.
- `impl drop for shared[T]`: atomic decrement, and at zero, `drop_in_place` the value and free the block.

**Compiler changes:**

- Remove `shared` from `k_builtin_generic_arities` in `src/semantic/types.cpp`.
- Remove `shared` from `is_prelude_value_name` in `src/semantic/check.cpp`.
- Find and update any other references to the zero-argument arity of bare `shared expr`.
- Re-export from prelude.

**Tests:**

- `std_test/shared_new_and_clone.cn`: construct and clone handles.
- `std_test/shared_deref.cn`: field and method access through deref.
- `std_test/shared_drop.cn`: exact drop output — the value drops exactly once when the last handle is gone, even through moves and early returns.
- `std_test/shared_in_container.cn`: handles stored in a `list`.
- Layout test with `# expect:` to catch addressing and stride bugs.

**Status:** Type is real. Ownership and drop are integrated.

## Phase 5: `shared expr` syntax

The parser currently desugars `shared e` to `&e` (`src/parser/parser.cpp:4126`). Update it to call the stdlib constructor instead.

The type form `shared T` already parses as `shared[T]` and will resolve once the builtin entry is gone.

**Changes:**

- Parser: `shared e` becomes a call to the stdlib constructor.
- Ensure `src/testdata/parser_stress/091_stmt_if_let_shared_super.cn` still passes.

**Tests:**

- `parser_stress` fixture updates if needed.
- `accept_shared_expr.cn`: construction via `shared` syntax.

## Phase 6: Final tests and close-out

Add comprehensive tests that exercise the whole feature end-to-end.

**Tests:**

- Copy and adapt existing shared-related tests from `src/testdata/parser_stress/` and `src/testdata/std_test/`.
- Reject tests for mutation through `shared` and for a borrow from deref that outlives the handle.
- Tests for drop with cycles (documented as a known leak; weak handles are out of scope).

**Spec close-out:**

- Update ch17 (Shared Ownership and Drop) status from "Partial" to "Implemented".
- Remove todo item 7 from `spec/todo.md`.

## Risks

**Auto-deref and inference:** Deref lookup can make a method resolve differently while a receiver type is only partly known. Mitigation: lookup runs only after the receiver type is fully settled, never as a guess.

**Name collisions:** A method existing on both `shared[T]` and `T` resolves to the handle's. This is intentional (so `clone()` increments the count, not cloning `T`), and it needs explicit tests.

**Cycles:** `shared` values in a cycle leak because there is no weak handle. This is acceptable and must be documented. Weak handles are out of scope.

**Struct layout:** A struct's `size_of` is 8 (held by reference). Getting the `shared_block` layout right is the most likely place for a silent wrong-answer bug. Use `# expect:` in the layout test.

## Out of scope

- `mutex[T]`, `rwlock[T]`, `atomic[T]` (synchronization primitives).
- `send` and `share` concepts (concurrency guarante tracking).
- Weak handles (`weak[T]`).
- Using `deref` for `box[T]` (will come in a separate implementation plan).
- Automatic `clone` impls for `copy` types (defer to a later phase if needed).
