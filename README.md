# Kira

Kira is an early-stage language and compiler project.

The repository currently contains:

- A language reference and grammar under `spec/`
- A hand-written lexer and recursive-descent parser under `src/parser/`
- A CLI compile driver under `src/`
- Protobuf-backed module metadata emitted after successful parses

## Build

```sh
bazel build //src:kira //src/parser:parser
```

## Test

```sh
bazel test //...
```

## Run

```sh
bazel run //src:kira -- path/to/module.kira
```

The driver currently:

- Loads and parses each source file
- Renders lexer/parser diagnostics
- Writes protobuf-encoded module metadata under `kira-out/module-metadata/` by default

Override the metadata output root with:

```sh
bazel run //src:kira -- --metadata-dir build/meta path/to/module.kira
```

Current project-owned tests cover:

- CLI argument parsing, compile-driver flow, and metadata emission
- Lexer indentation/dedent behavior
- Parser AST preservation for type bodies, associated types, `where` expressions,
  and pattern aliases

## Layout

- `src/cli.*`: CLI parsing and rendering helpers
- `src/module_metadata.proto`: protobuf schema for persisted module metadata
- `src/main.cpp`: binary entrypoint
- `src/parser/`: lexer, parser, diagnostics, AST, and parser tests
- `spec/kira-reference.md`: language reference
- `spec/kira-grammar.ebnf`: grammar sketch
- `spec/CONVENTIONS.md`: project C++ conventions

## Status

The parser library builds and has project-owned Bazel tests.
The current CLI is a frontend compile driver: it parses source files, reports
diagnostics, and emits protobuf-backed module metadata. Type checking, lowering,
and LLVM code generation still remain to be built.
