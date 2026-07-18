# Standard Library Design: `std.string`, `std.platform`, `std.fs.path`

This document specifies the design and design intent of three standard
library modules: the UTF-8 extension surface of `std.string`, `std.platform`,
and `std.fs.path`. It complements `spec/stdlib-reference.md`, which specifies
`std.io`, `std.format`, `std.console`, `std.iter`, `std.collections`, and
`std.algorithm`, and which also sketches an owned, growable `string` type
(`{ bytes: list[byte] }` with a UTF-8-validity invariant) built on `std.io`'s
`writer` trait. That owned type and this document's `extend str` surface are
two faces of the same module — `std.string` is where Kira's *immutable* `str`
view (see Strings in `spec/kira-reference.md`) gains a working method surface,
and where an *owned, mutable* companion to it lives once building one up in a
loop is needed. `std.fs.path` and `std.platform` both build on this module's
`str` methods rather than duplicating string-scanning logic.

This document is a design record, not a status report — it does not track
which parts of the surface below currently run on either backend.

---

## `std.string`: UTF-8 string extension methods

### Design intent

The compiler ships `str` as a built-in view type, but a view type is only as
useful as its method surface, and a young stdlib is exactly where hand-rolled
substring search and case mapping should *not* live at the call site. `std.
string` exists to give `str` (and, via the owned `string` type in
`spec/stdlib-reference.md`, growable text) a complete, efficient, correct
method surface, implemented once and shared by every backend.

Two design commitments shape everything below:

- **UTF-8 from the ground up.** Every operation is defined in terms of the
  Unicode scalar values encoded by a string's UTF-8 bytes, not raw bytes or
  ASCII. This is Rust's proven design for fast, correct UTF-8, and the model
  mirrors it deliberately.
- **Efficiency is a first-class goal, not an afterthought.** The compiler
  teaches the language, but the *programs it produces* must be fast. Every
  operation here uses the best practical algorithm (Two-Way / Boyer–Moore–
  Horspool-class substring search, O(1) zero-copy sub-slicing, single-pass
  table-driven case mapping) — never an O(n·m) fallback.

### String model

The model mirrors Rust's `String`/`&str` split:

- **`str` is a sequence of UTF-8 bytes** behind a `{ len; data_ptr }` header.
  `len` is the **byte** length.
- **Byte indices are the currency.** `str.len()` is the byte length, O(1);
  `find`/`rfind`/`split` report **byte offsets**; slicing is `s[a..b]` over
  byte offsets. This is what makes slicing O(1) and copy-free — a range slice
  is pure pointer arithmetic over the shared buffer, so sub-slices never
  allocate.
- **Iteration yields `char` (Unicode scalar values).** `for c in s: ...`
  decodes UTF-8 as it walks. Byte length and scalar length are distinct and
  both exposed (`.len()` vs. a scalar-count accessor).
- **Every reported byte offset lands on a scalar boundary.** This is UTF-8's
  self-synchronizing property: a valid UTF-8 needle can only match a valid
  UTF-8 haystack at scalar boundaries, so **byte-level substring search is
  automatically scalar-correct**. This is the theorem that lets search run at
  raw-byte speed while staying UTF-8-safe — decoding is never on the hot path.
- **Case mapping is Unicode simple (1:1) case mapping.** Full/special casing
  (multi-scalar expansions like `ß`→`SS`, locale-tailored rules like Turkish
  dotless-i) is explicitly out of scope for v1; see Limitations.
- **`reversed` reverses by scalar**, not by grapheme cluster. Combining marks
  and ZWJ sequences are not kept adjacent to their base — that needs UAX #29
  segmentation; see Limitations.

### Architecture: two layers

**Layer 1 — runtime intrinsics** (native, fast, UTF-8-aware). A small,
fixed set of `rt_str_*` primitives — equality, forward/reverse find, upper/
lower case mapping, reverse, trim, replace — each backed by a real algorithm
(Two-Way string matching for search, a generated Unicode simple-case-mapping
table for casing) and shared verbatim by every backend, the same way
`std.fmt` already delegates formatting work to `rt_fmt_*`. Substring search
and Unicode case mapping are exactly the operations a young stdlib should not
hand-roll per call site: one native implementation, reused everywhere.

