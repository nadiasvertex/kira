# `std.platform`

Cross-platform and platform-specific runtime and compile-time introspection. This module provides the information needed to adapt behavior to the host system, to log diagnostic context, and to inspect the Kira toolchain that produced the running binary.

---

## Design Principles

- **Compile-time target vs. runtime host.** Kira binaries are compiled for a target triple (`arch-vendor-os-env`). Most properties of the *target* — architecture, pointer width, endianness, OS family — are known when the binary is built and are exposed as `static` constants. Properties of the *host* the binary is running on — hostname, detailed OS release, processor name — are queried at runtime through thin intrinsics.
- **Best-effort convenience, explicit precision.** `uname()` returns a struct whose runtime-queried fields are `option[str]`, making "could not determine" (`@none`) distinguishable from "genuinely empty" (`@some("")`). The `show` implementation collapses `@none` into `"unknown"` for the common "print it and continue" use case. Individual field queries (`node()`, `release()`, `processor()`) return `result[T, io_error]` so that code that must handle failure explicitly can do so.
- **Platform-specific functions return `option`, not `result`.** Calling `libc_ver()` on Windows is not an error — the information simply does not apply. Returning `option` makes `if let` the natural way to consume platform-specific data.
- **Extensible sum types.** `architecture` and `os_family` enumerate the common cases but carry an `@other(str)` variant so that ports to new platforms do not require immediate standard library changes.
- **`static if` at module boundaries.** Platform-specific intrinsics and parsing helpers are compiled only on the platform they target. A Windows binary does not contain `rt_uname()` or the code that parses POSIX `utsname` buffers.

---

## Compile-Time Target Constants

These are provided by the compiler driver and build system. They describe the platform the binary was compiled **for** and are available as ordinary `static` constants.

```kira
pub static TARGET_ARCH: architecture = ...           # e.g. @x86_64
pub static TARGET_OS: str = ...                    # e.g. "linux", "windows", "macos"
pub static TARGET_VENDOR: str = ...                # e.g. "unknown", "pc", "apple"
pub static TARGET_ENV: str = ...                  # e.g. "gnu", "musl", "msvc"
pub static TARGET_ENDIAN: endianness = ...         # @little or @big
pub static TARGET_POINTER_WIDTH: usize = ...        # 4 or 8

pub static KIRA_VERSION: str = "0.1.0"
pub static KIRA_IMPLEMENTATION: str = "kira"
pub static KIRA_COMPILER: str = "llvm"
pub static KIRA_BUILD_DATE: str = "2026-07-13"
```

---

## Types

### Platform classification

```kira
pub type architecture =
    | @x86_64
    | @aarch64
    | @arm
    | @riscv32
    | @riscv64
    | @wasm32
    | @wasm64
    | @other(str)
    deriving eq, show

pub type os_family =
    | @unix
    | @windows
    | @macos
    | @other(str)
    deriving eq, show

pub type endianness = @little | @big
    deriving eq, show
```

### Information structs

```kira
pub type platform_info = {
    system: str,                       # always known at compile time — target OS
    node: option[str],
    release: option[str],
    version: option[str],
    machine: architecture,            # always known at compile time
    processor: option[str],
    endianness: endianness,             # always known at compile time
    pointer_width: usize,              # always known at compile time
    os_family: os_family,               # always known at compile time
}

pub type kira_build = {
    version: str,
    implementation: str,
    compiler: str,
    build_date: str,
}

pub type libc_version = {
    name: str,
    version: str,
}

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
    machine: str,
}
```

---

## Intrinsics

Platform-specific intrinsics are compiled only on the platforms that need them. Each is a thin wrapper around one OS call.

```kira
static if target_os_family() == @unix:
    type uname_raw = {
        sysname: str,
        nodename: str,
        release: str,
        version: str,
        machine: str,
    }
    intrinsic def rt_uname() -> result[uname_raw, io_errno]
    intrinsic def rt_libc_version() -> result[str, io_errno]

static if target_os_family() == @windows:
    type winver_raw = {
        major: uint32,
        minor: uint32,
        build: uint32,
        platform_id: uint32,
        csd_version: str,
    }
    intrinsic def rt_windows_version() -> result[winver_raw, io_errno]

static if target_os_family() == @macos:
    type macver_raw = {
        release: str,
        version: str,
        dev_stage: str,
        non_release_version: str,
        machine: str,
    }
    intrinsic def rt_macos_version() -> result[macver_raw, io_errno]

# Available on all platforms where a processor identifier can be queried.
intrinsic def rt_processor_name() -> result[str, io_errno]
```

---

## Target Queries (Compile-Time)

