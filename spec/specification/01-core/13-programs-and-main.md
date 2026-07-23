# 13. Programs and `main`

**Status:** Partial — see Implementation status

Covers `main`, `result` return for `?` inside `main`, `args()`/`env()`, and the script form (implicit `main` from top-level statements).

## `main`

An executable program has one entry point: a function named `main`. The simplest form takes no arguments and returns `unit`.

```kira
def main() -> unit:
    println("Hello, world!")
```

To use `?` inside `main`, give it a `result` return type; a returned `@err` ends the program with a non-zero exit status and reports the error.

```kira
def main() -> result[unit, app_error]:
    let cfg = load_config("app.toml")?
    run(cfg)
    return @ok(unit)
```

## `args()` and `env()`

Command-line arguments and environment variables come from prelude intrinsic functions rather than `main` parameters, so `main`'s signature stays uniform. Both are recognized prelude names in `src/semantic/check.cpp` (`name == "args"`, `name == "env"`).

```kira
let arguments = args()        # list[str]
let home      = env("HOME")   # option[str]
```

## Scripts

A file may skip the `main` declaration; top-level statements in that file are then treated as the body of an implicit `main`, run in order. A file may declare `main` explicitly or use top-level statements, but not both — mixing the two is a compile error (`` a file may declare `main` explicitly or use top-level statements, but not both ``, checked in `src/semantic/check.cpp`).

```kira
module hello

for name in args():
    println("Hi, {name}")
```

## Implementation status

- The explicit/implicit-`main` mutual-exclusion check is implemented and diagnosed (`src/semantic/check.cpp`, the "top-level statement in a file that declares `main`" check).
- No test corpus file under `src/testdata/` (parser_stress, semantic_stress, codegen_test, codegen_stress, driver_stress) exercises the *script* form end to end — every sample program in those corpora declares `main` explicitly. Whether top-level statements actually lower and run as an implicit `main` (as opposed to only being parsed and checked for the mutual-exclusion rule) is unverified against the compiled pipeline.
- The old tutorial's claim that entry-module selection is driven by a `project.kira` file is not implemented: no `project.kira` file, parser, or reference to one exists anywhere in `src/`. The current CLI (`src/driver/cli.cpp`) instead takes a source file path directly as a positional argument (`bazelisk run //src:kira -- path/to/module.kira`), with `--compile-function NAME` to select a non-`main` entry point for `--compile`. A library-vs-program distinction driven by project configuration is design-only at this point.

## See also

- [Error Handling](11-error-handling.md) — `?` and `result`, used by `main`'s `result` return form.
- [Modules and Imports](12-modules-and-imports.md) — the module a file belongs to.