Not-found is signaled by a small `find_result = { found: bool, pos: usize }`
struct, not a magic sentinel value (`usize::MAX`) and not an `option`
constructed on the native side — a plain struct needs no reserved value and
does not hard-code the sum-type layout in the runtime.

**Layer 2 — `extend str` composition** (Kira source). A thin `extend str:`
layer expresses the public API purely as composition over Layer 1:
`starts_with`/`ends_with` are one anchored `memcmp`-equivalent each,
`contains` is `find(...).is_some()`, `split` is a loop over `find` yielding
zero-copy sub-slices. Nothing above Layer 1 touches raw bytes directly — this
is the layer that keeps the API surface readable as ordinary Kira and keeps
the native surface minimal and stable.

### Public API

```kira
extend str:
    pub def is_empty(self) -> bool
    pub def eq(self, other: str) -> bool
    pub def starts_with(self, prefix: str) -> bool
    pub def ends_with(self, suffix: str) -> bool
    pub def contains(self, needle: str) -> bool
    pub def find(self, needle: str) -> option[usize]
    pub def rfind(self, needle: str) -> option[usize]
    pub def trim(self) -> str
    pub def trim_start(self) -> str
    pub def trim_end(self) -> str
    pub def reversed(self) -> str
    pub def to_uppercase(self) -> str
    pub def to_lowercase(self) -> str
    pub def replace(self, from: str, to: str) -> str
    pub def split(self, sep: str) -> list[str]
```

`eq` exists as a named method distinct from `==` for `str`; `==`/`!=`
dispatching to it at the operator level is a natural, separable follow-up
rather than part of this module's own surface.

### Method semantics

- **`is_empty()`** — `true` iff byte length is 0.
- **`eq(other)`** — `true` iff identical byte length and content.
- **`starts_with(prefix)` / `ends_with(suffix)`** — substring anchored at
  start/end; empty affix ⇒ `true`; affix longer than `self` ⇒ `false`.
- **`contains(needle)`** — `true` iff `needle` occurs anywhere; empty needle
  ⇒ `true`.
- **`find(needle)`** — `@some(byte_offset)` of the first occurrence, else
  `@none`; empty needle ⇒ `@some(0)`. Offset is a scalar boundary.
- **`rfind(needle)`** — `@some(byte_offset)` of the last occurrence, else
  `@none`; empty needle ⇒ `@some(self.len())`.
- **`trim` / `trim_start` / `trim_end`** — remove leading/trailing scalars
  with the Unicode `White_Space` property (not just ASCII); returns a
  zero-copy sub-slice; returns `self` unchanged when nothing trims.
- **`reversed()`** — scalars emitted in reverse order (valid UTF-8 out).
- **`to_uppercase()` / `to_lowercase()`** — Unicode simple case mapping
  applied per scalar; unmapped scalars pass through.
- **`replace(from, to)`** — all non-overlapping occurrences of `from`
  replaced with `to`; empty `from` ⇒ `self` unchanged (no zero-width
  matches).
- **`split(sep)`** — pieces between occurrences of `sep`; consecutive
  separators yield empty pieces (`"a::b".split(":")` → `["a", "", "b"]`);
  empty `sep` ⇒ `[self]`; no occurrence ⇒ `[self]`.

### Algorithms and complexity

