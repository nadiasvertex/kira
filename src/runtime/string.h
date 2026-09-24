#pragma once

#include <cstdint>

/// C-ABI entry points `llvm_codegen`-compiled IR (JIT and AOT alike) calls for
/// the `std.string` intrinsics (`spec/std-reference.md`) — the LLVM-tier
/// counterpart of `src/bytecode/vm.cpp`'s `intrinsic_rt_str_*` dispatch, the
/// same "one algorithm, two thin backends" split `src/runtime/fmt.h` uses for
/// the formatting intrinsics. The actual UTF-8 algorithms live once in
/// `src/runtime/string_ops.h`; these wrappers only marshal the heap ABI.
///
/// `str` is a 2-slot `{ len; data_ptr }` opaque heap pointer (see
/// `src/runtime/io.h`); a scalar argument/return crosses as a plain native
/// value (`src/intrinsics.h`'s `intrinsic_wire_kind`) — `rt_str_eq` returns
/// a native `uint32_t` (0/1, standing for `bool`); `rt_str_find`/
/// `rt_str_rfind` still return a 2-slot `find_result { found; pos }` struct,
/// which is not a bare scalar and so is still a heap pointer.
extern "C" {
auto kira_rt_str_eq(uint64_t *a, uint64_t *b) -> uint32_t;
auto kira_rt_str_cmp(uint64_t *a, uint64_t *b) -> int32_t;
auto kira_rt_str_find(uint64_t *haystack, uint64_t *needle, uint64_t from)
    -> uint64_t *;
auto kira_rt_str_rfind(uint64_t *haystack, uint64_t *needle) -> uint64_t *;
auto kira_rt_str_reverse(uint64_t *s) -> uint64_t *;
auto kira_rt_str_trim(uint64_t *s, uint32_t mode) -> uint64_t *;
auto kira_rt_str_replace(uint64_t *s, uint64_t *from, uint64_t *to)
    -> uint64_t *;
}
