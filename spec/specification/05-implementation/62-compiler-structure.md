# 62. Compiler Structure

**Status:** Implemented

The layers of the reference compiler, in the order a compilation passes through them, and the data each layer hands to the next.

## Overview

The compiler is a single process that takes a set of source files and either runs a program on the bytecode VM or builds a native executable through LLVM. Between the source and either backend there are three representations: the AST, the checker's results (side tables keyed by AST node), and HIR, a typed tree that both backends consume.

```
source files
   │  parse (per file)
   ▼
AST ──► static-if import folding ──► module graph + name resolution
   │
   ▼
type checker ◄──► compile-time execution (HIR on the bytecode VM)
   │  checked results: types, resolved calls, instances, proofs, drop plans
   ▼
ownership check
   │
   ▼
HIR lowering ──► HIR passes (references, tail calls, inlining, ...)
   │
   ├──► bytecode compiler ──► bytecode VM            (cinder FILE)
   └──► LLVM codegen ──► object file ──► link        (cinder --compile FILE)
```

## Layers

1. **Driver** (`src/driver/`). Reads the command line, adds the standard library's `.cn` sources to the session, and runs each stage below. The standard library is compiled from source in every session, under the same rules as user code.
2. **Parser** (`src/parser/`). A hand-written lexer and recursive-descent parser produce one AST per file. The lexer synthesizes `INDENT`/`DEDENT`/`NEWLINE` tokens from indentation. The parser never stops at the first error; it inserts error nodes and resynchronizes, so later stages see a structurally valid tree.
3. **Import folding.** `static if` blocks that guard `use` declarations are evaluated before the module graph is built, so the selected imports become ordinary top-level items.
4. **Module graph and resolution** (`src/semantic/analysis.cpp`, `resolution.cpp`, `scopes.cpp`). Files are grouped into modules, module boundaries and imports are validated, and a scope tree and symbol table are built. Types, values, type parameters and associated types live in separate namespaces.
5. **Type checker** (`src/semantic/check*.cpp`, `src/semantic/infer/`). Resolves every name, infers and checks every expression's type, resolves every call to a specific function or instance, checks trait coherence and bounds, and discharges proof obligations. It runs compile-time code as it goes. See [Type Checking and Inference](63-type-checking-and-inference.md), [Proof Solving](64-proof-solving.md), and [Compile-Time Execution in the Compiler](65-compile-time-execution-in-the-compiler.md).
6. **Ownership check** (`src/semantic/ownership_check.cpp`, `ownership_cfg.cpp`). Builds a small control-flow graph of ownership events for each function and lambda body and enforces use-after-move, borrow exclusivity, and loan lifetimes without annotations ([Ownership and Borrowing](../02-intermediate/14-ownership-and-borrowing.md)). A `crew` body holds the borrows of every task spawned into it until the crew ends. It also rejects a write, `&mut`, `mut self` call, or move through a `static`, which it finds through the checker's `static_refs`, and a `&mut` or `mut self` call on a `let` binding or on a name a `match`, `if let`, `while let` or `for` pattern binds to a value it owns (the checker's `immutable_pattern_bindings`). The checker itself rejects assignments to them. Any write, `&mut`, or `mut self` call that reaches its target through a `&T`, a `*T`, or a shared view is rejected too; a `*mut` along the way makes what it points at writable again. Its maybe-moved facts also decide where drops and drop flags are needed.
7. **HIR lowering** (`src/hir/lower.cpp`). Turns each checked module into HIR: a tree in which every node carries a concrete type, every call names its target, and generics, operators, method calls, `for` loops, contracts, and drops have been made explicit. Lowering fails closed: a construct it cannot lower is reported, never skipped.
8. **HIR passes** (`src/hir/`). Make every borrow and dereference explicit, mark tail calls, compute lambda captures and generator state, check per-frame stack budgets, find the modules a program reaches, and inline small functions. These run once, before either backend, so both backends see the same program.
9. **Backends.** The bytecode compiler and VM ([HIR and the Bytecode VM](66-hir-and-bytecode-vm.md)) and LLVM code generation ([LLVM and Ahead-of-Time Compilation](67-llvm-and-aot.md)). Both read the same HIR and share heap layout rules from `src/runtime/layout.h`.

