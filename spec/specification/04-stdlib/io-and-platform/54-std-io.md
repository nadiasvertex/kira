# 54. `std.io`

**Status:** Partial

Byte-level I/O: error types, the `reader`/`writer` traits, `open_options`, the `file_handle` type, pipes, and asynchronous I/O for `io` tasks.

`std.io` (`src/std/io.cn`) is the foundation `std.console` and `std.fs.path` build on: `std.console` implements printing in terms of `file_handle`'s `writer` impl, and `std.string` builds on `std.io`'s `writer` trait for its own formatting entry points.

## Types

```cinder
pub type raw_fd = { value: int64 }
pub type io_errno = { code: int32 }
pub type io_error = @not_found(str) | @permission_denied(str) | @already_exists(str)
                   | @interrupted | @would_block | @broken_pipe | @unexpected_eof | @other(io_errno)
```

- `raw_fd` is an opaque wrapper around a native file descriptor, for internal use.
- `io_errno` carries a raw system error code as returned by an intrinsic.
- `io_error` is the public error type: a closed sum of the operationally relevant POSIX errno cases, plus `@other(io_errno)` as a catch-all.

### `io_errno` to `io_error` conversion

```cinder
impl from[io_errno] for io_error:
    def from(e: io_errno) -> io_error
```

Maps raw error codes to variants by numeric code: `2` → `@not_found`, `13` → `@permission_denied`, `17` → `@already_exists`, `4` → `@interrupted`, the host's `EAGAIN` → `@would_block`, `32` → `@broken_pipe`; every other code becomes `@other(e)`. `EAGAIN` is 11 on Linux and 35 on macOS, so it is read from `rt_errno_would_block()` rather than written as a number. Every intrinsic in this module returns `result[T, io_errno]`; every public wrapper function converts that to `result[T, io_error]` through this `from` impl at the boundary — the pattern used throughout the standard library for propagating raw OS errors (see [`std.platform`](56-std-platform.md)).

### `open_options`

```cinder
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

```cinder
pub trait reader:
    def read(mut self, buf: &mut slice_mut[byte]) -> result[usize, io_error]
    def read_to_end(mut self, out: &mut list[byte]) -> result[usize, io_error]
    def read_exact(mut self, buf: &mut slice_mut[byte]) -> result[unit, io_error]
```

- `read` is the sole required method: reads up to `buf.len()` bytes, returns the count actually read.
- `read_to_end` is a provided default: loops `read` into a 4096-byte stack buffer, appending each chunk to `out`, until a zero-length read signals end of file. Returns the total byte count.
- `read_exact` is a provided default: loops `read` until `buf` is completely filled. A zero-length read before the buffer fills is reported as `@err(@unexpected_eof)`.

### `writer`

```cinder
pub trait writer:
    def write(mut self, buf: &slice[byte]) -> result[usize, io_error]
    def flush(mut self) -> result[unit, io_error]
    def write_all(mut self, buf: &slice[byte]) -> result[unit, io_error]
```

- `write` and `flush` are required. `write` writes up to `buf.len()` bytes and returns the count written; `flush` forces any buffered data to the destination.
- `write_all` is a provided default: loops `write` until every byte of `buf` is written. A zero-length write is reported as `@err(@broken_pipe)`.

Both traits take `self` by `mut self` (moved) on every method, including the provided defaults — a reader or writer is consumed and re-threaded through calls, not shared by reference.

## Intrinsics

```cinder
intrinsic def rt_stdin() -> raw_fd
intrinsic def rt_stdout() -> raw_fd
intrinsic def rt_stderr() -> raw_fd

intrinsic def rt_open(path: str, opts: open_options) -> result[raw_fd, io_errno]
intrinsic def rt_close(fd: raw_fd) -> result[unit, io_errno]
intrinsic def rt_read(fd: raw_fd, buf: &mut slice_mut[byte]) -> result[usize, io_errno]
intrinsic def rt_write(fd: raw_fd, buf: &slice[byte]) -> result[usize, io_errno]
intrinsic def rt_flush(fd: raw_fd) -> result[unit, io_errno]

pub type raw_pipe = { read: int64, write: int64 }
intrinsic def rt_pipe() -> result[raw_pipe, io_errno]
intrinsic def rt_set_nonblocking(fd: raw_fd) -> result[unit, io_errno]
intrinsic def rt_errno_would_block() -> int32

