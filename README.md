# Kira

Kira is an early-stage language and compiler project.

The repository currently contains:

- A language reference and grammar under `spec/`
- A hand-written lexer and recursive-descent parser under `src/parser/`
- A semantic analysis pipeline under `src/semantic/` (module graph, name resolution, type checking)
- A CLI compile driver under `src/` that emits protobuf-backed module metadata
- Regression test corpora under `src/testdata/`

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
- `src/semantic/`: module graph, name resolution, scope and symbol tables, type checking
- `src/testdata/`: regression test corpora
- `spec/kira-reference.md`: language reference
- `spec/kira-grammar.ebnf`: grammar sketch
- `spec/CONVENTIONS.md`: project C++ conventions

## Status

The compiler implements a complete parse-and-analyze frontend:

- Parsing: lexer + recursive-descent parser with error recovery
- Semantic analysis: module graph construction, name resolution, type checking, and diagnostics
- Output: protobuf-backed module metadata for each compiled module

There is no typed IR, LLVM lowering, or executable linker yet.
