# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Kira is an early-stage language and compiler project written in C++26 (Clang 22.1+), built with Bazel/bzlmod. The implemented surface is: a hand-written lexer + recursive-descent parser, a semantic analysis pipeline (module graph, name resolution, type checking), and a CLI compile driver that emits protobuf-backed module metadata. There is no typed IR, LLVM lowering, or executable linker yet.

## Commands

```sh
just build      # bazelisk build //src:kira
just test       # bazelisk test //...
just run        # bazelisk run //src:kira
just package    # release archives (tar.bz2, .deb on Linux) under dist/
just format     # regenerate compile_commands.json, run clang-tidy --fix over src/
```

Run a single test target directly with bazel/bazelisk, e.g.:

```sh
bazelisk test //src/parser:parser_test
bazelisk test //src/semantic:check_test
bazelisk test //src:cli_test
```

Run the compiled binary against a source file:

```sh
bazelisk run //src:kira -- path/to/module.kira
bazelisk run //src:kira -- --metadata-dir build/meta path/to/module.kira
```

Tests are hand-rolled binaries (no gtest) using `kira::testing::expect`/`fail` from `src/testing/test_assert.h`; a failing `expect()` calls `std::exit(1)` with a message, so add new checks as additional `expect(...)` calls rather than introducing a framework.

## Architecture

The compiler above all is meant to teach the user how to use the language. Friendliness and clarity of diagnostics are prioritized over performance or cleverness.

- `src/parser/`: lexer, recursive-descent parser, AST, diagnostics, source locations. The lexer produces a flat, eagerly-materialized token stream (INDENT/DEDENT/NEWLINE synthesized from Python-style indentation; newlines suppressed inside balanced brackets). Tokens carry `string_view`s into the original source buffer — no per-token allocation. The parser never aborts on the first error: it inserts `error_node`s (`error_expr`, `error_pattern`, `error_stmt`) and resynchronizes at likely construct boundaries (keywords, DEDENT/NEWLINE) so downstream phases still see a structurally valid tree.
- `src/semantic/`: runs after parsing, over an in-memory set of `parsed_module` (file_id + borrowed AST pointer). Pipeline stages, roughly in order:
  - `analysis.cpp` / `resolution.cpp`: builds the cross-file module graph (`module_index.h`), detects duplicate/conflicting module paths, validates parent/child module boundaries, validates `use` imports and qualified path references.
  - `scopes.cpp` / `symbols.cpp` / `session.cpp`: builds the scope tree (`semantic_scope`, kinds like `module_scope`, `impl_scope`, `function_body_scope`, `match_arm_scope`, ...) and interned symbol table (`semantic_symbol`, namespaced by `symbol_namespace` — types/traits/submodules, values, type parameters, and associated types are looked up in separate namespaces so a type and value may share a spelling).
  - `types.cpp` / `check.cpp`: interns types into a `type_table` (`type_id` equality is id equality); `k_unknown_type` deliberately unifies with everything so one gap in knowledge doesn't cascade into unrelated errors, and `k_error_type` marks expressions where a diagnostic was already reported. `check_program` resolves names, infers/checks expression types, validates match exhaustiveness for sum types, and checks trait/impl coherence and `requires` obligations.
  - Files already marked failing in a `std::vector<bool> file_has_errors` are skipped by later stages so parse errors don't cascade into low-value semantic noise.
- `src/cli.cpp` + `src/main.cpp`: the compile driver — loads/parses files, runs the semantic pipeline, renders diagnostics, and writes protobuf module metadata (`src/module_metadata.proto`) under `kira-out/module-metadata/` (overridable via `--metadata-dir`). Do not assume the vendored `third_party/argparse` is on the active CLI path — the real CLI parsing lives in `src/cli.cpp`.
- Diagnostics (`diagnostic.h`) are first-class output, not an afterthought: every diagnostic level includes `Help`/`Note` because the compiler's stated philosophy ("compiler is a teacher") requires explaining what was expected, what was found, why, and how to fix it — apply this same standard when adding new diagnostics anywhere in the pipeline.
- `src/testdata/parser_stress/` and `src/testdata/semantic_stress/`: `.kira` corpora exercised by `driver_stress_test.cpp` and `semantic_stress_test.cpp` respectively (registered as Bazel `filegroup`s and consumed as test `data`).
- `spec/`: `kira-reference.md` (language reference, organized in Layer 1 Core / Layer 2 Intermediate / Layer 3 Advanced), `kira-grammar.ebnf` (grammar sketch), `CONVENTIONS.md` (authoritative C++ style rules), `llm-repository-guide.md` (per-task file load order), `llm-compiler-roadmap.md`.

## C++ Conventions

Full rules in `spec/CONVENTIONS.md`; the essentials:

- All names (including namespaces) are `snake_case`. `#pragma once`, not header guards.
- Functional-first: prefer pure free functions, value semantics, `const`/`constexpr`. One logical public entity per header; prefer forward declares.
- `std::expected<T, E>` for recoverable errors, `std::optional<T>` for "maybe" values; reserve exceptions for truly exceptional, non-local failures.
- `std::move` only when consuming a named object in the current scope; `std::forward` only in deduced forwarding-reference parameters; never `return std::move(local);` — let NRVO/copy elision work.
- No raw owning pointers or naked `new`/`delete`; prefer `std::unique_ptr`, `std::span`/views for non-owning access.
- Must build warning-free with `-std=c++2c -Wall -Wextra -Wconversion -Wsign-conversion -Wshadow -Wdeprecated -Wold-style-cast -Werror`.
- Prefer small, targeted parser/semantic changes over new abstractions; add or update a regression test whenever parser or semantic behavior changes.