The checker also records the concrete fields of every struct instance and the concrete payloads of every sum instance (`checked_types::shapes`, `src/semantic/aggregate_shape.h`), with generic arguments substituted. `src/runtime/value_layout.h` computes byte layouts from them: inline members with offsets and padding, array strides, sum tag and payload offsets, and a diagnostic naming the field path of any type that contains itself by value. The backends do not read these layouts yet. They still use the pointer-per-aggregate rules in `layout.h`, and `size_of`/`align_of` still answer from it. Both models exist together while the backends migrate.

`src/hir/` also holds a storage form that states where each value lives: `storage.{h,cpp}` defines slots, places (a slot plus member, element, payload, or dereference projections), and the operations that build, copy, move, call into, and end them; `abi.{h,cpp}` classifies each type as zero-sized, scalar, address, handle, descriptor, or aggregate and derives internal signatures in which an aggregate result is written through a caller-supplied address; `storage_lower.{h,cpp}` lowers straight-line HIR into the form, forwarding destinations into constructors and calls; and `storage_verify.{h,cpp}` checks initialization, projections, transfers, storage lifetime, signature agreement, result completeness, and ownership. The form expresses `if`, `while`, `break`, `continue`, and early `return` as structured markers in a flat operation list (`if`/`else`/`end_if`, `loop`, `block`, `exit N`); the verifier joins initialization and drop state where paths meet and requires a loop body to end in the state it began with. The drop calls HIR synthesizes (`hir_call::is_drop`) stay calls on the address of a place; the verifier reads them against the checker's drop plans, so that each value's own drop and each droppable field's drop runs once, nothing is used after its drop, a replacement is written only after the old value's drops, and nothing droppable leaves its slot still owing a drop. Raw memory is expressed with a `pointer_offset` computation (`p + i * stride`, the stride being the element's size under the concrete layouts) followed by a dereference, so `p[i]` is a slot of inline bytes whatever the element type, and an array at a constant index is an `element` projection. Behind a raw pointer the verifier follows chapter 38 rather than the reference rules: a read moves or copies the value out and leaves the slot untracked, and a write never drops the old value. A `size_of` or `align_of` literal carries its question (`hir_literal::question`), and the storage lowerer asks it again of the concrete layouts, because HIR's own answer is for the legacy boxed aggregates. Functions with `match`, `while let`, `let else`, string literals, closures, indexing other than the two above, or generator bodies are refused by name. `and` and `or` lower to a conditional, so the right side runs only when the left does not decide the result. The bytecode compiler's `storage_compile.{h,cpp}` (chapter 66) and the LLVM compiler's `storage_codegen.{h,cpp}` (chapter 67) read the form, but the driver uses neither yet. `hir/intrinsic_abi.{h,cpp}` holds the check that a call to a runtime intrinsic agrees with the runtime symbol (`src/intrinsics.h` marks which symbols build their result in caller storage; `src/runtime/result_out.h` has the layout constants those `_into` functions write). The tests of both share `src/testing/storage_fixture.h` (a program lowered and verified) and `storage_cases.h`, `storage_intrinsic_cases.h`, and `storage_memory_cases.h` (value, intrinsic, and raw-memory cases with hand-computed expectations).

## Cross-cutting rules

- **Errors stop execution, not analysis.** A file with a parse error is skipped by later semantic stages so one mistake does not produce a cascade. A program with any error is never run or linked.
- **Diagnostics are part of the output.** Every stage reports what it expected, what it found, and how to fix it. Inference failures are explained from a retained record of constraints, not from whichever constraint failed last.
- **Whole-module compilation.** A module the program reaches is compiled in full, not only the functions it calls.
- **Module metadata.** After checking, the driver writes protobuf metadata for each module (`src/module_metadata.proto`) under `cinder-out/module-metadata/`, or the directory given by `--metadata-dir`.
- **One semantics, two executors.** Run-time and compile-time code both execute as HIR. The language's behavior is implemented once in lowering and twice in the backends, and the test suite cross-checks the two backends against each other and against expected values.

## See also

- [Compile-Time Semantics](../03-advanced/32-compile-time-semantics.md) — the language-level model that the checker and comptime VM implement.