intrinsic def rt_fd_readable(fd: int64) -> task[unit]
intrinsic def rt_fd_writable(fd: int64) -> task[unit]
```

`rt_pipe` makes a pipe whose ends are closed when the program runs another one. `rt_set_nonblocking` sets `O_NONBLOCK` on a descriptor. `rt_fd_readable` and `rt_fd_writable` return a task that finishes when the descriptor is ready, from the event loop behind the `io` context ([The Task Scheduler](../../05-implementation/69-task-scheduler.md#the-io-event-loop)). An error or hang-up on the descriptor also finishes it, so the read or write that follows reports the condition. A regular file, a directory, or a descriptor that is not open finishes it at once.

Native entry points, implemented on both backends (`src/runtime/io.cpp`, `src/bytecode/vm.cpp`, `src/llvm_codegen/codegen.cpp`) via the shared intrinsic-dispatch table (`src/intrinsics.h`). None of these is called directly by ordinary code; `file_handle` and the module-level `open`/`stdin_handle`/`stdout_handle`/`stderr_handle` functions wrap them.

## The `file_handle` type

```cinder
pub type file_handle = { fd: raw_fd, owns: bool }
```

A handle wrapping a `raw_fd`. `owns` says whether the handle owns the descriptor, and so whether dropping it closes the descriptor. (Named `file_handle` rather than `file` because `file` is a reserved visibility keyword — see [Modules and Imports](../../01-core/12-modules-and-imports.md).)

```cinder
pub def open(path: str, opts: open_options) -> result[file_handle, io_error]
pub def stdin_handle() -> file_handle
pub def stdout_handle() -> file_handle
pub def stderr_handle() -> file_handle
```

- `open` calls `rt_open` and wraps the resulting descriptor in a `file_handle` that owns it, converting any `io_errno` to `io_error`.
- `stdin_handle`/`stdout_handle`/`stderr_handle` wrap the corresponding niladic intrinsic in a handle that does not own it, so dropping one (for example the temporary in `stdout_handle().write_all(...)`) leaves the standard stream open; these never fail.

### Trait implementations

- `impl reader for file_handle`: `read` delegates to `rt_read`, converting the error.
- `impl writer for file_handle`: `write` delegates to `rt_write`; `flush` delegates to `rt_flush`; both convert the error. `write_all` is inherited from the trait default.
- `impl drop for file_handle`: calls `rt_close` on drop when the handle owns its descriptor, discarding the result.

### `extend file_handle`

```cinder
pub def close(mut self) -> result[unit, io_error]
```

Explicitly closes the handle via `rt_close`, sets `self.fd` to the sentinel `raw_fd { value: -1 }` and `owns` to false, and returns the result (unlike `drop`, which discards it). Intended for callers that need to observe a close failure. A `file_handle` not explicitly closed is closed by `drop`.

## Pipes

```cinder
pub type pipe_ends = { reader: file_handle, writer: file_handle }
pub def pipe() -> result[pipe_ends, io_error]
```

`pipe` makes a pipe with blocking ends: bytes written to `writer` are read from `reader`. Each end owns its descriptor. A read returns zero once every copy of the write end is closed.

## Asynchronous I/O

An `io` task that reads or writes a descriptor that is not ready suspends instead of blocking its worker thread. The descriptor is non-blocking, so a read or write that cannot proceed fails with `EAGAIN`; the task then awaits the descriptor's readiness and tries again. Other tasks run on the thread in the meantime.

```cinder
pub async def readable(fd: &raw_fd) -> unit
pub async def writable(fd: &raw_fd) -> unit
```

`readable` and `writable` suspend the calling task until the descriptor can be read or written without blocking, or has an error or hang-up. They return at once for a regular file, which never blocks.

### `async_reader` and `async_writer`

```cinder
pub trait async_reader:
    async def read(mut self, buf: &mut slice_mut[byte]) -> result[usize, io_error]
    async def read_to_end(mut self, out: &mut list[byte]) -> result[usize, io_error]
    async def read_exact(mut self, buf: &mut slice_mut[byte]) -> result[unit, io_error]

pub trait async_writer:
    async def write(mut self, buf: &slice[byte]) -> result[usize, io_error]
    async def flush(mut self) -> result[unit, io_error]
    async def write_all(mut self, buf: &slice[byte]) -> result[unit, io_error]
```

These mirror `reader` and `writer`. `read` and `write` are required; `read` suspends until at least one byte or the end of the input is available, and `write` until at least one byte can be written. The provided defaults loop as their blocking counterparts do: `read_to_end` until a zero-length read, `read_exact` until the buffer is full (`@unexpected_eof` if the input ends first), and `write_all` until every byte is written (`@broken_pipe` if a write accepts nothing).

### `async_file`

```cinder
pub type async_file = { fd: raw_fd, owns: bool }

pub async def open_async(path: str, opts: open_options) -> result[async_file, io_error]
pub def into_async(f: file_handle) -> result[async_file, io_error]
pub type async_pipe_ends = { reader: async_file, writer: async_file }
pub def async_pipe() -> result[async_pipe_ends, io_error]
```

`async_file` is a handle whose descriptor is non-blocking. It implements `async_reader` and `async_writer`, and `drop` closes the descriptor when `owns` is set. `close(mut self)` closes it explicitly and reports the result, as for `file_handle`. `read` and `write` retry after `@interrupted`.

- `open_async` opens a file with a non-blocking descriptor. The open itself does not suspend. Opening a FIFO does not wait for its other end, and opening one for writing fails with `@other` when nothing has it open for reading.
- `into_async` makes an existing handle's descriptor non-blocking and moves it into an `async_file`. The descriptor's file description is shared with every copy of it, so making a standard stream asynchronous also changes it for the process that started this one.
- `async_pipe` makes a pipe whose ends are both non-blocking.

```cinder
use std.io.{async_pipe, async_file, async_reader, async_writer}

async def produce(w: async_file) -> unit:
    var out = w
    let bytes: array[byte, 3] = [1, 2, 3]
    let _ = await out.write_all(&bytes[0..3])

async def consume(r: async_file) -> usize:
    var inp = r
    var got: list[byte] = []
    return match await inp.read_to_end(&mut got):
        @ok(n) => n
        @err(_) => 0

async def main() -> int32:
    match async_pipe():
        @ok(ends) =>:
            let (n, _) = await par:
                consume(ends.reader)
                produce(ends.writer)
            return n as int32
        @err(_) => return 1
```

The consumer starts first and suspends on the empty pipe. It sees the end of the input when the producer's end is dropped.

## Implementation status

Everything in this chapter is implemented in `src/std/io.cn` over the `rt_*` intrinsics, on both backends.

- A handle is dropped where [Shared Ownership and Drop](../../02-intermediate/17-shared-ownership-and-drop.md) says values drop; the cases that still leak are listed in items 17 and 18 of [todo.md](../../../todo.md).

## See also

- [`std.console`](55-std-console.md) — `print`/`println`/`eprint`/`eprintln`, built on `file_handle`'s `writer` impl.
- [`std.platform`](56-std-platform.md) — reuses the same `io_errno`-to-`io_error` conversion pattern for OS-level queries.
