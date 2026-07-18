# std.fs.path: Filesystem path string manipulation

## Overview

`std.fs.path` provides a `path` value type for constructing, joining, and decomposing filesystem paths as strings, modeled after C++'s `std::filesystem::path`. It is purely a string-manipulation library — no syscalls, no actual filesystem access. Path validation, checking existence, or querying properties (is directory, is symlink, etc.) are out of scope and belong to a future `std.fs` I/O module.

## Design principles

- **String-backed, lazily parsed**: paths are stored as plain strings (`raw: str`), not eagerly split into components. Every operation performs a linear scan as needed, avoiding allocation overhead for callers who only call `.is_absolute()` or similar quick checks.

- **Platform-aware but host-independent**: the module detects the target OS at runtime via `std.platform.is_windows()` to decide separator conventions, allowing code to run correctly on both Unix and Windows targets from a single codebase. Actual cross-platform execution is untested by the standard test harness (which compiles for the host), but the separation of concerns is clean.

- **Built on `std.string` via string interpolation**: leverages string operations (equality, substring searching) from `std.string` where applicable, and uses **string interpolation** (`"{a}{b}"`, not explicit `rt_str_concat` calls) to build new path strings, matching how `std.platform.kira` already builds multi-part strings.

## Core type

```kira
pub type path = { raw: str }
```

A single field holding the path as written. Callers construct paths via `path_of(s: str)` or `path.from(s: str)`.

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

### Method descriptions

- **`from(value: str)`** → `path`: constructs a path from a string.

- **`path_of(s: str)`** → `path`: ergonomic free-function alias for `path.from(s)`. More natural at call sites than `path.from(...)`.

- **`show(self)`** → `str`: returns the path's string representation (same as `self.raw`), enabling printing and interpolation.

- **`as_str(self)`** → `str`: returns the underlying string. Synonym for `.show()` for API consistency.

