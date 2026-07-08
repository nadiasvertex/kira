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
  interpolation and lets types opt in to width/precision/fill handling; a
  type that only implements `show` still works everywhere.

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

Builds on the prelude's `show` trait rather than replacing it.

```kira
module std.format

pub trait debug:
    def debug(self) -> str

pub type align = @left | @right | @center

pub type format_spec = {
    fill:      char,
    align:     option[align],
    width:     option[usize],
    precision: option[usize],
}

pub trait format_with:               # opt-in for types that care about fill/width/precision
    def format(self, spec: format_spec) -> str

impl[T: show] format_with for T:     # blanket default: ignore spec, defer to show()
    def format(self, spec: format_spec) -> str: self.show()
```

The `{expr:spec}` mini-language (e.g. `{value:>10.2}`) is new lexer/parser
work — string interpolation splitting is not implemented past the lexer yet
(see "Compiler and Language Changes Required"). This document specifies the
target shape; the grammar work is tracked separately.

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

None of `std.io`/`std.format`/`std.console` can compile end-to-end yet.
This is the checklist of compiler work they depend on, grouped by phase.

### Language / grammar

- [x] `intrinsic def name(...) -> T` declaration form: signature only, no
      body, no `{...}` block. Rejected (with a diagnostic, not silently)
      everywhere a body follows.
- [x] `drop` trait added to the language reference
      (`spec/kira-reference.md`, "Destructors: `drop`") and prelude trait
      list.

### Parser

- [x] Lex/parse `intrinsic` as a `func_decl` modifier (`src/parser/token.h`,
      `src/parser/parser.cpp`) — parses a signature and forces bodyless
      parsing regardless of the surrounding context's default; a body that
      follows anyway is a diagnostic, not a silent accept.
- [x] Regression tests in `src/parser/parser_test.cpp`
      (`intrinsic_def`, `intrinsic_def_rejects_body`).

### Semantic analysis

- [x] Recognize `intrinsic def` items: typecheck the signature like any
      other function, register the symbol as callable, skip body-checking
      (there is no body) — `check_function` in `src/semantic/check.cpp`.
- [x] Validate `intrinsic def` names against the shared intrinsic schema
      (`src/intrinsics.h`) — an unrecognized name is a compile error with a
      "did you mean" suggestion. Also enforced: intrinsics must be
      module-level (not a trait/impl member), and every parameter plus the
      return type must be explicitly annotated, since there is no body to
      infer from.
- [ ] Recognize `impl drop for T` and treat `drop` like any other trait impl
      for coherence/completeness checking (this part is unblocked today).
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
      (`src/parser/text_escape.h`); the `format_spec` mini-language depends
      on this splitting existing.

### Bytecode compiler / VM

- [x] New opcode `op_call_intrinsic` (`u8 dst, u8 intrinsic_id, u8
      first_arg_reg, u8 argc`), parameterized the same way `op_add` is
      parameterized by `numeric_kind` rather than combinatorially enumerated
      per intrinsic. `intrinsic_id` indexes `kira::known_intrinsic_names`
      (`src/intrinsics.h`).
- [x] `hir::lower_module` skips `intrinsic def` items (no body to lower);
      the bytecode compiler's `compile_call` recognizes a call to a known
      intrinsic name by itself and emits `op_call_intrinsic` instead of
      falling back to the ordinary `functions_`-table `op_call` path.
- [x] VM dispatch table (`src/bytecode/vm.cpp`) implementing all eight
      intrinsics with real POSIX calls (`::open`/`::close`/`::read`/
      `::write`), tested against a real pipe and a real missing-file error
      (`src/bytecode/vm_test.cpp`, `src/bytecode_compiler/compile_test.cpp`).
- [x] **Resolved:** `option[T]`/`result[T, E]` construction/matching in the
      bytecode backend. The gap was that `runtime::layout.cpp`'s
      `sum_variants_of` required an AST-backed `type_decl` for variant/tag
      metadata, which the builtin generics `option`/`result` don't have
      (they're `type_kind::builtin_generic_kind`, not `sum_kind`) — so
      `@some(x)`/`@ok(x)`/etc. typechecked and lowered to HIR fine but
      failed to *compile*, with "variant `some` does not resolve to a
      declared sum-type variant." Fixed by hardcoding a static variant
      table for `option` (`some`→tag 0/1 slot, `none`→tag 1/0 slots) and
      `result` (`ok`→tag 0/1 slot, `err`→tag 1/1 slot) in
      `builtin_generic_variants_of` (`src/runtime/layout.cpp`), matching
      the order `semantic::check.cpp`'s own hardcoded `option`/`result`
      handling already used. This fix lives in the file both the bytecode
      compiler and the LLVM/AOT codegen backend share, so it resolves the
      gap for both, not just the bytecode tier. `rt_open`/`rt_close`/
      `rt_read`/`rt_write`/`rt_flush` now construct real `@ok`/`@err`
      values instead of panicking; `panic_reason::io_failure` was removed
      as no longer needed. Regression tests: `src/runtime/layout_test.cpp`
      (variant metadata), `src/bytecode/vm_test.cpp` (intrinsics returning
      `@ok`/`@err`), `src/bytecode_compiler/compile_test.cpp` (real
      `intrinsic def rt_open` + real `match @ok(fd)/@err(e)` syntax, end to
      end).
- [ ] **Newly discovered, out of scope for the fix above — two separate
      pre-existing bugs surfaced while testing it:**
      1. `match` used as an *implicit* tail expression (function body ends
         with a bare `match ...`, no `return`) silently returns a
         zero/unit value instead of the matched arm's value; only
         `return match ...` works correctly. Reproduces with any type, not
         just `option`/`result` — e.g. `def f(x: bool) -> int32: match x: /
         true => 111 / false => 222` returns `0` either way via `--run`.
      2. Field access on a variant-payload-bound pattern variable (`@err(e)
         => e.code`) fails to lower ("expression kind ... is not lowered by
         the first milestone"), for *any* sum type, user-declared or
         builtin. Both are real correctness/diagnostics gaps — the first
         silently produces a wrong answer with no error — and deserve their
         own follow-up.

### LLVM codegen / AOT runtime

- [ ] Emit an external `declare` per intrinsic against a fixed C-ABI symbol
      name (`kira_rt_read`, `kira_rt_write`, ...).
- [ ] A small native runtime static library implementing those symbols,
      statically linked into AOT executables.
- [ ] Link step wiring so the runtime library is found and linked
      automatically — no user-visible linker flags for a program that just
      uses `std.io`/`std.console`.

### Stdlib source

- [ ] `std.io`, `std.format`, `std.console` written as `.kira` source once
      `intrinsic def` parses and typechecks (this document's code blocks are
      the target, not yet committed source).
