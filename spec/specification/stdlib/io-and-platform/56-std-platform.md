# 56. `std.platform`

**Status:** Implemented

Compile-time target introspection and runtime host introspection: architecture, OS family, endianness, and Kira toolchain build metadata.

`std.platform` (`src/std/platform.kira`) resolves a tension between two kinds of question a program can ask about "the platform":

- **Target** — architecture, pointer width, endianness, OS family — is knowable when the binary is built, computed once per compile from the toolchain's own preprocessor knowledge (`__x86_64__`, `__APPLE__`, `__linux__`, and similar), and exposed as `pure` accessor functions. Kira has no `--target` flag or cross-compilation support, so "target" and "the host the compiler itself was built on" are currently identical.
- **Host** — hostname, detailed OS release, processor name — can only be known by asking the OS at runtime, through thin intrinsics.

Some of this module's declarations are not present in the checked-in `src/std/platform.kira` source: the target/build-info accessor functions (`target_arch`, `target_os`, `target_os_family`, `target_vendor`, `target_env`, `target_endianness`, `target_pointer_width`, `kira_version`, `kira_implementation`, `kira_compiler`, `kira_build_date`) are generated and spliced in by the driver (`generate_platform_target_accessors`/`assemble_platform_module_source`, `src/driver/driver.cpp`) at compile time, each embedding its value as a literal directly in the function body. They cannot be declared in the checked-in file itself — a module path can only be declared by one file — and are generated as functions rather than `pub static` constants because of a bytecode-lowering bug with that alternative (see `generate_platform_target_accessors`'s doc comment). `target_os_family()` in particular is classified in C++ (`detect_target_os`) and baked in directly, rather than computed at runtime from `target_os()`'s string, because neither backend implements `str ==` yet for the general case.

## Types

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

`architecture` and `os_family` are extensible sum types (`@other(str)`), so an unsupported port stays representable without a stdlib change.

Every field or parameter that would naturally be called `machine` is named `machine_arch` instead, because `machine` is a reserved keyword — the function modifier opting a function body into machine-level arithmetic.

## Intrinsics

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

All six are declared unconditionally rather than gated per platform: a native implementation (`src/runtime/platform.cpp`, `src/bytecode/vm.cpp`) reports `io_errno { code: ENOSYS }` when queried on a host it doesn't describe, so an inapplicable platform costs code size, not correctness. `rt_libc_version` returns the `libc_version` struct directly rather than a raw string the caller would have to split, so `std.platform` never needs string splitting for its own purposes.

`io_error`/`io_errno` are declared in `std.io`; because a struct literal's type name must be a single identifier (the parser does not accept a qualified path directly before `{`), this module aliases them locally:

```kira
type io_error = std.io.io_error
type io_errno = std.io.io_errno
```

## Target queries (compile-time)

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

All `pure`, since the target never changes at runtime. `is_unix`/`is_windows`/`is_macos` `match` on `target_os_family()` rather than comparing with `==`, since sum-type `==` has no runtime codegen on either backend (only the compile-time evaluator understands it).

## Runtime queries

```kira
pub def node() -> result[str, io_error]
pub def processor() -> result[str, io_error]
pub def release() -> result[str, io_error]
pub def version() -> result[str, io_error]
```

`node`/`processor` propagate `rt_gethostname`/`rt_processor_name` directly via `?`.

`release`/`version` genuinely differ per platform but are each a single unconditional definition dispatching internally on the runtime value of `target_os_family()`, rather than one definition per platform selected by `static if`: item-level `static if`/`else` only affects that file's own compile-time checking, it does not splice a definition into the module's ordinary scope for other code to call.

```kira
pub def release() -> result[str, io_error]:
    match target_os_family():
        @unix    =>: let raw = rt_uname()?; return @ok(raw.release)
        @windows =>: let raw = rt_windows_version()?; return @ok("{raw.major}.{raw.minor}")
        @macos   =>: let raw = rt_macos_version()?; return @ok(raw.release)
        @other(_) => @err(io_error.from(io_errno { code: 0 }))
```

