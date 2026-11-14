#pragma once

#include <cstdint>

/// C-ABI entry points for the I/O intrinsics of `src/intrinsics.h`.
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
/// `raw_fd { value: int64 }` is 8 bytes, `open_options` five one-byte
/// `bool`s (`read`, `write`, `append`, `create`, `truncate`), a `str` or byte
/// slice a `{ len, data }` descriptor, and `result[T, io_errno]` a `u64` tag
/// (0 for `@ok`, 1 for `@err`) with the payload at offset 8.
extern "C" {
/// The host's `EAGAIN` code, which differs between Linux and macOS.
auto cinder_rt_errno_would_block() -> int32_t;

void cinder_rt_stdin_into(void *result);
void cinder_rt_stdout_into(void *result);
void cinder_rt_stderr_into(void *result);
void cinder_rt_open_into(void *result, const void *path, const void *opts);
void cinder_rt_close_into(void *result, const void *fd);
void cinder_rt_read_into(void *result, const void *fd, const void *buf);
void cinder_rt_write_into(void *result, const void *fd, const void *buf);
void cinder_rt_flush_into(void *result, const void *fd);
void cinder_rt_pipe_into(void *result);
void cinder_rt_set_nonblocking_into(void *result, const void *fd);
/// Writes `msg` (the address of a `str` descriptor) to stderr and ends the
/// process, or the task when one is running. The only intrinsic that does not
/// return; its `uint64_t *` result exists solely so it shares the uniform
/// intrinsic ABI the two backends emit calls under.
[[noreturn]] auto cinder_rt_panic(uint64_t *msg) -> uint64_t *;
}
