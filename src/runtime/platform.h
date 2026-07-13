#pragma once

#include <cstdint>

/// C-ABI entry points `llvm_codegen`-compiled IR (JIT and AOT alike) calls
/// for `std.platform`'s six runtime intrinsics (spec/std-platform.md) — the
/// LLVM-tier counterpart of `src/bytecode/vm.cpp`'s `intrinsic_rt_*`
/// dispatch table, mirroring `src/runtime/io.h`'s existing split between the
/// two backends. Each queries the host via `src/runtime/platform_query.h`
/// and encodes the result per `src/runtime/layout.h`'s flat-slot convention;
/// every argument and return value is an opaque heap pointer (`uint64_t *`).
///
/// All six are niladic and return `result[T, io_errno]` (a 2-slot
/// `{ tag; payload }` block, `tag == 0` for `@ok`). `rt_uname`,
/// `rt_libc_version`, `rt_windows_version`, and `rt_macos_version` are only
/// ever reachable from generated code on the host family they describe
/// (`std.platform` gates their declarations behind `static if
/// TARGET_OS_FAMILY == ...`) — on any other host they still need to exist
/// and link (this file backs the compiler's one fixed intrinsic table
/// regardless of build host) but simply report `io_errno { code: ENOSYS }`.
extern "C" {
auto kira_rt_uname() -> uint64_t *;
auto kira_rt_gethostname() -> uint64_t *;
auto kira_rt_processor_name() -> uint64_t *;
auto kira_rt_libc_version() -> uint64_t *;
auto kira_rt_windows_version() -> uint64_t *;
auto kira_rt_macos_version() -> uint64_t *;
}