- **Search** (`find`/`rfind`/`contains`/`split`/`replace`): **Two-Way string
  matching** (the Crochemore–Perrin algorithm — glibc `memmem`'s and Rust
  `str::find`'s choice). O(n + m) worst case, O(1) extra space, no
  precomputed table proportional to alphabet size. Byte-level and
  UTF-8-correct by self-synchronization.
- **`eq`/`starts_with`/`ends_with`**: O(min length) after an O(1) length
  check; no allocation — affixes are zero-copy slices.
- **`split`**: O(n) total search; pieces are O(1) zero-copy sub-slices.
- **`to_uppercase`/`to_lowercase`/`reversed`/`replace`**: single
  decode/encode pass into one length-reserved buffer — O(n), one allocation,
  no O(n²) repeated-concatenation blowup.
- **`trim*`**: O(leading + trailing) scalar scan; result is a zero-copy
  sub-slice.

### Limitations

- **Simple case mapping only.** `to_uppercase`/`to_lowercase` apply Unicode
  1:1 simple mappings. Full/special casing (`ß`→`SS`, final-sigma,
  locale-tailored Turkish/Azeri/Lithuanian rules) and case *folding* for
  caseless comparison are out of scope for v1. `eq`/search are code-point
  exact, not case-insensitive or normalization-aware.
- **No normalization.** Canonically-equivalent but differently-composed
  strings (e.g. `é` as U+00E9 vs. `e` + U+0301) compare unequal and search
  independently. NFC/NFD normalization is a separate future module.
- **Scalar-level, not grapheme-level.** `reversed` reverses scalars; `find`
  offsets are scalar boundaries, not grapheme boundaries. A combining mark
  can be separated from its base by `reversed`, or land mid-grapheme in a
  `split` piece. Grapheme segmentation (UAX #29) is out of scope.
- **`str` methods only.** No `char`-classification API (`is_alphabetic`,
  etc.) lives here.
- **No regex.** `split`/`replace` take literal substrings only.

---

## `std.platform`: platform and build introspection

### Design intent

`std.platform` gives programs the information needed to adapt behavior to
the host system, log diagnostic context, and inspect the Kira toolchain that
produced the running binary. The central design tension it resolves is
**compile-time target vs. runtime host**: most properties of the *target* —
architecture, pointer width, endianness, OS family — are knowable when the
binary is built and are exposed as `pure` accessor functions; properties of
the *host* the binary happens to be running on — hostname, detailed OS
release, processor name — can only be known by asking the OS, and are queried
through thin runtime intrinsics.

Kira has no `--target` flag or cross-compilation support today, so "target"
and "the host this compiler binary was itself built on" are currently the
same thing — target accessors are computed once per compile from the
toolchain's own preprocessor knowledge (`__x86_64__`, `__APPLE__`,
`__linux__`, and friends), not queried at runtime.

Supporting principles:

- **Error propagation is `?`, not combinators.** Kira's one propagation
  primitive is `?`; a function returning `result[T, io_error]` propagates a
  failing intrinsic call with `let x = rt_thing()?`, and `?` converts the
  intrinsic's raw `io_errno` to `io_error` via the same `from` mechanism used
  throughout the language (`impl from[io_errno] for io_error`, see
  `std.io`). Swallowing an error into `@none` instead of propagating it is
  spelled with an explicit `match`.
- **Best-effort convenience, explicit precision.** A combined `uname()`-style
  struct exposes its runtime-queried fields as `option[str]`, so "could not
  determine" (`@none`) is distinguishable from "genuinely empty" (`@some(
  "")`). Individual field queries (`node()`, `release()`, `processor()`)
  return `result[T, io_error]` for callers that must handle failure
  explicitly.
- **Platform-specific functions return `option`, not `result`.** Calling a
  Windows-only query on macOS is not an error — the information simply does
  not apply there. `option` makes `if let` the natural way to consume
  platform-specific data, distinct from a genuine I/O failure.
- **Extensible sum types.** `architecture` and `os_family` enumerate the
  common cases but carry an `@other(str)` variant, so a port to a new
  platform does not require an immediate stdlib change to stay representable.
- **Runtime dispatch, not compile-time dispatch, for platform-varying
  behavior.** Functions like `release()`/`version()` `match` a runtime value
  (`target_os_family()`) rather than being selected per-platform at compile
  time via `static if` branches — one unconditional definition per function,
  dispatching internally, rather than one definition per platform spliced in
  conditionally.

### Types

```kira
pub type architecture = @x86_64 | @aarch64 | @arm | @riscv32 | @riscv64 | @wasm32 | @wasm64 | @other(str)
pub type os_family = @unix | @windows | @macos | @other(str)
pub type endianness = @little | @big

pub type platform_info = {
    system: str,
    node: option[str],
    release: option[str],
    version: option[str],
    machine_arch: architecture,
    processor: option[str],
    endianness: endianness,
    pointer_width: usize,
    os_family: os_family,
}

pub type kira_build = {
    version: str,
    implementation: str,
    compiler: str,
    build_date: str,
}

pub type libc_version = { name: str, version: str }

pub type windows_version = {
    release: str,
    version: str,
    csd_version: str,
    platform_type: str,
}

