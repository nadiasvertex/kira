#pragma once

#include <cstdint>

/// C-ABI entry points `llvm_codegen`-compiled IR (JIT and AOT alike) calls
/// for the nine string-formatting intrinsics
/// (`spec/string-formatting-design.md`) — the LLVM-tier counterpart of
/// `src/bytecode/vm.cpp`'s `intrinsic_rt_str_*`/`intrinsic_rt_fmt_*`
/// dispatch table, following the same "one backend, one implementation"
/// split `src/runtime/io.h` documents for the eight I/O intrinsics.
///
/// Every argument/return here is an opaque heap pointer, matching
/// `src/runtime/io.h`'s convention: `str` is the usual 2-slot
/// `{ len; data_ptr }` header; every scalar argument (an integer, `bool`, or
/// `float64`) is boxed in a 1-slot struct (`box_u64`/`box_u32`/`box_u8`/
/// `box_usize`/`box_bool`/`box_f64`, `src/std/fmt.kira`) so it stays
/// heap-representable — this backend's intrinsic-declaration convention
/// (`llvm_codegen/codegen.cpp`) hardcodes every intrinsic parameter/return
/// as a pointer, so a bare scalar argument would not fit without boxing.
extern "C" {
auto kira_rt_str_concat(uint64_t *a, uint64_t *b) -> uint64_t *;
auto kira_rt_str_len_scalars(uint64_t *s) -> uint64_t *;
auto kira_rt_str_repeat_char(uint64_t *codepoint, uint64_t *count)
    -> uint64_t *;
auto kira_rt_str_truncate_scalars(uint64_t *s, uint64_t *count) -> uint64_t *;
auto kira_rt_fmt_radix_digits(uint64_t *value, uint64_t *radix,
                              uint64_t *uppercase) -> uint64_t *;
auto kira_rt_fmt_f64_fixed(uint64_t *value, uint64_t *precision) -> uint64_t *;
auto kira_rt_fmt_f64_sci(uint64_t *value, uint64_t *precision,
                         uint64_t *uppercase) -> uint64_t *;
auto kira_rt_fmt_f64_general(uint64_t *value, uint64_t *precision)
    -> uint64_t *;
auto kira_rt_fmt_char_from_codepoint(uint64_t *codepoint) -> uint64_t *;
}
