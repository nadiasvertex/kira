# `std.platform`

Cross-platform and platform-specific runtime and compile-time introspection. This module provides the information needed to adapt behavior to the host system, to log diagnostic context, and to inspect the Kira toolchain that produced the running binary.

`std.platform` is implemented end-to-end: `src/std/platform.kira` (checked in), six native intrinsics (`src/intrinsics.h`, `src/bytecode/vm.cpp`, `src/runtime/platform.cpp`, `src/runtime/platform_query.h`/`.cpp`), and driver-generated target/build-info accessors (`src/driver/driver.cpp`). It runs correctly through both backends — the bytecode VM (`kira SOURCE`) and the LLVM AOT path (`kira --compile`).

---

## Design Principles

- **Compile-time target vs. runtime host.** Most properties of the *target* — architecture, pointer width, endianness, OS family — are known when the binary is built and are exposed as `pub pure def` accessor functions. Properties of the *host* the binary is running on — hostname, detailed OS release, processor name — are queried at runtime through thin intrinsics.
- **No `--target` flag yet.** Kira has no cross-compilation support, so "target" and "the host this compiler binary itself was built on" are the same thing today. The driver computes `target_arch()`/`target_os()`/etc. from C++ preprocessor macros (`__x86_64__`, `__APPLE__`, `__linux__`, ...) once per `kira` invocation.
- **Error propagation uses `?`, not combinators.** Kira does not have `result`/`option` combinator methods (`.map`, `.map_err`, `.ok()`); the primitive is `?`. A function that returns `result[T, io_error]` propagates a failing intrinsic call with `let x = rt_thing()?`, and `?` converts the intrinsic's `io_errno` to `io_error` automatically via the same `from` mechanism used elsewhere in the language (`impl from[io_errno] for io_error`, `std.io`). Where an error must be *swallowed* rather than propagated (turning a `result` into an `@none`), that is spelled with an explicit `match`, since `?` only propagates.
- **Best-effort convenience, explicit precision.** `uname()` returns a struct whose runtime-queried fields are `option[str]`, making "could not determine" (`@none`) distinguishable from "genuinely empty" (`@some("")`). Individual field queries (`node()`, `release()`, `processor()`) return `result[T, io_error]` so that code that must handle failure explicitly can do so.
- **Platform-specific functions return `option`, not `result`.** Calling `libc_ver()` on Windows is not an error — the information simply does not apply. Returning `option` makes `if let` the natural way to consume platform-specific data.
- **Extensible sum types.** `architecture` and `os_family` enumerate the common cases but carry an `@other(str)` variant so that ports to new platforms do not require immediate standard library changes.
- **Intrinsics are declared unconditionally, not gated per platform.** The most natural design — gate `rt_uname`/`rt_windows_version`/`rt_macos_version` behind `static if TARGET_OS_FAMILY == "unix"` etc., so a Windows binary never contains POSIX `uname`-parsing code — runs into two real compiler gaps (see "Compiler Gaps Encountered" below): `intrinsic def` cannot appear inside a `static if` body at all, and even an ordinary `def` selected by item-level `static if`/`else` isn't visible to code outside that `static if`. So all six intrinsics and their raw structs are declared unconditionally; every native implementation reports `io_errno { code: ENOSYS }` gracefully when called on a host it doesn't describe, so nothing but code size is lost.
- **Platform dispatch happens at runtime, not compile time.** `release()`/`version()` `match target_os_family()` (a runtime value) rather than being selected by `static if` — for the same reason as above.

---

## Compiler Gaps Encountered

Implementing this module surfaced several pre-existing compiler limitations, worked around here rather than fixed (each is a real, separately-scoped feature):