pub type macos_version = {
    release: str,
    version: str,
    dev_stage: str,
    non_release_version: str,
    machine_arch: str,
}
```

(`machine`, not `machine_arch`, would read more naturally, but `machine` is a
reserved keyword — the function modifier opting into machine-level
arithmetic — so every field/parameter that would otherwise be `machine` is
`machine_arch` instead.)

### Intrinsics

Every intrinsic is niladic and returns `result[T, io_errno]`, declared
unconditionally rather than gated per platform — a native implementation
reports "not supported on this host" gracefully when called on a host it
doesn't describe, so a platform that doesn't apply costs only code size, not
correctness:

```kira
type uname_raw = { sysname: str, nodename: str, release: str, version: str, machine_arch: str }
type winver_raw = { major: uint32, minor: uint32, build: uint32, platform_id: uint32, csd_version: str }
type macver_raw = { release: str, version: str, dev_stage: str, non_release_version: str, machine_arch: str }

intrinsic def rt_uname() -> result[uname_raw, io_errno]
intrinsic def rt_libc_version() -> result[libc_version, io_errno]
intrinsic def rt_windows_version() -> result[winver_raw, io_errno]
intrinsic def rt_macos_version() -> result[macver_raw, io_errno]
intrinsic def rt_gethostname() -> result[str, io_errno]
intrinsic def rt_processor_name() -> result[str, io_errno]
```

`rt_libc_version` returns the `libc_version` struct directly (name +
version) rather than a raw string a caller would need to split — this
sidesteps `std.platform` ever needing string splitting for its own purposes.

### Target queries (compile-time)

`pure`, because the target never changes at runtime:

```kira
pub pure def target_arch() -> architecture
pub pure def target_os() -> str
pub pure def target_os_family() -> os_family
pub pure def target_vendor() -> str
pub pure def target_env() -> str
pub pure def target_endianness() -> endianness
pub pure def target_pointer_width() -> usize

pub pure def is_unix() -> bool
pub pure def is_windows() -> bool
pub pure def is_macos() -> bool
```

### Runtime queries

```kira
pub def node() -> result[str, io_error]
pub def processor() -> result[str, io_error]
pub def release() -> result[str, io_error]
pub def version() -> result[str, io_error]
```

`release()`/`version()` genuinely differ per platform, but are each **one**
unconditional definition that `match`es the runtime value
`target_os_family()` returns:

```kira
pub def release() -> result[str, io_error]:
    match target_os_family():
        @unix    =>: let raw = rt_uname()?; return @ok(raw.release)
        @windows =>: let raw = rt_windows_version()?; return @ok("{raw.major}.{raw.minor}")
        @macos   =>: let raw = rt_macos_version()?; return @ok(raw.release)
        @other(_) => @err(io_error.from(io_errno { code: 0 }))