These functions return the `static` constants above. They are `pure` — referentially transparent — because the target never changes at runtime.

```kira
pub pure def target_arch() -> architecture: TARGET_ARCH
pub pure def target_os() -> str: TARGET_OS
pub pure def target_vendor() -> str: TARGET_VENDOR
pub pure def target_env() -> str: TARGET_ENV
pub pure def target_endianness() -> endianness: TARGET_ENDIAN
pub pure def target_pointer_width() -> usize: TARGET_POINTER_WIDTH

pub pure def target_os_family() -> os_family:
    classify_os_family(TARGET_OS)
```

### Convenience predicates

```kira
pub pure def is_unix() -> bool: target_os_family() == @unix
pub pure def is_windows() -> bool: target_os_family() == @windows
pub pure def is_macos() -> bool: target_os_family() == @macos
```

---

## Runtime Queries

These query the host system at runtime and may fail if the underlying OS call fails.

```kira
pub def node() -> result[str, io_error]:
    static if target_os_family() == @unix:
        rt_uname().map_err(io_error.from).map(raw => raw.nodename)
    else:
        # Windows and other platforms: separate intrinsic or fallback
        rt_gethostname().map_err(io_error.from)

pub def release() -> result[str, io_error]:
    static if target_os_family() == @unix:
        rt_uname().map_err(io_error.from).map(raw => raw.release)
    static if target_os_family() == @windows:
        rt_windows_version().map_err(io_error.from).map(raw => raw.release)
    static if target_os_family() == @macos:
        rt_macos_version().map_err(io_error.from).map(raw => raw.release)
    else:
        @err(io_error.from(io_errno { code: 0 }))  # ENOSYS equivalent

pub def version() -> result[str, io_error]:
    static if target_os_family() == @unix:
        rt_uname().map_err(io_error.from).map(raw => raw.version)
    static if target_os_family() == @windows:
        rt_windows_version().map_err(io_error.from).map(raw => raw.version)
    static if target_os_family() == @macos:
        rt_macos_version().map_err(io_error.from).map(raw => raw.version)
    else:
        @err(io_error.from(io_errno { code: 0 }))

pub def processor() -> result[str, io_error]:
    rt_processor_name().map_err(io_error.from)
```

---

## Cross-Platform Convenience

### `uname` — structured platform info

Best-effort: fields that cannot be determined are `@none`. This is the common-case convenience; it never fails.

```kira
pub def uname() -> platform_info:
    platform_info {
        system: target_os(),
        node: node().ok(),
        release: release().ok(),
        version: version().ok(),
        machine: target_arch(),
        processor: processor().ok(),
        endianness: target_endianness(),
        pointer_width: target_pointer_width(),
        os_family: target_os_family(),
    }
```

### `platform` — human-readable string

A single string in the style of Python's `platform.platform()`, useful for logging and bug reports. Never fails.

```kira
pub def platform() -> str:
    let rel = match release(): @ok(s) => s @err(_) => "unknown"
    let proc = match processor(): @ok(s) => s @err(_) => "unknown"
    return "{target_os()}-{rel}-{target_arch().show()}-{proc}"
```

---

## Kira Runtime Information

Information about the Kira toolchain that produced the running binary. All compile-time constants.

```kira
pub pure def kira_version() -> str: KIRA_VERSION
pub pure def kira_implementation() -> str: KIRA_IMPLEMENTATION
pub pure def kira_compiler() -> str: KIRA_COMPILER

pub pure def kira_build_info() -> kira_build:
    kira_build {
        version: KIRA_VERSION,
        implementation: KIRA_IMPLEMENTATION,
        compiler: KIRA_COMPILER,
        build_date: KIRA_BUILD_DATE,
    }
```

---

## Platform-Specific Queries

These return `@none` when called on a platform other than the one they describe. The caller uses `if let` to consume them.

### Unix — libc version

```kira
pub def libc_ver() -> option[libc_version]:
    if not is_unix(): return @none
    match rt_libc_version().map_err(io_error.from):
        @ok(v) => @some(parse_libc_version(v))
        @err(_) => @none
```

### Windows — version details

```kira
pub def win32_ver() -> option[windows_version]:
    if not is_windows(): return @none
    match rt_windows_version().map_err(io_error.from):
        @ok(v) => @some(parse_windows_version(v))
        @err(_) => @none
```

### macOS — version details

```kira
pub def mac_ver() -> option[macos_version]:
    if not is_macos(): return @none
    match rt_macos_version().map_err(io_error.from):
        @ok(v) => @some(parse_macos_version(v))
        @err(_) => @none
```

---

## Internal Helpers

