# Kira

Kira is an early-stage language and compiler project.

The repository currently contains:

- A language reference and grammar under `spec/`
- A hand-written lexer and recursive-descent parser under `src/parser/`
- A semantic analysis pipeline under `src/semantic/` (module graph, name resolution, type checking)
- A CLI compile driver under `src/` that emits protobuf-backed module metadata
- Regression test corpora under `src/testdata/`

## How to Use Kira

### Run a single-file script

By default, `kira` compiles a source file to bytecode and immediately executes it via the tier-0 VM — no build step needed:

```sh
kira path/to/module.kira
```

This runs the module's `main` function. Use `--run-function NAME` to execute a different entry point instead:

```sh
kira --run-function my_func path/to/module.kira
```

### AOT compile a Kira program

Pass `--compile` to compile the module to native code via LLVM and link a standalone executable instead of running it:

```sh
kira --compile path/to/module.kira
```

Use `--compile-output PATH` to control where the linked executable is written, and `--compile-function NAME` to choose a different entry point:

```sh
kira --compile --compile-output build/my_program --compile-function my_func path/to/module.kira
```

### Install Kira

Kira ships a `just install` recipe that builds a release binary and installs it, together with its runtime archives and standard library sources, into a self-contained prefix.

```sh
just install
```

This installs to `$HOME/.kira` by default. To install somewhere else, pass a path:

```sh
just install /usr/local/kira
```

This works the same way on macOS and Linux.

### Add Kira to your PATH

After installing, add the install prefix's `bin` directory to your shell's `PATH`. Assuming the default `$HOME/.kira` prefix:

**bash** (`~/.bashrc` or `~/.bash_profile`):

```sh
export PATH="$HOME/.kira/bin:$PATH"
```

**zsh** (`~/.zshrc`):

```sh
export PATH="$HOME/.kira/bin:$PATH"
```

**fish** (`~/.config/fish/config.fish`):

```fish
fish_add_path $HOME/.kira/bin
```

Restart your shell (or source the config file) afterward, then confirm it worked:

```sh
kira --help
```

## How to Build Kira

### Build

```sh
just build
```

### Test

```sh
just test
```

Run a single test target directly with `bazelisk` when you don't need the whole suite, e.g.:

```sh
bazelisk test //src/parser:parser_test
bazelisk test //src/semantic:check_test
bazelisk test //src:cli_test
```

### Run

```sh
just run path/to/module.kira
```

The driver currently:

- Loads and parses each source file
- Renders lexer/parser diagnostics
- Writes protobuf-encoded module metadata under `kira-out/module-metadata/` by default

Override the metadata output root with:

```sh
bazelisk run //src:kira -- --metadata-dir build/meta path/to/module.kira
```

Current project-owned tests cover:

- CLI argument parsing, compile-driver flow, and metadata emission
- Lexer indentation/dedent behavior
- Parser AST preservation for type bodies, associated types, `where` expressions,
  and pattern aliases

### Package

```sh
just package
```

Builds release archives (`.tar.bz2`, and `.deb` on Linux) under `dist/`.

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

The compiler implements a full pipeline from source to execution:

- Parsing: lexer + recursive-descent parser with error recovery
- Semantic analysis: module graph construction, name resolution, type checking, and diagnostics
- Lowering: HIR construction (`src/hir/`)
- Execution: tier-0 bytecode VM (default `kira` invocation) and LLVM-backed AOT native compilation (`--compile`), both currently limited to increment 1's scalar/control-flow subset of the language
- Output: protobuf-backed module metadata for each compiled module

Broader language coverage (beyond the scalar/control-flow subset) is still in progress.
