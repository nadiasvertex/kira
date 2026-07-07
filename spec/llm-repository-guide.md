# Kira Repository Loading Guide

This document is for LLMs and new contributors who need a fast, high-signal map of the repository.

## Read First

1. `README.md` for the short project summary.
2. `spec/kira-reference.md` for language goals, semantics, and diagnostics philosophy.
3. `spec/CONVENTIONS.md` for C++ style, error-handling, and build expectations.
4. `MODULE.bazel` and `.justfile` for versioning, build entrypoints, and release automation.

## Current Scope

- The implemented compiler surface is a CLI compile driver plus a hand-written lexer and recursive-descent parser.
- There is no type checker, typed IR, LLVM lowering, runtime, or executable linker yet.
- The `//src:kira` binary currently loads source files, parses them, reports diagnostics, and writes protobuf-backed module metadata.

## High-Signal Directories

- `src/`: top-level CLI code, protobuf schema, and the current binary entrypoint.
- `src/parser/`: lexer, parser, AST, diagnostics, and parser regression tests.
- `spec/`: language reference, grammar sketch, coding conventions, and planning docs.
- `third_party/`: vendored code. Avoid it unless the task is explicitly about dependency maintenance.
- `bazel-*`, `external/`, `.cache/`, and `dist/`: generated or tool-owned outputs. Do not edit them by hand.

## Load Order By Task

### Parser or syntax work

1. `spec/kira-reference.md`
2. `spec/kira-grammar.ebnf`
3. `src/parser/token.h`
4. `src/parser/lexer.h`
5. `src/parser/source_location.h`
6. `src/parser/diagnostic.h`
7. `src/parser/ast.h`
8. `src/parser/parser.h`
9. `src/parser/parser.cpp`
10. `src/parser/parser_test.cpp`

### CLI or driver work

1. `src/cli.h`
2. `src/cli.cpp`
3. `src/module_metadata.proto`
4. `src/main.cpp`
5. `src/cli_test.cpp`
6. `src/BUILD.bazel`

### Build, release, or packaging work

1. `MODULE.bazel`
2. `.bazelrc`
3. `.justfile`
4. `README.md`

## Editing Guidance

- Add or update regression tests whenever parser behavior changes.
- Prefer small parser changes over introducing new abstractions.
- Keep diagnostics explicit: expected item, found item, explanation, and a useful fix.
- Do not assume vendored `third_party/argparse` is part of the active CLI path. The active CLI lives in `src/cli.cpp`.
- Module metadata persistence is protobuf-backed; evolve schemas additively for forward compatibility.
- Ignore the nested Bazel output directories under `src/parser/`; they are generated artifacts, not source-of-truth files.

## Verification Commands

- `just build`
- `just test`
- `just package`
- `bazelisk test //...`

## Current Anchors

- `src/parser/parser.cpp`: main parser implementation and the most active file for syntax work.
- `src/parser/parser_test.cpp`: named parser regressions and the best place to confirm intended behavior.
- `src/cli.cpp`: project-owned compile driver, diagnostics aggregation, and metadata writing.
- `src/module_metadata.proto`: persisted module metadata schema.
- `.justfile`: developer automation and packaging entrypoint.
