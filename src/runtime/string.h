#pragma once

#include <cstdint>

/// C-ABI entry points `llvm_codegen`-compiled IR (JIT and AOT alike) calls for
/// the eight `std.string` intrinsics (`spec/std-string.md`) — the LLVM-tier
/// counterpart of `src/bytecode/vm.cpp`'s `intrinsic_rt_str_*` dispatch, the
/// same "one algorithm, two thin backends" split `src/runtime/fmt.h` uses for
/// the formatting intrinsics. The actual UTF-8 algorithms live once in
/// `src/runtime/string_ops.h`; these wrappers only marshal the heap ABI.
///
/// Every argument/return is an opaque heap pointer (see `src/runtime/io.h`):
/// `str` is the usual 2-slot `{ len; data_ptr }` header; scalar arguments are
/// boxed 1-slot structs (`box_usize`/`box_u8`, `src/std/string.kira`);
/// `rt_str_eq` returns a boxed `bool`; `rt_str_find`/`rt_str_rfind` return a
/// 2-slot `find_result { found; pos }` struct.
extern "C" {
auto kira_rt_str_eq(uint64_t *a, uint64_t *b) -> uint64_t *;
auto kira_rt_str_find(uint64_t *haystack, uint64_t *needle, uint64_t *from)
    -> uint64_t *;
auto kira_rt_str_rfind(uint64_t *haystack, uint64_t *needle) -> uint64_t *;
auto kira_rt_str_to_upper(uint64_t *s) -> uint64_t *;
auto kira_rt_str_to_lower(uint64_t *s) -> uint64_t *;
auto kira_rt_str_reverse(uint64_t *s) -> uint64_t *;
auto kira_rt_str_trim(uint64_t *s, uint64_t *mode) -> uint64_t *;
auto kira_rt_str_replace(uint64_t *s, uint64_t *from, uint64_t *to)
    -> uint64_t *;
}