```

### Cross-platform convenience

```kira
pub def uname() -> platform_info
pub def platform() -> str   # e.g. "{target_os()}-{release}-{arch}-{processor}"; never fails, "unknown" fills gaps
pub def kira_build_info() -> kira_build
```

### Platform-specific queries

`@none` on a platform other than the one they describe, or when the
underlying intrinsic fails — not an error, since asking the wrong platform
about itself isn't a failure:

```kira
pub def libc_ver() -> option[libc_version]
pub def win32_ver() -> option[windows_version]
pub def mac_ver() -> option[macos_version]
```

### Summary of design decisions

| Decision | Rationale |
|---|---|
| Target/build info are `pure def`s, not `pub static` constants | Keeps every target property expressed as a function of the compile, not stored state |
| Host info is `result[T, io_error]` | OS calls can genuinely fail; the type forces the caller to decide what to do |
| Error propagation uses `?`, not `.map`/`.map_err`/`.ok()` | `?` is the language's one propagation primitive, with implicit `from`-based conversion; swallow-the-error conversions use `match` instead |
| All intrinsics/raw structs are declared unconditionally, not gated by platform | Keeps one code path; a platform that doesn't apply reports "not supported," it doesn't need to not exist |
| `release()`/`version()` are single definitions matching the *runtime* `target_os_family()` | One function per concept, dispatching internally, rather than one function per platform |
| `uname()` uses `option[str]` for runtime fields | `@none` (could not query) and `@some("")` (genuinely empty from OS) are distinguishable; no magic-value sentinel |
| `platform()` never fails | It is a formatting convenience; `"unknown"` is an acceptable fallback |
| Platform-specific functions return `option` | Calling `libc_ver()` on Windows is not an error — the information simply does not apply |
| `architecture` / `os_family` are sum types with `@other` | Makes invalid states unrepresentable for known platforms while remaining extensible |
| Struct fields that would be named `machine` are `machine_arch` | `machine` is a reserved function-modifier keyword |

---

## `std.fs.path`: filesystem path string manipulation

### Design intent

`std.fs.path` provides a `path` value type for constructing, joining, and
decomposing filesystem paths as strings, modeled after C++'s
`std::filesystem::path`. It is deliberately **purely a string-manipulation
library** — no syscalls, no actual filesystem access. Checking existence,
querying whether something is a directory or symlink, and other I/O-backed
queries belong to a future `std.fs` module that layers on top of this one,
not here: separating "what does this path string mean" from "what does the
filesystem say about it" keeps this module usable without I/O permissions
and keeps its behavior fully deterministic and host-independent to reason
about.

Guiding choices:

- **String-backed, lazily parsed.** A path is stored as a plain string
  (`raw: str`), not eagerly split into components. Every operation performs
  a linear scan as needed, so a caller who only calls `.is_absolute()` never
  pays for component-splitting they didn't ask for.
- **Platform-aware, host-independent.** Separator conventions (`/` vs. `\`,
  drive-letter absoluteness) are decided by consulting `std.platform`'s
  target/host classification, not hardcoded to one OS — the same source
  should behave correctly whether it targets Unix or Windows.
- **Built on `std.string`, composed via interpolation.** Path-building
  operations reuse `std.string`'s equality and substring-search methods
  rather than re-implementing string scanning, and new path strings are
  assembled with string interpolation (`"{a}{b}"`) rather than an explicit
  concatenation call — interpolation already lowers to the same operation
  and reads more like the shape of the path being built.

### Core type

```kira
pub type path = { raw: str }
```

A single field holding the path exactly as written. Constructed via
`path_of(s: str)` or `path.from(s: str)`.

### API surface

```kira
impl from[str] for path:
    def from(value: str) -> path

pub def path_of(s: str) -> path

impl show for path:
    def show(self) -> str

extend path:
    pub def as_str(self) -> str
    pub def is_absolute(self) -> bool
    pub def is_relative(self) -> bool
    pub def join(self, component: str) -> path
    pub def parent(self) -> option[path]
    pub def filename(self) -> option[str]
    pub def stem(self) -> option[str]
    pub def extension(self) -> option[str]
    pub def with_extension(self, ext: str) -> path
    pub def with_filename(self, name: str) -> path
    pub def components(self) -> list[str]
    pub def starts_with(self, prefix: path) -> bool
    pub def ends_with(self, suffix: path) -> bool

impl div for path:
    type output = path
    def div(self, other: path) -> path

impl eq for path:
    def eq(self, other: &self) -> bool
