#pragma once

#include <cstdint>

/// C-ABI entry points for `std.platform`'s runtime intrinsics
/// (spec/std-reference.md). Each queries the host through
/// `src/runtime/platform_query.h`.
///
/// The LLVM tier links these. The bytecode VM (`src/bytecode/vm.cpp`) calls
/// the same `_into` functions, so the two tiers cannot build a descriptor or
/// aggregate result differently.
///
/// A descriptor or aggregate argument is the address of the caller's own
/// bytes in the layout of `src/runtime/value_layout.h`, borrowed for the
/// call. A function whose result is a descriptor or aggregate is an `_into`
/// entry point (`src/intrinsics.h`, `intrinsic_signature::into`): `result`
/// is the address of storage the caller owns, sized and aligned for the
/// declared result type, which the function fills completely (every byte,
/// so padding is zero). `src/runtime/result_out.h` lists every shape.
///
/// All six are niladic and build a `result[T, io_errno]`, each `str` member
/// an inline descriptor; `winver_raw` is four `uint32` at 0, 4, 8, 12 and a
/// `str` at 16. `rt_uname`, `rt_libc_version`, `rt_windows_version`, and
/// `rt_macos_version` are only reachable from generated code on the host
/// family they describe (`std.platform` gates their declarations behind
/// `static if TARGET_OS_FAMILY == ...`); on any other host they still exist
/// and link, and report `io_errno { code: ENOSYS }`.
extern "C" {
void cinder_rt_uname_into(void *result);
void cinder_rt_gethostname_into(void *result);
void cinder_rt_processor_name_into(void *result);
void cinder_rt_libc_version_into(void *result);
void cinder_rt_windows_version_into(void *result);
void cinder_rt_macos_version_into(void *result);
}