- **`is_absolute(self)`** → `bool`: returns `true` if the path is an absolute path.
  - **Unix**: a path is absolute if it starts with `/`.
  - **Windows** (when `std.platform.is_windows()` is true): a path is absolute if it starts with `/` or `\`, or if it begins with a drive-letter prefix (`[A-Za-z]:` followed by a separator). `C:foo` (colon but no following separator) is relative, not absolute.

- **`is_relative(self)`** → `bool`: returns `true` if the path is relative.

- **`join(component: str)`** → `path`: appends a path component, returning a new path.
  - If `component` is absolute (parsed as a path), returns a path equal to `component` — the component replaces the entire base. E.g., `path_of("/a").join("/b")` returns `path_of("/b")`. This matches C++'s behavior.
  - Otherwise, concatenates with a single native separator (`/` on Unix, `\` on Windows) between them, stripping trailing separators from `self.raw` and leading separators from `component` to avoid doubling.

- **`parent(self)`** → `option[path]`: returns the parent directory.
  - Bare filename or no separator → `@none`.
  - Root or root-only path → `@none`.
  - Top-level entry (e.g., `"/etc"`) → `@some(path_of("/"))`.
  - Otherwise → path up to (but not including) the last separator.

- **`filename(self)`** → `option[str]`: returns the final component.
  - Path ends in separator → `@none`.
  - No separator → `@some(self.raw)`.
  - Otherwise → substring after the last separator.

- **`stem(self)`** → `option[str]`: returns the filename without its extension.
  - Derived from `filename()`; `@none` if that is `@none`.
  - Scans for the last `.` to separate name from extension.
  - **Dotfile rule**: if there is no `.`, or the only `.` is the first byte (e.g., `.gitignore`), the entire filename is the stem and there is no extension.
  - Otherwise, the stem is everything before the last `.`.
  - Example: `"archive.tar.gz"` → stem `"archive.tar"`.

- **`extension(self)`** → `option[str]`: returns the filename's extension.
  - Derived from `filename()`; `@none` if that is `@none`.
  - Uses the same last-dot logic as `stem()`.
  - `@none` if there is no `.` or the only `.` is the first byte.
  - May be empty if the filename ends in a dot: `"file."` has extension `@some("")`.

- **`with_extension(ext: str)`** → `path`: returns a new path with the extension replaced.
  - If the path has a stem, constructs `parent().join("{stem}.{ext}")`.
  - Otherwise returns a path with `ext` as the filename.
  - Tolerates a leading `.` on `ext` (strips it if present).

- **`with_filename(name: str)`** → `path`: returns a new path with the filename replaced.
  - Calls `parent().join(name)` if a parent exists, otherwise returns `path_of(name)`.

- **`components(self)`** → `list[str]`: returns a list of path components split on separators.
  - Splits on runs of one-or-more separators; discards empty components.
  - Leading separator produces no empty leading component — use `is_absolute()` to test root-ness.
  - Example: `"/usr/local/bin"` → `["usr", "local", "bin"]`.

- **`starts_with(prefix: path)`** → `bool`: returns `true` if this path starts with the given prefix (component-wise).
  - Splits both into components and compares.
  - Example: `"/usr/local/bin".starts_with(path_of("/usr"))` → `true`.

- **`ends_with(suffix: path)`** → `bool`: returns `true` if this path ends with the given suffix (component-wise).
  - Example: `"/usr/local/bin".ends_with(path_of("local/bin"))` → `true`.

- **`a / b` (div operator)** → `path`: shorthand for `a.join(b.as_str())`. Convenience for chaining: `path_of("dir") / path_of("subdir") / path_of("file.txt")`. Note: RHS must be a `path`; use `.join(str)` to append a string component.

- **`eq(self, other: &self)`** → `bool`: returns `true` if both paths have the same raw string content byte-for-byte. Does not normalize; `"a/b"` ≠ `"a/b/"`.

## Implementation notes

### Separator predicate (`is_sep`)

A byte is a separator when:
- It equals `/` (always), OR
- (when `std.platform.is_windows()` is true) it equals `\`

The `is_windows()` check is runtime-gated, not compile-time `static if`, matching the pattern already used in `std.platform.kira`.

### Emitting separators

When constructing a new path (via `join`, `with_filename`, `with_extension`), always emit the native separator:
- `/` on Unix-like systems
- `\` on Windows (when `std.platform.is_windows()` is true)

### String construction

String concatenation uses **string interpolation** (`"{a}{b}"`), not explicit `rt_str_concat` calls. Interpolation is lowered to `rt_str_concat` by the compiler internally, and is the proven pattern already used in `std.platform.kira` (e.g., building a multi-part platform string). This sidesteps visibility constraints and follows established convention.

### Scanning helpers

- **`last_sep_index(raw: str) -> option[usize]`**: reverse index scan to find the byte-index of the last separator.
- **`strip_trailing_seps(raw: str) -> str`**: returns a new string with trailing separators removed.
- **`is_equal_slices(a: slice[byte], b: slice[byte]) -> bool`**: byte-for-byte slice equality, used by `eq`, `starts_with`, `ends_with`.

### Parsing logic

When extracting filename, stem, or extension:
1. Strip trailing separators from the raw string.
2. Find the last separator in the stripped string (if any).
3. Slice the portion after the separator (or the whole string if no separator exists).

When splitting into components:
1. Strip trailing separators.
2. Walk with two cursors, marking the start and advancing ahead.
3. On hitting a separator, push the non-empty range as a component.
4. Advance past the entire run of consecutive separators before resuming.

## Guarantees and limitations

- **String-based, not filesystem-based**: a `path` value is just a string; it does not check whether the path exists or refer to actual files/directories.

- **No path normalization**: no collapsing `..` or `.` components or resolving symlinks. E.g., `path_of("a/../b")` treats `..` as a regular component, not a "go up one level" directive.

- **No UNC path support**: Windows UNC paths (`\\server\share\...`) are not recognized; only simple drive-letter (`C:\...`) and Unix-style (`/...`) absolute paths.

- **Byte-level operations**: all component/extension/stem logic works at the byte level with no understanding of UTF-8 or Unicode. Slicing a multi-byte UTF-8 sequence produces a syntactically invalid `str`.

- **Platform conventions from runtime check**: separator handling is determined by `std.platform.is_windows()` at runtime, allowing one binary to adapt behavior across platforms.

## Testing

Test fixtures in `src/testdata/std_test/`:
- `path_join.kira` — joining, doubled-separator handling, absolute-RHS replacement
- `path_parent.kira` — parent extraction, root-stays-root, cascading to `@none`
- `path_filename_stem_extension.kira` — final component, stem, extension; multi-dot names
- `path_dotfile.kira` — dotfile edge case (leading dot ≠ extension marker)
- `path_absolute_relative.kira` — `is_absolute()` and `is_relative()`
- `path_components.kira` — component extraction; collapsed separators
- `path_eq.kira` — equality checks
- `path_div_operator.kira` — `path / path` operator dispatch

Note: True cross-platform separator recognition is an untested gap (CI runs on the host platform only).

## Dependencies

- **`std.string`**: used for equality and substring searches via `std.string.eq()`, `.starts_with()`, etc.
- **`std.platform`**: runtime `is_windows()` check for separator conventions.
- No `use` declarations needed — cross-module references are fully qualified (matching the pattern in `console.kira`/`platform.kira`).