```kira
file pure def classify_os_family(os: str) -> os_family:
    let lower = os.to_lowercase()
    match lower:
        "linux" => @unix
        "freebsd" => @unix
        "openbsd" => @unix
        "netbsd" => @unix
        "dragonfly" => @unix
        "windows" => @windows
        "win32" => @windows
        "darwin" => @macos
        "macos" => @macos
        _ => @other(os)

file def parse_libc_version(s: str) -> libc_version:
    let parts = s.split(" ")
    if parts.len() >= 2:
        libc_version { name: parts[0], version: parts[1] }
    else:
        libc_version { name: s, version: "" }

file def parse_windows_version(raw: winver_raw) -> windows_version:
    windows_version {
        release: "{raw.major}.{raw.minor}",
        version: "{raw.major}.{raw.minor}.{raw.build}",
        csd_version: raw.csd_version,
        platform_type: "{raw.platform_id}",
    }

file def parse_macos_version(raw: macver_raw) -> macos_version:
    macos_version {
        release: raw.release,
        version: raw.version,
        dev_stage: raw.dev_stage,
        non_release_version: raw.non_release_version,
        machine: raw.machine,
    }
```

---

## `show` Implementations

```kira
impl show for architecture:
    def show(self) -> str:
        match self:
            @x86_64 => "x86_64"
            @aarch64 => "aarch64"
            @arm => "arm"
            @riscv32 => "riscv32"
            @riscv64 => "riscv64"
            @wasm32 => "wasm32"
            @wasm64 => "wasm64"
            @other(s) => s

impl show for os_family:
    def show(self) -> str:
        match self:
            @unix => "unix"
            @windows => "windows"
            @macos => "macos"
            @other(s) => s

impl show for endianness:
    def show(self) -> str:
        match self:
            @little => "little"
            @big => "big"

impl show for platform_info:
    def show(self) -> str:
        let n = match self.node: @some(s) => s @none => "unknown"
        let r = match self.release: @some(s) => s @none => "unknown"
        let v = match self.version: @some(s) => s @none => "unknown"
        let p = match self.processor: @some(s) => s @none => "unknown"
        "uname(system={self.system}, node={n}, release={r},          version={v}, machine={self.machine.show()}, processor={p})"

impl show for kira_build:
    def show(self) -> str:
        "kira_build(version={self.version}, compiler={self.compiler},          build_date={self.build_date})"

impl show for libc_version:
    def show(self) -> str:
        "libc_version(name={self.name}, version={self.version})"

impl show for windows_version:
    def show(self) -> str:
        "win32_ver(release={self.release}, version={self.version},          csd={self.csd_version})"

impl show for macos_version:
    def show(self) -> str:
        "mac_ver(release={self.release}, version={self.version})"
```

---

## Usage Examples

```kira
use std.platform

def main() -> unit:
    # Compile-time target info — zero runtime cost
    println("Compiled for {target_arch().show()}-{target_os()}")
    println("Pointer width: {target_pointer_width()}")

    # Runtime host info — best-effort
    let info = uname()
    println("Running on: {info.show()}")
    println("Platform string: {platform()}")

    # Explicit error handling
    match node():
        @ok(hostname) => println("Hostname: {hostname}")
        @err(e) => println("Could not get hostname: {e.show()}")

    # Platform-specific code
    if let @some(libc) = libc_ver():
        println("libc: {libc.name} {libc.version}")

    if let @some(win) = win32_ver():
        println("Windows release: {win.release}")

    # Kira toolchain info
    println("Kira version: {kira_version()}")
    println("Kira build: {kira_build_info().show()}")
```

---

## Summary of Design Decisions

| Decision | Rationale |
|---|---|
| Target info is `static` / `pure` | The target triple is baked into the binary; there is no runtime cost and no failure mode |
| Host info is `result[T, io_error]` | OS calls can genuinely fail; the type forces the caller to decide what to do |
| `uname()` uses `option[str]` for runtime fields | `@none` (could not query) and `@some("")` (genuinely empty from OS) are distinguishable; no magic-value sentinel |
| `platform()` never fails | It is a formatting convenience; `"unknown"` is an acceptable fallback |
| Platform-specific functions return `option` | Calling `libc_ver()` on Windows is not an error — the information simply does not apply |
| `architecture` / `os_family` are sum types with `@other` | Makes invalid states unrepresentable for known platforms while remaining extensible |
| `static if` guards platform-specific intrinsics | A Windows binary does not contain POSIX `uname` parsing code, and vice versa |
| `is_unix()`, `is_windows()`, `is_macos()` predicates | Common branches that read better than `target_os_family() == @unix` |
