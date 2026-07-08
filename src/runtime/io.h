#pragma once

#include <cstdint>

/// C-ABI entry points `llvm_codegen`-compiled IR (JIT and AOT alike) calls
/// for each of `src/intrinsics.h`'s eight intrinsics — the LLVM-tier
/// counterpart of `src/bytecode/vm.cpp`'s `intrinsic_rt_*` dispatch table.
/// Per spec/stdlib.md's design principle ("one backend, one implementation
/// of each intrinsic"), this is a *second*, independent native
/// implementation of the same eight operations, not a shared call into the
/// bytecode VM's — the two tiers never share a runtime call path (mirrors
/// why `kira_rt_alloc` (`arena.h`) is a thin AOT/JIT-only wrapper around
/// logic the VM instead calls directly).
///
/// Every argument and return value here is an opaque heap pointer
/// (`uint64_t *`, matching `src/runtime/layout.h`'s "flat N-slot block"
/// convention `llvm_codegen`'s `storage_llvm_type` already maps every heap
/// type to `ptr`) — `raw_fd { value: int64 }` and `io_errno { code: int32 }`
/// are each a 1-slot struct, `open_options` a 5-slot struct in declaration
/// order (`read`, `write`, `append`, `create`, `truncate`), `str`/
/// `slice[byte]`/`slice_mut[byte]` a 2-slot `{ len; data_ptr }` header, and
/// `result[T, io_errno]` a 2-slot `{ tag; payload }` block (`tag == 0` for
/// `@ok`, `tag == 1` for `@err`) — the same layout
/// `src/runtime/layout.cpp`'s `builtin_generic_variants_of` established for
/// this backend to share with the bytecode tier.
extern "C" {
auto kira_rt_stdin() -> uint64_t *;
auto kira_rt_stdout() -> uint64_t *;
auto kira_rt_stderr() -> uint64_t *;
auto kira_rt_open(uint64_t *path, uint64_t *opts) -> uint64_t *;
auto kira_rt_close(uint64_t *fd) -> uint64_t *;
auto kira_rt_read(uint64_t *fd, uint64_t *buf) -> uint64_t *;
auto kira_rt_write(uint64_t *fd, uint64_t *buf) -> uint64_t *;
auto kira_rt_flush(uint64_t *fd) -> uint64_t *;
}
