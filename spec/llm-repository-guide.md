# Kira Repository Loading Guide

This document is for LLMs and new contributors who need a fast, high-signal map of the repository.

## Read First

1. `README.md` for the short project summary.
2. `spec/kira-reference.md` for language goals, semantics, and diagnostics philosophy.
3. `spec/CONVENTIONS.md` for C++ style, error-handling, and build expectations.
4. `MODULE.bazel` and `.justfile` for versioning, build entrypoints, and release automation.

## Current Scope

- The implemented compiler surface is a CLI plus a hand-written lexer and recursive-descent parser.
- There is no type checker, typed IR, LLVM lowering, runtime, or executable linker yet.
- The `//src:kira` binary currently parses CLI arguments and prints source summaries. It does not compile Kira source files yet.

## High-Signal Directories

- `src/`: top-level CLI code and the current binary entrypoint.
- `src/k-parser/`: lexer, parser, AST, diagnostics, and parser regression tests.
- `spec/`: language reference, grammar sketch, coding conventions, and planning docs.
- `third_party/`: vendored code. Avoid it unless the task is explicitly about dependency maintenance.
- `bazel-*`, `external/`, `.cache/`, and `dist/`: generated or tool-owned outputs. Do not edit them by hand.

## Load Order By Task

### Parser or syntax work

1. `spec/kira-reference.md`
2. `spec/kira-grammar.ebnf`
3. `src/k-parser/token.h`
4. `src/k-parser/lexer.h`
5. `src/k-parser/source_location.h`
6. `src/k-parser/diagnostic.h`
7. `src/k-parser/ast.h`
8. `src/k-parser/parser.h`
9. `src/k-parser/parser.cpp`
10. `src/k-parser/parser_test.cpp`

### CLI or driver work

1. `src/cli.h`
2. `src/cli.cpp`
3. `src/main.cpp`
4. `src/cli_test.cpp`
5. `src/BUILD.bazel`

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
- Ignore the nested Bazel output directories under `src/k-parser/`; they are generated artifacts, not source-of-truth files.

## Verification Commands

- `just build`
- `just test`
- `just package`
- `bazelisk test //...`

## Current Anchors

- `src/k-parser/parser.cpp`: main parser implementation and the most active file for syntax work.
- `src/k-parser/parser_test.cpp`: named parser regressions and the best place to confirm intended behavior.
- `src/cli.cpp`: project-owned argument parsing.
- `.justfile`: developer automation and packaging entrypoint.
