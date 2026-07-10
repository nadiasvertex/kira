# Standard Library: `io`, `format`, `console`

This document specifies the first three standard library modules: `std.io`,
`std.format`, and `std.console`. It also tracks the compiler and language
work each one depends on (see "Compiler and Language Changes Required"
below) — several pieces of this design do not run yet because the compiler
phase they need does not exist yet.

## Design Principles

- **Thin, syscall-shaped intrinsics.** The only native surface is a handful
  of primitive operations (open/close/read/write/flush + the three standard
  streams). Everything else — buffering, formatting, the `reader`/`writer`
  trait machinery — is ordinary Kira source, shared by every backend.
- **One backend, one implementation of each intrinsic.** The bytecode
  interpreter implements intrinsics directly with C++ standard library calls
  in the VM. The AOT/LLVM backend declares each intrinsic as an external
  C-ABI symbol and links against a small native runtime library that
  implements it. Both read from one shared intrinsic schema so they cannot
  drift apart (see below).
- **Errors are data, classified once.** Intrinsics return the raw OS error
  code (`io_errno`); the Kira-side `io` module is the only place that turns
  codes into the friendly `io_error` sum type, via `impl from[io_errno] for
  io_error`. Native code never has to know about Kira's error types.
- **`show` stays the default formatter.** `std.format` does not introduce a
  second formatting mechanism — it specifies the mini-language inside `{...}`
  interpolation and lets types opt in to width/precision/fill handling via
  per-capability traits; a type that only implements `show` still works
  everywhere. The full design lives in `spec/string-formatting-design.md`.

---

## Intrinsics

Eight intrinsics, all syscall-shaped. `raw_fd` is an opaque handle (not a
bare POSIX integer) so the AOT runtime is free to store a platform handle
(e.g. a Windows `HANDLE`) in it without changing the ABI shape.

```kira
intrinsic def rt_stdin()  -> raw_fd
intrinsic def rt_stdout() -> raw_fd
intrinsic def rt_stderr() -> raw_fd

intrinsic def rt_open(path: str, opts: open_options) -> result[raw_fd, io_errno]
intrinsic def rt_close(fd: raw_fd) -> result[unit, io_errno]
intrinsic def rt_read(fd: raw_fd, buf: slice_mut[byte]) -> result[usize, io_errno]
intrinsic def rt_write(fd: raw_fd, buf: slice[byte]) -> result[usize, io_errno]
intrinsic def rt_flush(fd: raw_fd) -> result[unit, io_errno]
```

An `intrinsic def` has a signature and no body. It is not part of the public
API of any stdlib module — `std.io` is the only thing that calls these
directly, and it wraps them in `reader`/`writer`/`file`. No `seek` yet; add
it once something needs random access.

### Shared intrinsic schema

Both backends (bytecode VM, LLVM/AOT) and the semantic checker's intrinsic
registry must agree on the same (name, id, signature) table. This table
should be defined exactly once — e.g. as an X-macro list — and consumed by:

- the semantic checker, to validate `intrinsic def` signatures and assign a
  stable numeric id used by the bytecode compiler,
- the bytecode VM's dispatch table (`op_call_intrinsic` indexes into an
  array of C++ function pointers),
- the LLVM codegen's `declare` list and the native runtime library's symbol
  names (e.g. `kira_rt_write`).

A hand-maintained duplicate list in each backend is exactly the kind of
drift this schema exists to prevent.

---

## `std.io`

```kira
module std.io

pub type raw_fd = { value: int64 }
pub type io_errno = { code: int32 }

pub type io_error =
    | @not_found(str)
    | @permission_denied(str)
    | @already_exists(str)
    | @interrupted
    | @would_block
    | @broken_pipe
    | @unexpected_eof
    | @other(io_errno)

impl from[io_errno] for io_error:
    def from(e: io_errno) -> io_error: ...   # maps OS codes to variants

pub type open_options = {
    read:     bool,
    write:    bool,
    append:   bool,
    create:   bool,
    truncate: bool,
}

pub trait reader:
    def read(mut self, buf: slice_mut[byte]) -> result[usize, io_error]

    def read_to_end(mut self, out: mut list[byte]) -> result[usize, io_error]:
        ...   # default, built on read()

    def read_exact(mut self, buf: slice_mut[byte]) -> result[unit, io_error]:
        ...   # default, built on read()

pub trait writer:
    def write(mut self, buf: slice[byte]) -> result[usize, io_error]
    def flush(mut self) -> result[unit, io_error]

    def write_all(mut self, buf: slice[byte]) -> result[unit, io_error]:
        ...   # default, loops write() until buf is consumed

pub type file = { fd: raw_fd }

pub def open(path: str, opts: open_options) -> result[file, io_error]: ...

impl reader for file:
    def read(mut self, buf: slice_mut[byte]) -> result[usize, io_error]:
        rt_read(self.fd, buf).map_err(io_error.from)

impl writer for file:
    def write(mut self, buf: slice[byte]) -> result[usize, io_error]:
        rt_write(self.fd, buf).map_err(io_error.from)
    def flush(mut self) -> result[unit, io_error]:
        rt_flush(self.fd).map_err(io_error.from)

impl drop for file:
    def drop(mut self) -> unit:
        _ = rt_close(self.fd)   # best-effort — a drop can't hand back an error

# offered for callers who need to *observe* a close failure
pub def (file).close(mut self) -> result[unit, io_error]:
    let r = rt_close(self.fd)
    self.fd = closed_sentinel   # drop() then no-ops instead of double-closing
    return r.map_err(io_error.from)
```

