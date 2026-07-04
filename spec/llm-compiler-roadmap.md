# Kira Compiler Roadmap

This document is a planning snapshot for LLMs and contributors. It describes what exists now and what still needs to exist before Kira becomes a full LLVM-backed compiler.

## Non-Negotiable Goals

- Friendly, teaching-oriented diagnostics outrank raw compilation speed.
- The backend target is LLVM.
- Unoptimized compilation should be fast enough that `.kira` can replace small Python scripts in local workflows.
- Optimized binaries should eventually be competitive with well-built C++ and Rust programs.
- Every major compiler phase should preserve source spans so diagnostics stay precise.

## Current Status

### Implemented

- A hand-written lexer.
- A recursive-descent parser.
- AST coverage for functions, type bodies, associated types, inline `if`, inline `on`, guarded `for`, and pattern aliases.
- A CLI frontend that loads source files, parses them, reports diagnostics, and writes protobuf-backed module metadata.
- A shared multi-file `source_manager` used by parsing and cross-file diagnostics.
- Session-local module graph validation for duplicate module paths, parent/child module boundaries, and inline-vs-external submodule conflicts.
- Session-local import validation for owned module roots, unresolved imports, and child-module visibility.
- An initial semantic declaration-scope pass that rejects duplicate type, trait, concept, and submodule names within one module scope.
- Project-owned Bazel tests for CLI and parser regressions.
- A protobuf schema for persisted module metadata, intended to evolve additively for forward compatibility.

### Missing

- Name resolution and symbol tables.
- Type inference and type checking.
- Trait and impl validation.
- Typed lowering IR between the parser and backend.
- LLVM IR generation.
- Runtime and standard library integration.
- Object generation, linking, and executable production.

## Suggested Phase Order

### 1. Frontend stabilization

- Keep parser coverage growing with regression tests.
- Normalize AST shapes for later semantic passes.
- Harden recovery paths so one syntax error does not create many low-value follow-on errors.

Exit criteria:

- `bazelisk test //...` stays green after grammar additions.
- All syntax described in `spec/kira-reference.md` has parser coverage.

### 2. Source management and module graph

- Add file loading, module declarations, import resolution, and package boundaries.
- Define a canonical source manager shared by diagnostics and later passes.
- Expand persisted module metadata as the driver learns more semantic facts.

Exit criteria:

- The driver can load multiple files and report cross-file locations correctly.

Status:

- Complete for the current explicit CLI-input compilation model.
- Multi-file sessions, shared source management, cross-file module diagnostics, and session-local import validation are implemented and covered by project-owned tests.
- Full `project.kira` loading, `static search_path`, and dependency resolution remain later module-loader work, not a blocker for phase 3 under the current roadmap exit criteria.

### 3. Semantic analysis

- Build symbol tables and scope tracking.
- Add name resolution.
- Add type checking and inference for core expressions, functions, and pattern matching.
- Validate traits, impls, associated types, contracts, and `where` clauses.

Exit criteria:

- The compiler can reject ill-typed programs with specific, actionable diagnostics.

Status:

- Started.
- The current phase-3 entry point is module-local declaration scope validation for type/module names, which lays groundwork for symbol tables and later name resolution.
- The detailed phase-3 implementation backlog lives in `spec/phase-3.md`.

### 4. Typed lowering pipeline

- Introduce a typed HIR or MIR between the parser AST and LLVM.
- Lower sugar such as comprehensions, pattern aliases, and inline control flow into simpler forms.
- Record ownership and borrowing metadata once the language model needs it.

Exit criteria:

- Later passes operate on a reduced, type-checked IR instead of parser-shaped nodes.

### 5. LLVM code generation

- Lower typed IR to LLVM IR.
- Define the runtime boundary for strings, lists, sum types, and concurrency features.
- Emit object files and executables through an explicit driver path.

Exit criteria:

- Small end-to-end programs compile and run through LLVM.

### 6. Performance work

- Improve frontend throughput without regressing diagnostics quality.
- Add optimization profiles for fast edit-compile-run loops and optimized release builds.
- Benchmark generated code against equivalent C++ and Rust programs.

Exit criteria:

- Unoptimized `kira` builds feel script-friendly.
- Optimized binaries are within an acceptable range of C++ and Rust baselines.

## Architectural Guidance

- Keep the parser AST and typed IR separate.
- Preserve source spans through every lowering step.
- Prefer explicit phase boundaries over a monolithic compiler pass.
- Add project-owned tests at every stage before broadening surface area.
- Use LLVM as the backend, but keep Kira-specific diagnostics and semantics above the LLVM layer.

## Near-Term Work

- Finish parser and spec alignment for the remaining syntax in `spec/kira-reference.md`.
- Expand the current parse driver into a true multi-phase compiler driver.
- Design module loading and a shared diagnostics/source manager.
- Pick the first typed IR boundary and document it before implementing code generation.

## What Not To Assume Yet

- There is no stable runtime ABI.
- There is no settled ownership or borrow-checking implementation.
- There is no finalized optimization pipeline.
- The current `kira` binary is not yet a full compiler.