```

### Method semantics

- **`from(value)` / `path_of(s)`** — construct a `path` from a string;
  `path_of` is the ergonomic free-function spelling at call sites, mirroring
  `path.from(...)`.
- **`show(self)` / `as_str(self)`** — the path's string representation
  (`self.raw`), enabling printing, interpolation, and access to the
  underlying string.
- **`is_absolute(self)` / `is_relative(self)`** — on Unix, absolute means
  starting with `/`. On Windows, absolute additionally includes paths
  starting with `\`, or a drive-letter prefix (`[A-Za-z]:` followed by a
  separator) — `C:foo` (colon with no following separator) is relative, not
  absolute.
- **`join(component)`** — appends a path component, returning a new path. If
  `component` is itself absolute, it replaces the entire base
  (`path_of("/a").join("/b")` → `path_of("/b")`), matching
  `std::filesystem::path`'s behavior. Otherwise the two are concatenated with
  a single native separator between them, with trailing separators stripped
  from `self` and leading separators stripped from `component` to avoid
  doubling.
- **`parent(self)`** — the parent directory. A bare filename or root-only
  path yields `@none`; a top-level entry (`"/etc"`) yields
  `@some(path_of("/"))`; otherwise the path up to (but not including) the
  last separator.
- **`filename(self)`** — the final component. `@none` if the path ends in a
  separator; the whole raw string if there is no separator; otherwise the
  substring after the last separator.
- **`stem(self)` / `extension(self)`** — derived from `filename()`, splitting
  on the last `.`. **Dotfile rule**: if there is no `.`, or the only `.` is
  the first byte (`.gitignore`), the entire filename is the stem and there is
  no extension. `extension` may be the empty string when the filename ends
  in a dot (`"file."` → `@some("")`). Example: `"archive.tar.gz"` → stem
  `"archive.tar"`, extension `"gz"`.
- **`with_extension(ext)`** — a new path with the extension replaced: if the
  path has a stem, built as `parent().join("{stem}.{ext}")`; otherwise `ext`
  becomes the filename outright. Tolerates (and strips) a leading `.` on
  `ext`.
- **`with_filename(name)`** — a new path with the filename replaced:
  `parent().join(name)` if a parent exists, else `path_of(name)`.
- **`components(self)`** — the path split on runs of one-or-more separators,
  discarding empty components; a leading separator produces no empty leading
  component (`is_absolute()` is the way to test root-ness). Example:
  `"/usr/local/bin"` → `["usr", "local", "bin"]`.
- **`starts_with(prefix)` / `ends_with(suffix)`** — component-wise
  comparison (splits both into components first), not a raw string
  prefix/suffix check. Example: `"/usr/local/bin".starts_with(path_of(
  "/usr"))` → `true`.
- **`a / b` (div operator)** — shorthand for `a.join(b.as_str())`, meant for
  chaining: `path_of("dir") / path_of("subdir") / path_of("file.txt")`. The
  right-hand side must be a `path`; `.join(str)` is the entry point for
  appending a plain string component.
- **`eq(self, other)`** — `true` iff both paths have identical raw string
  content, byte-for-byte. Deliberately does **not** normalize:
  `"a/b"` ≠ `"a/b/"`. Normalization is a distinct, separately-scoped concern
  (see Guarantees and limitations).

### Implementation shape

- **Separator predicate.** A byte is a separator when it is `/` (always), or
  — when the target is Windows — also when it is `\`.
- **Emitting separators.** Any operation that constructs a new path
  (`join`, `with_filename`, `with_extension`) always emits the *native*
  separator for the target: `/` on Unix-like systems, `\` on Windows.
- **Scanning helpers.** A reverse index scan locates the last separator; a
  helper strips trailing separators; byte-slice equality backs `eq`,
  `starts_with`, and `ends_with`.
- **Parsing filename/stem/extension.** Strip trailing separators, find the
  last separator in what remains (if any), then slice the portion after it
  (or the whole string, if there is no separator).
- **Splitting into components.** Strip trailing separators, then walk with
  two cursors: on hitting a separator, push the non-empty range collected so
  far as a component, and advance past the entire run of consecutive
  separators before resuming.

### Guarantees and limitations

- **String-based, not filesystem-based.** A `path` value is just a string;
  it never checks whether the path exists or names an actual file or
  directory. This is the deliberate boundary with the future `std.fs`
  module, not an oversight.
- **No path normalization.** `..` and `.` components are not collapsed and
  symlinks are not resolved — `path_of("a/../b")` treats `..` as an ordinary
  component, never as a "go up one level" directive.
- **No UNC path support.** Windows UNC paths (`\\server\share\...`) are not
  recognized; only simple drive-letter (`C:\...`) and Unix-style (`/...`)
  absolute paths are.
- **Byte-level operations.** All component/extension/stem logic works at the
  byte level with no understanding of UTF-8 grapheme boundaries; slicing a
  multi-byte UTF-8 sequence in the wrong place produces a syntactically
  invalid `str`. (The scans themselves are safe by construction, since they
  only ever cut at ASCII separator/dot bytes, which UTF-8's self-
  synchronization guarantees can't appear inside a multi-byte sequence.)
- **Platform conventions from a runtime check.** Separator handling is
  decided by consulting `std.platform` at runtime rather than compiling two
  separate variants, so one binary adapts its behavior across targets it was
  built for.

### Dependencies

- **`std.string`** — equality and substring search back `eq`,
  `starts_with`, `ends_with`, and the scanning helpers above.
- **`std.platform`** — target/host classification decides separator
  conventions and absolute-path rules.