`file` relies on the `drop` trait (see `spec/kira-reference.md`, "Destructors:
`drop`") for automatic cleanup — no mandatory explicit close. The same
best-effort-drop-plus-explicit-fallible-method pattern should be reused by
any future buffered writer, since a drop-only flush that silently swallows
errors is a well-known footgun (Rust's `BufWriter` has exactly this
problem).

---

## `std.format`

Builds on the prelude's `show` trait rather than replacing it. The full
design — the `{expr:spec}` mini-language, the per-capability `show`/`debug`/
`hex`/`octal`/`binary` traits, the `format_spec` shape, and the
sign/prefix-aware padding rules — is specified in full in
`spec/string-formatting-design.md`; that document is authoritative for this
module's contents, superseding the earlier `format_with`/single-`format_spec`
sketch that used to live here.

```kira
module std.format

pub type align_mode = @left | @right | @center
pub type sign_mode   = @always | @negative_only | @space
pub type format_spec = { ... }   # see spec/string-formatting-design.md

pub def pad_str(s: str, spec: format_spec) -> str
pub def pad_integral(negative: bool, prefix: str, digits: str, spec: format_spec) -> str
```

`show`, `debug`, `hex`, `octal`, and `binary` are prelude traits (not
`std.format`-scoped) so that `impl show for T` keeps working without a
`use` of this module — `std.format` supplies the `format_spec` type and the
`pad_str`/`pad_integral` helpers the compiler's generated formatting code
calls into.

---

## `std.console`

Thin — sits entirely on `io::writer`/`io::reader`.

```kira
module std.console

use std.io.{ file, writer, reader }

pub def stdout() -> file
pub def stderr() -> file
pub def stdin()  -> file

pub def print(s: str) -> unit:    stdout().write_all(s.as_bytes())
pub def println(s: str) -> unit:  print(s); print("\n")
pub def eprint(s: str) -> unit:   stderr().write_all(s.as_bytes())
pub def eprintln(s: str) -> unit: eprint(s); eprint("\n")
```

The prelude's `print`/`println` (see `spec/kira-reference.md`, "The
Prelude") re-export these.

---

## Compiler and Language Changes Required

`std.format` still cannot, entirely blocked on `{expr:spec}` interpolation splitting. This is the 
checklist of compiler work it depends on, grouped by phase.

### Semantic analysis

- [ ] **Blocked:** automatically invoking `drop()` at scope exit requires
      knowing whether a binding was moved from, which requires move/ownership
      tracking. The roadmap's "Missing" list already calls out that borrow
      checking and full move tracking do not exist yet
      (`spec/llm-compiler-roadmap.md`). Scope-exit `drop` calls should not be
      implemented ahead of that tracking — doing so risks either double-drop
      or use-after-drop bugs that look like compiler bugs to users, which
      cuts directly against the "compiler is a teacher" project goal.
- [ ] `{expr:spec}` interpolation splitting: today the lexer emits the whole
      string as one token and nothing downstream splits it
      (`src/parser/text_escape.h`); the format-spec mini-language depends
      on this splitting existing. See `spec/string-formatting-design.md`
      ("Implementation Staging") for the recommended v1 approach — a direct
      compiler pass rather than waiting on the quote/splice interpreter.

### Running the stdlib: HIR lowering gaps

- [ ] `std.format` remains entirely blocked on `{expr:spec}` interpolation
      splitting (unchanged from above) — this is lexer/parser work, not
      lowering work, and hasn't been started.