| Gap | Symptom | Workaround used here |
|---|---|---|
| ~~`use module.{a, b}` / `use module` don't import names~~ **Fixed 2026-07** — `use module.member`, `use module.{a, b as c}`, and `use module.*` now bind names into unqualified scope on both backends | `import X.Y does not resolve` even for a real `pub` declaration; a bare `use module` silently imports nothing | Reference other modules fully qualified (`std.io.io_error`), matching `std.console`'s existing `std.io.stdout_handle()` style — still works, no longer required |
| Struct literal construction requires a single-identifier type name | `std.io.io_errno { code: 0 }` fails to parse | A local `type io_errno = std.io.io_errno` alias, then `io_errno { code: 0 }` |
| `intrinsic def` cannot be nested inside a `static if` body | "intrinsic declarations cannot be a trait or impl member" | Declare intrinsics unconditionally instead of gating per `TARGET_OS_FAMILY` |
| Item-level `static if`/`else` doesn't splice declarations into module scope for other code to call | `undefined name` for a `pub def` selected by a taken `static if` branch, referenced from a different function | One unconditional function, dispatching on the runtime value `target_os_family()` returns, instead of one function per `static if` branch |
| A bare reference to a non-scalar (`str`, sum type, struct) top-level `static` doesn't lower to bytecode/LLVM | "reference to `X` is not a local binding — a bare function value used outside of call position is not supported yet" | Driver-generated accessors (`target_arch()`, `target_os()`, ...) embed their value as a literal directly in the function body, never as a `pub static` read by name |
| `str == str` has no runtime codegen in either backend | "type `str` has no scalar bytecode/llvm_codegen representation yet ... until spec/codegen-design.md increment 6 (heap types) lands" | `target_os_family()` is generated directly by the driver (which already knows the classification in C++), instead of classifying `target_os()`'s string at runtime |
| `match` on `str` literal patterns has no bytecode codegen | "string literal patterns need increment 5's growable-string comparison, not supported yet" | Avoided; no remaining `match` on string literals in this module |

None of these are `std.platform`-specific — they would block *any* future stdlib module leaning on the same patterns (cross-module `use`, per-platform `static if` dispatch, or runtime `str` comparison) and are worth fixing generally at some point.

---

## Where the Target/Build-Info Constants Come From

`target_arch()`, `target_os()`, `target_os_family()`, `target_vendor()`, `target_env()`, `target_endianness()`, `target_pointer_width()`, `kira_version()`, `kira_implementation()`, `kira_compiler()`, and `kira_build_date()` are **not** in the checked-in `src/std/platform.kira` — the driver (`generate_platform_target_accessors`, `src/driver/driver.cpp`) generates them fresh each invocation from C++ preprocessor macros and splices them into the *front* of `platform.kira`'s body (`assemble_platform_module_source`), writing the assembled result to a temp file that's what actually gets compiled. The checked-in file is a fragment, never fed to the compiler directly.

`KIRA_BUILD_DATE` is `"unknown"` under a `bazelisk build`: Bazel's toolchain redefines `__DATE__` to the literal `"redacted"` for build reproducibility, and the driver detects and reports that case explicitly rather than mis-parsing it.

---

## Types

### Platform classification

```kira
pub type architecture = @x86_64 | @aarch64 | @arm | @riscv32 | @riscv64 | @wasm32 | @wasm64 | @other(str)

pub type os_family = @unix | @windows | @macos | @other(str)

pub type endianness = @little | @big
```

`show` is implemented by hand for each of these below rather than with `deriving`: `deriving` only generates real code for struct-shaped types today, so a sum type gains nothing from listing it and the explicit `impl show` is what actually runs.

### Information structs

```kira
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
    machine_arch: str,
}
```

(`machine`, not `machine_arch`, would read more naturally, but `machine` is a reserved keyword — a function modifier opting into machine-level arithmetic — so every field/parameter that would otherwise be named `machine` is `machine_arch` instead.)

---

## Intrinsics

Every intrinsic is niladic and returns `result[T, io_errno]`. Declared unconditionally (see "Design Principles" above); each native implementation reports `ENOSYS` when called on a host it doesn't describe.

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

