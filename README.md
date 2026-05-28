# Kira

Kira is an early-stage language and compiler project.

The repository currently contains:

- A language reference and grammar under `spec/`
- A hand-written lexer and recursive-descent parser under `src/k-parser/`
- A small CLI entrypoint under `src/`

## Build

```sh
bazel build //src:kira //src/k-parser:k-parser
```

## Test

```sh
bazel test //...
```

Current project-owned tests cover:

- CLI argument parsing and output formatting
- Lexer indentation/dedent behavior
- Parser AST preservation for type bodies, associated types, `where` expressions,
  and pattern aliases

## Layout

- `src/cli.*`: CLI parsing and rendering helpers
- `src/main.cpp`: binary entrypoint
- `src/k-parser/`: lexer, parser, diagnostics, AST, and parser tests
- `spec/kira-reference.md`: language reference
- `spec/kira-grammar.ebnf`: grammar sketch
- `spec/CONVENTIONS.md`: project C++ conventions

## Status

The parser library builds and has project-owned Bazel tests.
The CLI is still minimal and currently reports source files rather than running a
full compilation pipeline.