`version()` follows the same shape, formatting `"{major}.{minor}.{build}"` on the Windows arm.

Error propagation throughout this module uses `?`, not `.map`/`.map_err`/`.ok()`: `?` is Kira's one propagation primitive, and converts a failing intrinsic's `io_errno` to `io_error` implicitly via `impl from[io_errno] for io_error` (see [`std.io`](54-std-io.md)). Swallowing an error into `option` instead of propagating it is spelled with an explicit `match` (see `result_to_option` below).

## Cross-platform convenience

```kira
pub def uname() -> platform_info
pub def platform() -> str
pub def kira_build_info() -> kira_build
```

- `uname()` assembles a `platform_info`: target fields (`system`, `machine_arch`, `endianness`, `pointer_width`, `os_family`) come from the `pure` target queries directly; host fields (`node`, `release`, `version`, `processor`) come from the runtime queries above, each converted from `result` to `option` by a private helper so a query failure becomes `@none` rather than propagating. `@none` (could not determine) is distinguishable from `@some("")` (genuinely empty from the OS) — there is no sentinel value.
- `platform()` never fails: it formats `"{target_os()}-{release}-{arch}-{processor}"`, substituting `"unknown"` for any runtime query that returned `@err`.
- `kira_build_info()` is `pure`: it reads the four driver-generated build accessors (`kira_version`, `kira_implementation`, `kira_compiler`, `kira_build_date`) and assembles a `kira_build`.

## Platform-specific queries

```kira
pub def libc_ver() -> option[libc_version]
pub def win32_ver() -> option[windows_version]
pub def mac_ver() -> option[macos_version]
```

Each first checks the corresponding `is_unix`/`is_windows`/`is_macos` target predicate and returns `@none` immediately if it does not match; otherwise it calls the corresponding intrinsic and returns `@none` on `@err`, `@some(...)` on `@ok`. `win32_ver`/`mac_ver` additionally reshape the raw intrinsic struct (`winver_raw`/`macver_raw`) into the public `windows_version`/`macos_version` type via private helpers `parse_windows_version`/`parse_macos_version`.

Calling a Windows-only query on macOS is not an error — the information simply does not apply — so these return `option`, not `result`, letting `if let` be the natural way to consume platform-specific data, distinct from genuine I/O failure.

## `show` implementations

`architecture`, `os_family`, `endianness`, `platform_info`, `kira_build`, `libc_version`, `windows_version`, and `macos_version` each have a hand-written `impl show`. These are hand-written rather than `deriving`, because `deriving` only generates real code for struct-shaped types today — a sum type (`architecture`, `os_family`, `endianness`) gains nothing from listing it.

## Design decisions

| Decision | Rationale |
|---|---|
| Target/build info are `pure def`s, not `pub static` constants | Every target property is a function of the compile, not stored state |
| Host info is `result[T, io_error]` | OS calls can genuinely fail; the type forces the caller to decide what to do |
| Error propagation uses `?`, not `.map`/`.map_err`/`.ok()` | `?` is the language's one propagation primitive, with implicit `from`-based conversion |
| All intrinsics/raw structs are declared unconditionally | One code path; an inapplicable platform reports "not supported," it doesn't need to not exist |
| `release()`/`version()` are single definitions matching runtime `target_os_family()` | One function per concept, dispatching internally, rather than one function per platform |
| `uname()` uses `option[str]` for runtime fields | `@none` (could not query) and `@some("")` (genuinely empty) are distinguishable |
| `platform()` never fails | It is a formatting convenience; `"unknown"` is an acceptable fallback |
| Platform-specific functions return `option` | Calling `libc_ver()` on Windows is not an error — the information simply does not apply |
| `architecture`/`os_family` are sum types with `@other` | Invalid states unrepresentable for known platforms, while remaining extensible |
| Fields that would be named `machine` are `machine_arch` | `machine` is a reserved function-modifier keyword |

## See also

- [`std.io`](54-std-io.md) — the `io_errno`-to-`io_error` conversion this module reuses.
- [`std.fs.path`](57-std-fs-path.md) — consults `is_windows()` at runtime for separator and absolute-path conventions.
