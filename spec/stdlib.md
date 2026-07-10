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

`std.io`/`std.console` compile and run end-to-end now (see "Running the
stdlib: HIR lowering gaps" below for the full path, including
`println`/`print` routing through real `std.console` source rather than a
lowering shortcut); `std.format` still cannot, entirely blocked on
`{expr:spec}` interpolation splitting. This is the checklist of compiler
work they depend on, grouped by phase.

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
- [x] Recognize `impl drop for T` and treat `drop` like any other trait impl
      for coherence/completeness checking. **Resolved:** turned out to need
      no special-casing at all — even "prelude" traits like `show`/`eq` have
      no synthesized `trait_decl` anywhere in the compiler; every prelude
      trait name only ever affects name *resolution* (letting the bare name
      be used as a bound/in `deriving` without a `use`,
      `is_prelude_trait_name`, `src/semantic/types.cpp`), while impl
      coherence/completeness (`validate_impl_coherence`,
      `check_impl_members`, both in `src/semantic/check.cpp`) is driven
      entirely by finding a real, in-session `trait_decl` — a trait a test
      or program declares itself. So `drop` just needed adding to
      `k_prelude_trait_names` (`src/semantic/types.cpp`) for parity with
      `show`/`eq`/etc., and any `trait drop: def drop(mut self) -> unit`
      declared in source flows through the exact same
      duplicate-impl/missing-method/extra-method checks as any other trait.
      Regression tests: `accept_drop_impl.kira`,
      `report_duplicate_drop_impl.kira`, `report_incomplete_drop_impl.kira`
      (`src/testdata/semantic_check_test/`, wired into
      `src/semantic/check_test.cpp`).
- [x] **Superseded by a real prelude, below:** the `k_prelude_trait_names`
      array was a compiler-hardcoded stand-in for `from`/`drop` having no
      real declaration anywhere. Replaced with an actual `prelude.kira` +
      `std/traits.kira` that every real invocation auto-imports; see
      "Real auto-imported prelude" below. `eq`/`ord`/`hash`/`show`/`into`/
      `add`/`sub`/`mul`/`div`/`rem`/`neg` now all have real `std.traits`
      declarations too, and `k_prelude_trait_names`/`is_prelude_trait_name`
      (`src/semantic/types.h`/`.cpp`) have been deleted entirely — every
      prelude trait name resolves purely through `find_prelude_trait`
      finding a real, in-session `trait_decl`. `src/semantic/check_test.cpp`
      now auto-injects `std/traits.kira` + `std/prelude.kira` into every
      fixture session (`prelude_fixtures`, backed by the `//src:std_sources`
      filegroup) so bound positions like `T: eq` keep resolving without the
      hardcoded fallback.
- [x] **Real auto-imported prelude.** `prelude.kira` and `std/traits.kira`
      are real committed `.kira` source (`src/std/prelude.kira`,
      `src/std/traits.kira` — module `prelude` and module `std.traits`,
      the latter declaring the real `pub trait from[T]:`/`pub trait drop:`).
      Every real `kira` invocation now auto-injects both into the
      compile session: `inject_stdlib_prelude` (`src/driver/driver.cpp`)
      locates them next to the running binary (mirroring
      `find_bazel_archive`'s bundled-data search in `src/driver/aot.cpp` —
      works under `bazelisk run`, `bazel test`, and a plain `bazel-bin`
      invocation) and appends them to `cli_config::sources` unless a
      source with the same resolved path is already present; `main.cpp`
      calls it right before `compile_sources`. Deliberately *not* called
      inside `compile_sources` itself, so `compile_sources`'s own unit
      tests keep their exact hand-crafted sessions — only the real CLI
      entry point (and any test that explicitly opts in, e.g.
      `test_compile_sources_typechecks_stdlib_io_and_console`,
      `src/cli_test.cpp`) gets the injected files.

      Name resolution: `checker::find_prelude_trait`
      (`src/semantic/check.cpp`) is a new fallback tier, consulted after
      "declared in the current module" and "reachable through an explicit
      `use`" both miss. It looks up the name in module `prelude`'s own
      traits table, then in each stdlib module `prelude` is known to
      re-export (today just `std.traits`) — a small fixed list
      (`k_prelude_reexport_modules`), not generic transitive `use`
      resolution (see the member-group-import gap noted below; genuine
      transitivity would need that fixed first). Wired into
      `find_session_trait` (so `impl drop for T`/`impl from[X] for Y`
      resolve the real trait with zero local declaration or `use`) and
      into both `is_prelude_trait_name` bound-position fallback sites
      (`T: drop` in a `where`/bound position). Gated on
      `ast::file::no_prelude` (parsed and stored since before this work,
      but never actually consumed until now — see
      `checker::file_no_prelude_`) so a file can opt out.
      `src/std/io.kira` no longer declares its own local `trait from[T]:`/
      `trait drop:` (needed last iteration only because no real prelude
      existed yet) — it relies on the auto-injected one, verified via a
      real end-to-end compile with zero diagnostics.
      Verified interactively (not just by test): `impl drop for T`
      without any local trait declaration resolves and passes
      coherence/completeness checking (missing-method and extra-method
      diagnostics both fire correctly against the real trait), and
      `no_prelude` correctly makes the same file fail with "undefined
      type `drop`".
