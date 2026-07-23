# 57. `std.fs.path`

**Status:** Implemented

The `path` value type: string-based construction, joining, and decomposition of filesystem paths.

`std.fs.path` (`src/std/fs/path.kira`) is modeled after C++'s `std::filesystem::path`, but is deliberately **purely a string-manipulation library** — no syscalls, no filesystem access. Checking existence or querying whether a path names a directory or symlink belongs to a future `std.fs` module layered on top of this one, not here: separating "what does this path string mean" from "what does the filesystem say about it" keeps this module usable without I/O permissions and its behavior fully deterministic and host-independent.

Guiding choices:

- **String-backed, lazily parsed.** A path is a plain string (`raw: str`), never eagerly split into components; every operation performs a linear byte scan as needed, so a caller who only calls `is_absolute()` never pays for component-splitting they didn't ask for.
- **Platform-aware, host-independent.** Separator conventions (`/` vs. `\`, drive-letter absoluteness) are decided at runtime by consulting [`std.platform`](56-std-platform.md)'s `is_windows()`, not hardcoded to one OS.
- **Built on `std.string`.** Equality and substring search back `eq`, `starts_with`, `ends_with`, and the scanning helpers; new path strings are assembled with string interpolation rather than an explicit concatenation call.

## Core type

```kira
pub type path = { raw: str }
```

A single field holding the path exactly as written.

## API surface

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

This matches `src/std/fs/path.kira` exactly; there is no drift between the design doc and the implementation.

## Method semantics

- **`from(value)` / `path_of(s)`** — construct a `path` from a string; `path_of` is the ergonomic free-function spelling, mirroring `path.from(...)`.
- **`show(self)` / `as_str(self)`** — the path's string representation (`self.raw`).
- **`is_absolute(self)` / `is_relative(self)`** — on Unix, absolute means starting with `/`. On Windows, absolute additionally includes paths starting with `\`, or a drive-letter prefix (`[A-Za-z]:` followed by a separator) — `C:foo` (colon with no following separator) is relative, not absolute.
- **`join(component)`** — appends a path component, returning a new path. If `component` is itself absolute, it replaces the entire base (`path_of("/a").join("/b")` → `path_of("/b")`), matching `std::filesystem::path`'s behavior. Otherwise the two are concatenated with a single native separator between them, with trailing separators stripped from `self` and leading separators stripped from `component` to avoid doubling.
- **`parent(self)`** — the parent directory. A bare filename or root-only path yields `@none`; a top-level entry (`"/etc"`) yields `@some(path_of("/"))`; otherwise the path up to (but not including) the last separator.
- **`filename(self)`** — the final component. `@none` if the path ends in a separator; the whole raw string if there is no separator; otherwise the substring after the last separator.
- **`stem(self)` / `extension(self)`** — derived from `filename()`, splitting on the last `.`. Dotfile rule: if there is no `.`, or the only `.` is the first byte (`.gitignore`), the entire filename is the stem and there is no extension. `extension` may be the empty string when the filename ends in a dot (`"file."` → `@some("")`). Example: `"archive.tar.gz"` → stem `"archive.tar"`, extension `"gz"`.
- **`with_extension(ext)`** — a new path with the extension replaced: if the path has a stem, built as `with_filename("{stem}.{ext}")`; otherwise `ext` becomes the filename outright. Tolerates (and strips) a leading `.` on `ext`.
- **`with_filename(name)`** — a new path with the filename replaced: `parent().join(name)` if a parent exists, else `path_of(name)`.
- **`components(self)`** — the path split on runs of one-or-more separators, discarding empty components; a leading separator produces no empty leading component (`is_absolute()` is the way to test root-ness). Example: `"/usr/local/bin"` → `["usr", "local", "bin"]`.
- **`starts_with(prefix)` / `ends_with(suffix)`** — component-wise comparison (splits both into components first), not a raw string prefix/suffix check. Example: `"/usr/local/bin".starts_with(path_of("/usr"))` → `true`.
- **`a / b` (div operator)** — shorthand for `a.join(b.as_str())`, meant for chaining: `path_of("dir") / path_of("subdir") / path_of("file.txt")`. The right-hand side must be a `path`; `.join(str)` is the entry point for appending a plain string component.
- **`eq(self, other)`** — `true` iff both paths have identical raw string content, byte-for-byte. Deliberately does **not** normalize: `"a/b"` ≠ `"a/b/"`. Normalization is a distinct, separately-scoped concern (see Limitations below).

## Implementation shape

- **Separator predicate** (`is_sep`) — a byte is a separator when it is `/` (always), or — when `std.platform.is_windows()` — also when it is `\`.
- **Native separator** (`native_sep`) — any operation that constructs a new path (`join`, `with_filename`, `with_extension`) always emits the native separator for the target: `/` on Unix-like systems, `\` on Windows.
- **Scanning helpers** — `last_sep_index`/`last_dot_index` do a reverse byte-index scan; `strip_trailing_seps` strips trailing separators; `strip_leading_dot` strips a leading `.` from an extension argument; byte-slice equality (via `std.string`'s `eq`) backs `eq`, `starts_with`, and `ends_with`.
- **Parsing filename/stem/extension** — strip trailing separators, find the last separator in what remains (if any), then slice the portion after it (or the whole string, if there is no separator).
- **Splitting into components** — strip trailing separators, then walk with two cursors: on hitting a separator, push the non-empty range collected so far as a component, and advance past the entire run of consecutive separators before resuming.

## Guarantees and limitations

- **String-based, not filesystem-based.** A `path` value is just a string; it never checks whether the path exists or names an actual file or directory. This is the deliberate boundary with the future `std.fs` module, not an oversight.
- **No path normalization.** `..` and `.` components are not collapsed and symlinks are not resolved — `path_of("a/../b")` treats `..` as an ordinary component, never as a "go up one level" directive.
- **No UNC path support.** Windows UNC paths (`\\server\share\...`) are not recognized; only simple drive-letter (`C:\...`) and Unix-style (`/...`) absolute paths are.
- **Byte-level operations.** All component/extension/stem logic works at the byte level with no understanding of UTF-8 grapheme boundaries; slicing a multi-byte UTF-8 sequence in the wrong place produces a syntactically invalid `str`. The scans themselves are safe by construction, since they only ever cut at ASCII separator/dot bytes, which UTF-8's self-synchronization guarantees can't appear inside a multi-byte sequence.
- **Platform conventions from a runtime check, not a compile-time one.** Separator handling is decided by consulting `std.platform` at runtime rather than compiling two separate variants, so one binary adapts its behavior across targets it was built for.

These limitations are permanent design boundaries, not outstanding work — normalization, UNC support, and filesystem access are explicitly out of scope for this module.

## Dependencies

- `std.string` (see the strings-and-formatting section) — equality and substring search back `eq`, `starts_with`, `ends_with`, and the scanning helpers.
- [`std.platform`](56-std-platform.md) — `is_windows()` decides separator conventions and absolute-path rules.

## See also

- [`std.platform`](56-std-platform.md) — the target/host classification this module consults for separator conventions.