Native implementations (`src/runtime/platform_query.h`/`.cpp`, shared by both backends' ABI layers): `::uname`/`::gethostname` (POSIX), `sysctlbyname` for macOS CPU brand string and `kern.osproductversion`, `/proc/cpuinfo` parsing for Linux CPU name, `gnu_get_libc_version()` for glibc, `RtlGetVersion` (via `ntdll`) for genuine (non-manifest-lied) Windows version info. `rt_libc_version` returns the `libc_version` struct directly (name + version), rather than a raw string a caller would need to split — sidesteps `std.platform` ever needing `str.split()`.

---

## Target Queries (Compile-Time)

`pure` because the target never changes at runtime. Driver-generated (see above), except `target_os_family()`'s convenience predicates:

```kira
pub pure def target_arch() -> architecture
pub pure def target_os() -> str
pub pure def target_os_family() -> os_family
pub pure def target_vendor() -> str
pub pure def target_env() -> str
pub pure def target_endianness() -> endianness
pub pure def target_pointer_width() -> usize
```

### Convenience predicates

`os_family` equality has no runtime `==` codegen for user-defined sum types (this part of the language is implemented and unrelated to the gaps above — `deriving eq`'s generated code is compile-time-evaluator-only right now), so these are spelled with `match` rather than `target_os_family() == @unix`:

```kira
pub pure def is_unix() -> bool:
    match target_os_family():
        @unix => true
        _ => false

pub pure def is_windows() -> bool: ...  # same shape, @windows
pub pure def is_macos() -> bool: ...    # same shape, @macos
```

---

## Runtime Queries

`?` propagates a failing intrinsic call, converting `io_errno` to `io_error` via `impl from[io_errno] for io_error` (`std.io`):

```kira
pub def node() -> result[str, io_error]:
    let name = rt_gethostname()?
    return @ok(name)

pub def processor() -> result[str, io_error]:
    let name = rt_processor_name()?
    return @ok(name)
```

`release()`/`version()` genuinely differ per platform. Each is **one** unconditional definition, `match`-ing the *runtime* value `target_os_family()` returns (not `static if` — see "Compiler Gaps Encountered"):

```kira
pub def release() -> result[str, io_error]:
    match target_os_family():
        @unix =>:
            let raw = rt_uname()?
            return @ok(raw.release)
        @windows =>:
            let raw = rt_windows_version()?
            return @ok("{raw.major}.{raw.minor}")
        @macos =>:
            let raw = rt_macos_version()?
            return @ok(raw.release)
        @other(_) => @err(io_error.from(io_errno { code: 0 }))  # ENOSYS equivalent

pub def version() -> result[str, io_error]: ...  # same shape, .version/.build fields
```

(`pattern =>:` followed by an indented statement block is the grammar's multi-statement match-arm form — `pattern => expr` on one line only accepts a single expression.)

---

## Cross-Platform Convenience

```kira
priv def result_to_option(r: result[str, io_error]) -> option[str]:
    match r:
        @ok(v) => @some(v)
        @err(_) => @none

pub def uname() -> platform_info: ...   # struct-builds from the target/runtime queries above

pub def platform() -> str:
    let rel = match release():
        @ok(s) => s
        @err(_) => "unknown"
    let proc = match processor():
        @ok(s) => s
        @err(_) => "unknown"
    return "{target_os()}-{rel}-{target_arch().show()}-{proc}"
```

---

## Kira Runtime Information

```kira
pub pure def kira_build_info() -> kira_build:
    kira_build {
        version: kira_version(),
        implementation: kira_implementation(),
        compiler: kira_compiler(),
        build_date: kira_build_date(),
    }
```

(`kira_version`/`kira_implementation`/`kira_compiler`/`kira_build_date` are driver-generated — see above.)

---

## Platform-Specific Queries

`@none` on a platform other than the one they describe, or when the underlying intrinsic fails:

```kira
pub def libc_ver() -> option[libc_version]:
    if not is_unix():
        return @none
    match rt_libc_version():
        @ok(v) => @some(v)
        @err(_) => @none

pub def win32_ver() -> option[windows_version]: ...  # same shape, is_windows() + rt_windows_version()
pub def mac_ver() -> option[macos_version]: ...      # same shape, is_macos() + rt_macos_version()
```

---

## Usage Example

`use` does not currently import individual names from another module (see "Compiler Gaps Encountered"), so callers reference `std.platform` fully qualified — the same style `std.console` already uses for `std.io`:

```kira
module sample

use std.console

def main() -> unit:
    println("Compiled for {std.platform.target_arch().show()}-{std.platform.target_os()}")
    println("Pointer width: {std.platform.target_pointer_width()}")

    let info = std.platform.uname()
    println("Running on: {info.show()}")
    println("Platform string: {std.platform.platform()}")

    match std.platform.node():
        @ok(hostname) => println("Hostname: {hostname}")
        @err(_) => println("Could not get hostname")

    if let @some(libc) = std.platform.libc_ver():
        println("libc: {libc.name} {libc.version}")

    println("Kira version: {std.platform.kira_version()}")
    println("Kira build: {std.platform.kira_build_info().show()}")
```

Verified running end-to-end on macOS/aarch64 through both the bytecode VM (`kira sample.kira`) and the LLVM AOT path (`kira --compile -o sample_bin sample.kira && ./sample_bin`), producing real hostname, macOS release/version (via `sysctlbyname`), and CPU brand string.

---

## Summary of Design Decisions

| Decision | Rationale |
|---|---|
| Target/build info are driver-generated `pub pure def`s, not `pub static` constants | A bare reference to a non-scalar top-level `static` doesn't lower to bytecode/LLVM today; embedding the value as a function-body literal sidesteps the bug |
| Host info is `result[T, io_error]` | OS calls can genuinely fail; the type forces the caller to decide what to do |
| Error propagation uses `?`, not `.map`/`.map_err`/`.ok()` | Kira has no result/option combinator methods; `?` is the language's one propagation primitive, with implicit `from`-based error conversion; swallow-the-error conversions use `match` instead |
| All six intrinsics/raw structs are declared unconditionally, not gated by `static if` per platform | `intrinsic def` can't be nested in a `static if` body, and item-level `static if`/`else` doesn't splice declarations into module scope for other code to call |
| `release()`/`version()` are single unconditional functions matching the *runtime* `target_os_family()` | Same reason as above — compile-time per-platform dispatch of runtime-callable definitions doesn't work today |
| `target_os_family()` is driver-generated rather than classified from `target_os()`'s string at runtime | `str == str` has no runtime codegen in either backend yet |
| `rt_libc_version()` returns `libc_version` directly, not a raw string | Avoids needing `str.split()`, which also has no runtime codegen |
| Runtime OS-family checks use `match`, not `==` | The compiler does not generate `==` for user-defined sum types at runtime |
| `uname()` uses `option[str]` for runtime fields | `@none` (could not query) and `@some("")` (genuinely empty from OS) are distinguishable; no magic-value sentinel |
| `platform()` never fails | It is a formatting convenience; `"unknown"` is an acceptable fallback |
| Platform-specific functions return `option` | Calling `libc_ver()` on Windows is not an error — the information simply does not apply |
| `architecture` / `os_family` are sum types with `@other` | Makes invalid states unrepresentable for known platforms while remaining extensible; `show` is hand-written since `deriving` only generates real code for structs |
| Struct fields that would be named `machine` are `machine_arch` | `machine` is a reserved function-modifier keyword |
| Callers reference `std.platform` fully qualified | `use module.{a, b}` doesn't currently import names; full qualification is the proven-working style (`std.console`'s own `std.io.stdout_handle()`) |
