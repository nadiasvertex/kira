# 55. `std.console`

**Status:** Implemented

Standard stream handle accessors and the `print`/`println`/`eprint`/`eprintln` functions.

`std.console` (`src/std/console.kira`) is a thin layer over [`std.io`](54-std-io.md): every function here is a direct call into `std.io`'s handle constructors and `writer` trait.

## Stream handles

```kira
pub def stdout() -> std.io.file_handle
pub def stderr() -> std.io.file_handle
pub def stdin() -> std.io.file_handle
```

Each returns a fresh `std.io.file_handle` wrapping the corresponding standard descriptor, via `std.io.stdout_handle`/`stderr_handle`/`stdin_handle`.

## Printing

```kira
pub def print(s: str) -> unit
pub def println(s: str) -> unit
pub def eprint(s: str) -> unit
pub def eprintln(s: str) -> unit
```

- `print(s)` writes `s`'s UTF-8 bytes to `stdout()` via `write_all`, discarding the result.
- `println(s)` calls `print(s)` then `print("\n")` — two separate writes, not one.
- `eprint`/`eprintln` are the same pair against `stderr()`.

None of these four functions can report a write failure to the caller; a failing `write_all` is silently discarded. There is no error-propagating counterpart in this module — a caller that must observe I/O errors writes directly against a `std.io.file_handle` and its `writer` trait.

## See also

- [`std.io`](54-std-io.md) — `file_handle`, `writer`, `write_all`, the traits these functions are built on.
