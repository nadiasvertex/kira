# 54. `std.io`

**Status:** Implemented

Byte-level I/O: error types, the `reader`/`writer` traits, `open_options`, and the `file` handle type.

`std.io` (`src/std/io.kira`) is the foundation `std.console` and `std.fs.path` build on: `std.console` implements printing in terms of `file`'s `writer` impl, and `std.string` builds on `std.io`'s `writer` trait for its own formatting entry points.

## Types

```kira
pub type raw_fd = { value: int64 }
pub type io_errno = { code: int32 }
pub type io_error = @not_found(str) | @permission_denied(str) | @already_exists(str)
                   | @interrupted | @would_block | @broken_pipe | @unexpected_eof | @other(io_errno)
```

- `raw_fd` is an opaque wrapper around a native file descriptor, for internal use.
- `io_errno` carries a raw system error code as returned by an intrinsic.
- `io_error` is the public error type: a closed sum of the operationally relevant POSIX errno cases, plus `@other(io_errno)` as a catch-all.

### `io_errno` to `io_error` conversion

```kira
impl from[io_errno] for io_error:
    def from(e: io_errno) -> io_error
```

Maps raw error codes to variants by numeric code: `2` → `@not_found`, `13` → `@permission_denied`, `17` → `@already_exists`, `4` → `@interrupted`, `11` → `@would_block`, `32` → `@broken_pipe`; every other code becomes `@other(e)`. Every intrinsic in this module returns `result[T, io_errno]`; every public wrapper function converts that to `result[T, io_error]` through this `from` impl at the boundary — the pattern used throughout the standard library for propagating raw OS errors (see [`std.platform`](56-std-platform.md)).

### `open_options`

```kira
pub type open_options = {
    read: bool,
    write: bool,
    append: bool,
    create: bool,
    truncate: bool
}
```

Controls the mode `open` uses when opening a file; all fields are plain booleans, combined by the underlying `rt_open` intrinsic.

## Traits

### `reader`

```kira
pub trait reader:
    def read(mut self, buf: &mut slice_mut[byte]) -> result[usize, io_error]
    def read_to_end(mut self, out: &mut list[byte]) -> result[usize, io_error]
    def read_exact(mut self, buf: &mut slice_mut[byte]) -> result[unit, io_error]
```

- `read` is the sole required method: reads up to `buf.len()` bytes, returns the count actually read.
- `read_to_end` is a provided default: loops `read` into a 4096-byte stack buffer, appending each chunk to `out`, until a zero-length read signals end of file. Returns the total byte count.
- `read_exact` is a provided default: loops `read` until `buf` is completely filled. A zero-length read before the buffer fills is reported as `@err(@unexpected_eof)`.

### `writer`

```kira
pub trait writer:
    def write(mut self, buf: &slice[byte]) -> result[usize, io_error]
    def flush(mut self) -> result[unit, io_error]
    def write_all(mut self, buf: &slice[byte]) -> result[unit, io_error]
```

- `write` and `flush` are required. `write` writes up to `buf.len()` bytes and returns the count written; `flush` forces any buffered data to the destination.
- `write_all` is a provided default: loops `write` until every byte of `buf` is written. A zero-length write is reported as `@err(@broken_pipe)`.

Both traits take `self` by `mut self` (moved) on every method, including the provided defaults — a reader or writer is consumed and re-threaded through calls, not shared by reference.

## Intrinsics

```kira
intrinsic def rt_stdin() -> raw_fd
intrinsic def rt_stdout() -> raw_fd
intrinsic def rt_stderr() -> raw_fd

intrinsic def rt_open(path: str, opts: open_options) -> result[raw_fd, io_errno]
intrinsic def rt_close(fd: raw_fd) -> result[unit, io_errno]
intrinsic def rt_read(fd: raw_fd, buf: &mut slice_mut[byte]) -> result[usize, io_errno]
intrinsic def rt_write(fd: raw_fd, buf: &slice[byte]) -> result[usize, io_errno]
intrinsic def rt_flush(fd: raw_fd) -> result[unit, io_errno]
```

Native entry points, implemented on both backends (`src/runtime/io.cpp`, `src/bytecode/vm.cpp`, `src/llvm_codegen/codegen.cpp`) via the shared intrinsic-dispatch table (`src/intrinsics.h`). None of these is called directly by ordinary code; `file` and the module-level `open`/`stdin_handle`/`stdout_handle`/`stderr_handle` functions wrap them.

## The `file` type

```kira
pub type file = { fd: raw_fd }
```

A handle wrapping a `raw_fd`.

```kira
pub def open(path: str, opts: open_options) -> result[file, io_error]
pub def stdin_handle() -> file
pub def stdout_handle() -> file
pub def stderr_handle() -> file
```

- `open` calls `rt_open` and wraps the resulting descriptor in a `file`, converting any `io_errno` to `io_error`.
- `stdin_handle`/`stdout_handle`/`stderr_handle` wrap the corresponding niladic intrinsic; these never fail.

### Trait implementations

- `impl reader for file`: `read` delegates to `rt_read`, converting the error.
- `impl writer for file`: `write` delegates to `rt_write`; `flush` delegates to `rt_flush`; both convert the error. `write_all` is inherited from the trait default.
- `impl drop for file`: calls `rt_close` on drop, discarding the result — a `file` going out of scope closes its descriptor unconditionally.

### `extend file`

```kira
pub def close(mut self) -> result[unit, io_error]
```

Explicitly closes the handle via `rt_close`, sets `self.fd` to the sentinel `raw_fd { value: -1 }`, and returns the result (unlike `drop`, which discards it). Intended for callers that need to observe a close failure; a `file` not explicitly closed is still closed by `drop`.

## See also

- [`std.console`](55-std-console.md) — `print`/`println`/`eprint`/`eprintln`, built on `file`'s `writer` impl.
- [`std.platform`](56-std-platform.md) — reuses the same `io_errno`-to-`io_error` conversion pattern for OS-level queries.