- [x] **Newly discovered and resolved while implementing the above:** the
      `mut self`/`mut <ident>` binding-pattern syntax used throughout this
      document and `spec/kira-reference.md`'s `drop` section did not parse
      at all — `mut` previously only appeared on reference/pointer *types*
      (`&mut T`, `*mut T`), never as a pattern prefix
      (`spec/kira-grammar.ebnf`'s `atomic_pattern` had no `mut` case).
      Fixed by adding `bool is_mut` to `ast::binding_pattern`
      (`src/parser/ast.h`), a new `parser::parse_mut_binding_pattern`
      dispatched from `parse_atomic_pattern` on `token_kind::kw_mut`
      (`src/parser/parser.cpp`/`.h`), a new `binding_origin::mut_binding`
      (mutable, like `var`) consumed at both binding sites —
      `check_pattern`'s `binding_pattern` case and `let_stmt`'s fast path
      (`src/semantic/check.cpp`) — and a grammar update
      (`spec/kira-grammar.ebnf`). Function parameters needed no semantic
      change: `check_function` already binds every parameter (including
      `self`) with the always-reassignable `parameter` origin regardless of
      pattern mutability, so `mut self` only needed to *parse*. Regression
      tests: `mut_binding_pattern` (`src/parser/parser_test.cpp`),
      `accept_let_mut_reassignment.kira`
      (`src/testdata/semantic_check_test/`, proving `let mut x` allows
      reassignment where plain `let x` would error).
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
- [x] **Resolved — two bugs discovered while testing the fix above, both
      now fixed:**
      1. `match`/`if` used as an *implicit* tail expression (function body
         ends with a bare `match ...`/`if ...`, no `return`) silently
         returned a zero/unit value instead of the matched/branch value;
         only `return match ...`/`return if ...` worked. Root cause:
         `hir::lower_block` always lowered a trailing `match_stmt`/`if_stmt`
         with `k_unknown_type` and never wrapped it in `hir_expr_stmt` (the
         node kind that marks "this is the tail value" for
         `compile_body_and_finish`/`compile_block_as_value` in both
         backends) — unlike an ordinary trailing `expr_stmt`, which always
         got that treatment. `if_stmt` additionally needed a
         `semantic::check.cpp` fix: unlike `match_stmt`, it never threaded
         `expected_tail` through its branches, always typing itself `unit`
         regardless of position. Fixed by: `lower_block` detecting the last
         non-null statement and, if it's `if_stmt`/`match_stmt`, lowering it
         via a new `lower_tail_control_flow_stmt` with the block's real
         target type, `hir_expr_stmt`-wrapped; the same target type is now
         threaded into `lower_function`/lambda bodies/match-arm bodies/`if`
         branch-and-else bodies, all of which had the identical gap;
         `check_body_node`'s `if_stmt` case now joins branch tail types
         against `expected_tail` the same way `infer_if_expr` (ordinary
         `if`-expression position) already did. Fixing this also exposed a
         second, LLVM-codegen-only bug: `compile_expr`'s `hir_if`/
         `hir_match`/`hir_block` cases, and `compile_body_and_finish`/
         `compile_block_as_value`, unconditionally loaded/stored/returned a
         value after `compile_if`/`compile_match`/`compile_block_as_value`
         had already closed the current block with `unreachable` (every
         branch/arm diverges via `return`) — tripping LLVM's verifier
         ("Terminator found in the middle of a basic block"). Fixed by
         checking `terminated`/`builder_.GetInsertBlock()->getTerminator()`
         before emitting anything further, mirroring how the bytecode
         backend already handled this. Regression tests:
         `src/testdata/codegen_test/implicit_tail_match_and_if.kira`,
         exercised through both `src/bytecode_compiler/compile_test.cpp`
         and `src/llvm_codegen/codegen_test.cpp` (the latter specifically
         covers the all-branches-diverge codegen bug); the pre-existing
         `src/testdata/codegen_stress/014_if_elif_else_statement.kira` also
         now passes (it was the stress-corpus file that caught the
         `llvm_codegen` regression).
      2. Field access on a local (`x.field`) failed to lower for *any*
         local — not just a variant-payload binding, confirmed with a plain
         `let` too — with "expression kind ... is not lowered by the first
         milestone." Root cause: `a.b` always parses as
         `ast::module_path_expr` when `a` is a bare leading identifier
         (`parser.cpp` can't yet tell "value" from "module path" without
         symbol info); `semantic::check.cpp`'s `infer_module_path` already
         disambiguates this correctly for typing, but `hir::lower_expr` had
         no case for `module_path_expr` at all, so it always fell through
         to "unsupported." Fixed by adding `lowerer::lower_module_path`,
         which resolves a two-segment `root.field` path the same way the
         checker does (root must be a local binding currently in scope) and
         lowers it as an ordinary `hir_field` over `hir_local_ref` — which
         required teaching the lowerer to track each local's checked type
         at `declare_local` (previously untracked; every call site already
         had the type in hand, either directly or via its already-lowered
         initializer/pattern expression's own `.type`). Longer chains
         (`a.b.c`) and genuine module-qualified paths remain unsupported —
         intermediate segment types aren't recoverable from anything
         lowering can reach, and a longer chain is left `unsupported_construct`
         rather than guessed at. Regression test:
         `src/testdata/codegen_test/field_access_on_local.kira`
         (`src/bytecode_compiler/compile_test.cpp`), covering both a plain
         `let` and a variant-payload binding.

### LLVM codegen / AOT runtime

- [x] Emit an external `declare` per intrinsic against a fixed C-ABI symbol
      name (`kira_rt_stdin`, `kira_rt_open`, ...). `compile_module`
      (`src/llvm_codegen/codegen.cpp`) declares all eight from
      `kira::known_intrinsic_names`/`known_intrinsic_arities`
      (`src/intrinsics.h`) — every argument and return value is an opaque
      heap `ptr`, so arity is all the declaration needs. `compile_call`
      checks `kira::intrinsic_index_of` before falling back to the ordinary
      function table, mirroring `bytecode_compiler`'s identical check.
- [x] A small native runtime static library implementing those symbols:
      `src/runtime/io.h`/`io.cpp` — a second, independent native
      implementation of the same eight operations (per this document's
      design principle: one implementation per backend, not a shared call
      path), added to the existing `//src/runtime:runtime` target.
- [x] Link step wiring — no new plumbing needed. `io.cpp` was added to the
      same `alwayslink=True` `//src/runtime:runtime` target `cli.cpp`'s
      `--build` flow already locates and links (`find_bazel_archive`), and
      that same target is already a real `deps` of `:jit_support`/
      `:codegen`, so JIT tests resolve `kira_rt_*` via ordinary process-
      symbol lookup with no separate wiring either. Verified end-to-end via
      two new JIT tests (`src/llvm_codegen/codegen_test.cpp`:
      `test_intrinsic_call_resolves_to_native_symbol`,
      `test_intrinsic_result_constructs_and_matches_through_real_syntax`) —
      both reuse the existing `intrinsic_call.kira`/`intrinsic_result.kira`
      fixtures already proven against the bytecode backend.
- [x] **Resolved — a pre-existing `--build` CLI linking gap, not specific to
      intrinsics.** `cli.cpp`'s link command invoked the plain C linker
      driver (`cc "<obj>" "<panic_archive>" "<heap_archive>" -o "<out>"`)
      rather than a C++-aware one — any program whose generated object file
      actually referenced a symbol from either archive (any heap type:
      struct/string/list/sum type/intrinsic call, or any checked-arithmetic
      panic path) failed to link with pages of undefined
      `std::__1::*`/`operator new`/`__cxa_*` symbols, since
      `arena.cpp`/`aot_runtime.cpp`/`io.cpp` all use the C++ standard
      library internally. Reproduced with zero relation to intrinsics —
      confirmed with a two-line `add.kira`. A pure-scalar program (e.g.
      `def main() -> int32: return 42`) linked fine only because nothing in
      it referenced either archive, so the linker never pulled in the
      object files that needed libc++. Fixed by switching the link command
      to `c++` (`cli.cpp`). Also fixed `find_bazel_archive` to additionally
      search via the `TEST_SRCDIR`/`TEST_WORKSPACE` convention
      `src/testing/test_data.h`'s `candidate_test_data_dirs` already uses,
      so this path is testable hermetically under `bazel test`, not just
      `bazelisk run`/direct `bazel-bin` invocation. Regression test:
      `src/cli_test.cpp`'s `test_build_links_and_runs_a_heap_using_program`
      — builds a struct-returning program through the real `--build` flow,
      actually runs the produced executable as a child process, and checks
      its exit code. `src/llvm_codegen/aot_test.cpp`'s own passing
      `--build`-equivalent test never exercised this path — it deliberately
      links against a hand-written, dependency-free C stub instead of the
      real `aot_runtime`/`runtime` archives (see its own top-of-file
      comment), so this gap had never been end-to-end tested before.

### Stdlib source

- [x] `std.io`, `std.console` written as real `.kira` source
      (`src/std/io.kira`, `src/std/console.kira`), parsing and typechecking
      cleanly end-to-end through the real CLI/driver — regression test:
      `test_compile_sources_typechecks_stdlib_io_and_console`
      (`src/cli_test.cpp`, backed by the `//src:std_sources` filegroup in
      `src/BUILD.bazel`). `std.format` remains blocked on `{expr:spec}`
      interpolation splitting, above. Notable deviations from this
      document's code blocks, all forced by real compiler/language gaps
      found while writing the source (not stylistic choices):
      - `from` is now real — `std.io` no longer declares its own local
        `trait from[T]:`; `impl from[io_errno] for io_error` resolves the
        real trait via the auto-imported prelude (see "Real auto-imported
        prelude", above). `into` is now real too (`src/std/traits.kira`),
        though unused by `std.io`/`std.console` today.
      - The document's `...` placeholder bodies aren't valid Kira syntax;
        every method below has a real body (`io_error.from`'s errno-code
        match covers `ENOENT`/`EACCES`/`EEXIST`/`EINTR`/`EAGAIN`/`EPIPE`
        explicitly, falling back to `@other`).
      - `use std.io.{ file, writer, ... }`-style *member-group* imports
        don't resolve: `src/semantic/resolution.cpp`'s import validation
        (`emit_unresolved_import`) only ever checks whether a `use`'s
        dotted path matches a *submodule* declared in-session — there is
        no mechanism to import an individual type/trait/function by name
        into unqualified scope, only whole-module imports (`use std.io`)
        or wildcard submodule imports (`use pkg.tools.*`, see
        `test_compile_sources_resolves_session_imports`,
        `src/cli_test.cpp`). `std.console` therefore references `std.io`'s
        public items by fully qualified path (`std.io.file`,
        `std.io.stdout_handle()`, ...) instead of importing them — the
        same pattern already proven by
        `src/testdata/semantic_check_test/accept_cross_module_qualified_types/`.
        Cross-module *method* calls (`.write_all()` on a `std.io.file`
        from `std.console`) needed no such workaround: `find_method`
        (`src/semantic/check.cpp`) resolves impls session-wide regardless
        of `use` imports.
      - The document's `pub def (file).close(mut self) -> ...` extend-method
        syntax isn't real; the actual syntax is an `extend file:` block
        (already used by `extend_user_type`/`extend_builtin_type` tests).
      - **Confirmed but out of scope:** neither file lowers to HIR yet
        (`bazelisk run` reports "Lowered 0/2 module(s)" with zero
        diagnostics from the semantic pass) — HIR lowering for `trait`
        default-method dispatch, `extend` blocks, and generic `impl`s isn't
        exercised anywhere in the existing corpus either (no
        `codegen_test`/`codegen_stress` fixture uses `trait`/`extend` at
        all), so this predates and is unrelated to the stdlib work. It
        blocks actually *running* `std.io`/`std.console`, not typechecking
        them. See "Running the stdlib: HIR lowering gaps" below for the
        specific constructs this breaks down into.
      - [x] Minor, separately-noticed gap, now fixed: `render_compile_summary`
        (`src/driver/driver.cpp`) never surfaced a per-module HIR-lowering
        failure's error message (`hir_lowering_result::error` was recorded
        but never printed), making the "Lowered N/M" line silently
        unhelpful when M > N. `render_compile_summary` now prints
        `[module] message` for every module that failed to lower, and
        `lower_and_emit_modules` (`src/driver/lowering_stage.cpp`) appends
        the failing node's byte offset to the message so a failure can be
        matched back to a source line without a debugger. This is what
        made the findings below possible to pin down precisely.

### Running the stdlib: HIR lowering gaps

Initial investigation, below, was verified by hand against the real driver (`bazel-bin/src/kira`, not just
`bazelisk run` — argv0 differs enough between the two that
`inject_stdlib_prelude`'s runfiles search behaves differently; see
`find_stdlib_source_file`), passing `src/std/{traits,prelude,io,console}.kira`
plus a trivial `main` module. Typechecking is clean (0 diagnostics); lowering
fails for every stdlib module that calls into another one. All three
failures share one root cause: `hir::lowerer`'s call/path resolution
(`lower_module_path`/`lower_call`, `src/hir/lower.cpp`) only ever resolves a
multi-segment path whose *root* is a local binding currently in scope —
there is no case for "root is a module" or "root is a type name."

- [x] **Module-qualified free-function calls and type-qualified
      associated-function calls — resolved, including real cross-module
      execution, not just lowering.** Root cause was two-fold, not one:
      (1) the *checker* never resolved a module-qualified call
      (`console.println(...)`, `std.io.stdout_handle()`) or a
      type-qualified associated-function call (`io_error.from(e)`) to a
      real declaration at all — `infer_call` fell straight to "unknown"
      for these; and (2) even a correctly-resolved cross-module call had
      nothing to *run* against — `bytecode_compiler::compile_module` and
      `llvm_codegen::compile_module` each only ever compiled one
      `hir::hir_module` in isolation, and the driver only ever selected and
      handed off a single module to begin with.
      Fixed: `semantic::check.cpp`'s new `infer_qualified_call` (reached
      from `infer_method_call`, since a dotted call — `a.b(...)` — always
      parses as `field_expr`, never a bare `module_path_expr`, once a call
      follows the last segment; see `parser::parse_ident_or_path_expr`)
      resolves a call's `object.fn_name` shape against a real module
      function (fully qualified, or through a whole-module `use` import)
      or a type's associated function (an impl member with no `self`
      parameter — an actual instance-method call, `x.method()`, is a
      different, still out-of-scope call shape, see below), reusing the
      existing `find_session_module_of_path`/`find_import`/
      `find_type_decl_by_name`/`find_method`/`check_call_against_decl`
      lookups rather than inventing new resolution. Each match is recorded
      in a new `checked_types::resolved_callees` map
      (`semantic/types.h`'s `resolved_callee`) keyed by the call node, so
      `hir::lower_call` (`src/hir/lower.cpp`) can rebuild the exact same
      call site without redoing checker-side resolution — it builds a
      `hir_local_ref` whose name is the bare declared name (or, for an
      associated function, `TargetType::method`) and whose new
      `owner_module` field (`hir_local_ref::owner_module`,
      `src/hir/nodes.h`, additive/optional — every other construction site
      is unaffected) names the declaring module. `hir::lower_module` was
      also extended to lower eligible impl members (no `self` parameter,
      no generics on the impl or the method) into real `hir_function`s
      under that same `TargetType::method` name — previously `lower_module`
      only ever walked top-level `func_decl` items, so no impl member had
      ever been lowered at all.
      Cross-module *execution*: both `bytecode_compiler::compile_module`
      and `llvm_codegen::compile_module` gained a `std::span`-of-modules
      overload (the existing single-module signature now just delegates to
      it) that builds one combined function table across every module
      passed in — the entry module's (`modules.front()`) functions keep
      their bare names (so `--run`/`--build` function-name lookup is
      unaffected), every other module's functions are keyed
      `module_name::name`, and each backend's `function_compiler` resolves
      a callee's table key from `hir_local_ref::owner_module` plus which
      module the currently-compiling function itself belongs to — no HIR
      tree is ever mutated, moved, or cloned to achieve this, so the same
      lowered modules can be reused for both `--run` and `--build` in one
      invocation. A new read-only discovery pass,
      `hir::find_reachable_modules` (`src/hir/link.h`/`.cpp`, a mutable-free
      walker mirroring `captures.cpp`'s shape), finds every module an entry
      module transitively needs by following `owner_module` references, one
      whole module at a time; `driver/run_build_stage.cpp` calls it before
      handing modules to either backend.
      Verified against the real stdlib source, not just synthetic
      fixtures: `bazel-bin/src/kira --run` against
      `src/std/{traits,prelude,io}.kira` plus a trivial `main` module
      calling `std.io.stdin_handle()` now reports `Lowered 4/4 module(s)`
      and actually executes (`main() -> 7`); the identical program built
      with `--compile` links and runs as a native executable returning
      the same value. `std.io`'s own internal use of `io_error.from(e)`
      (inside `open`, `read`, `write`, `flush`) lowers and links cleanly as
      part of that same run — the real pattern this gap blocked, not a
      simplified stand-in. `std.console` still fails to lower as a whole
      file (`print`/`println`/`eprint`/`eprintln` call the self-taking
      `write_all` trait-default method — item 3, below), even though its
      own `stdout()`/`stderr()`/`stdin()` (pure module-qualified calls, no
      self-taking dependencies) are individually fixed by this work —
      lowering still fails closed per-file, so one still-blocked function
      in a file blocks the whole file. Regression tests:
      `src/semantic/check_test.cpp` (`accept_module_qualified_call`,
      `accept_fully_qualified_call`, `accept_type_qualified_associated_call.kira`,
      `src/testdata/semantic_check_test/`), `src/hir/lower_test.cpp`
      (`test_lowers_module_qualified_call`,
      `test_lowers_type_qualified_associated_call`), `src/hir/link_test.cpp`
      (new — direct/transitive module discovery), and, for real
      cross-module execution through both backends end to end,
      `src/bytecode_compiler/compile_test.cpp` /
      `src/llvm_codegen/codegen_test.cpp`
      (`test_calls_a_function_in_another_module`,
      `test_calls_an_associated_function_via_a_type_qualified_path`, both
      run/JIT'd to a real expected value).
- [x] **Trait default-method dispatch** (`file.write_all(...)`,
      `.read_to_end(...)`, `.read_exact(...)` — calling a trait method that
      the impl doesn't override, only the trait's default body supplies) and
      **`extend` blocks** (`extend file: pub def close(...)`) are resolved
      at the checker/lowering level: `semantic::check.cpp`'s
      `build_method_table` synthesizes a per-concrete-impl clone of each
      unwritten trait-default method (`monomorphize_trait_default`, via the
      now-general `ast::clone_func_decl`), type-checks it with `self_type_`
      bound to the concrete target, and records it in `checked_types::
      synthesized_trait_defaults` for `hir::lower_module` to lower alongside
      every other impl member; `extend` blocks lower through a
      `lower_extend_methods` sibling of the ordinary impl-member lowering.
      Reading the code directly (this checklist item having gone stale
      relative to it) confirms the mechanism is fully wired end to end,
      checker through lowering.
      This surfaced several lower-level blockers, all now fixed and
      verified by a full `bazelisk test //...` run plus manual `--run`/
      `--compile` smoke tests of a real `println` program on both
      backends:
      - `&`/`&mut`/range-slicing (`buf[a..b]`) were entirely unimplemented
        in both `bytecode_compiler`/`llvm_codegen` — `std.console::print`'s
        real body (`stdout().write_all(&s.as_bytes())`) needs the former,
        `write_all` needs both. Both backends now pass `&`/`&mut` through
        unchanged for any already-heap-pointer-represented value (struct/
        sum/opaque/fn/tuple/array/`list`/`option`/`result`/`str`), and
        range-index a `str`/`slice`/`slice_mut`/`array[byte, N]` source by
        pure pointer arithmetic on its byte-addressed `{ len; data }`
        header (or, for `array[byte, N]`, the statically-known `N` and the
        array's own pointer — it has no header). `array[byte, N]` is now
        given its own tightly byte-packed representation (`is_byte_array_
        type`, new `op_store_byte`/`op_load_byte_indexed`/`op_store_byte_
        indexed` bytecode opcodes and their LLVM equivalents) instead of
        the generic 8-bytes/element slot layout every other array uses —
        needed so `reader::read_to_end`'s `buf[0..4096]` aliases a real
        mutable view (`rt_read` writes through it, and `buf[i]` reads the
        result back out afterward), not a silently-dropped copy.
      - A dangling-reference bug in `type_table`: `entry()` returned a
        `const type_entry &` into `type_table`'s backing `std::vector`,
        which callers routinely held across further calls that could
        themselves intern new types (e.g. `infer_method_call` holding its
        own `entry` across `find_method`/`fn_type_of`) — a `push_back`-
        triggered reallocation would silently invalidate it mid-expression
        (a struct's own name reading back empty). Fixed at the root by
        switching `entries_` to `std::deque`, which never invalidates
        references to existing elements on `push_back`.
      - `unit` had no bytecode/LLVM representation as a *value* (only as
        an absent return) — `std.console.print`/`eprint`'s explicit
        `return unit` needs one. Both backends now treat it as a zero
        placeholder, mirroring how little information a `unit` value
        carries.
      - Assigning to a struct field (`self.fd = ...`, `std.io.file`'s
        `close()`) had no bytecode/LLVM support; added.
      - `while true: ...` (Kira's only spelling for an unconditional loop,
        having no `break`/`continue`) — `read_to_end`/`read_exact`'s shape
        — miscompiled under `llvm_codegen`: the loop's `end_bb` is
        statically unreachable (its only exits are `return`s inside the
        body), but was left open for a caller to plant a *different*,
        wrongly-typed fallthrough return into, tripping the LLVM verifier.
        Now marked `unreachable` instead.
      - `aot.cpp`'s hand-rolled AOT link step only linked `libruntime.a`/
        `libaot_runtime.a`, missing `runtime`'s own transitive Bazel
        `deps` on `//src/semantic`/`//src/parser` (needed by `layout.cpp`'s
        `struct_field_slot`-family compile-time layout helpers) — any AOT
        program touching a struct/sum type failed to link with
        `type_table::entry` undefined. Now finds and links those two
        archives as well.
- [ ] `std.format` remains entirely blocked on `{expr:spec}` interpolation
      splitting (unchanged from above) — this is lexer/parser work, not
      lowering work, and hasn't been started.
