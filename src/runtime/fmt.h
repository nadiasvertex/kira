#pragma once

#include <cstdint>

/// C-ABI entry points `llvm_codegen`-compiled IR (JIT and AOT alike) calls
/// for the nine string-formatting intrinsics
/// (`spec/string-formatting-design.md`) — the LLVM-tier counterpart of
/// `src/bytecode/vm.cpp`'s `intrinsic_rt_str_*`/`intrinsic_rt_fmt_*`
/// dispatch table, following the same "one backend, one implementation"
/// split `src/runtime/io.h` documents for the eight I/O intrinsics.
///
/// `str` is still the usual 2-slot `{ len; data_ptr }` heap-pointer header
/// (`uint64_t *`), matching `src/runtime/io.h`'s convention — but every
/// scalar argument/return here crosses as a plain native value now
/// (`src/intrinsics.h`'s `intrinsic_wire_kind`), not boxed in a 1-slot
/// struct: this backend's intrinsic-declaration convention
/// (`llvm_codegen/codegen.cpp`) declares each parameter/return at its own
/// wire kind rather than hardcoding every one as `ptr`.
extern "C" {
auto cinder_rt_str_concat(uint64_t *a, uint64_t *b) -> uint64_t *;
auto cinder_rt_str_len_scalars(uint64_t *s) -> uint64_t;
auto cinder_rt_str_repeat_char(uint32_t codepoint, uint64_t count)
    -> uint64_t *;
auto cinder_rt_str_truncate_scalars(uint64_t *s, uint64_t count) -> uint64_t *;
auto cinder_rt_fmt_radix_digits(uint64_t value, uint32_t radix,
                                uint32_t uppercase) -> uint64_t *;
auto cinder_rt_fmt_f64_fixed(double value, uint64_t precision) -> uint64_t *;
auto cinder_rt_fmt_f64_sci(double value, uint64_t precision, uint32_t uppercase)
    -> uint64_t *;
auto cinder_rt_fmt_f64_general(double value, uint64_t precision) -> uint64_t *;
auto cinder_rt_fmt_char_from_codepoint(uint32_t codepoint) -> uint64_t *;
auto cinder_rt_bitcast_f64_to_u64(double value) -> uint64_t;
auto cinder_rt_bitcast_f32_to_u32(float value) -> uint32_t;

// The `_into` entry points: the `str` result is a `{len, data}` descriptor
// written into the caller's storage (`src/runtime/result_out.h`), and each
// `str` argument is the address of the caller's own descriptor.
void cinder_rt_str_concat_into(void *result, const void *a, const void *b);
void cinder_rt_str_repeat_char_into(void *result, uint32_t codepoint,
                                    uint64_t count);
void cinder_rt_str_truncate_scalars_into(void *result, const void *s,
                                         uint64_t count);
void cinder_rt_fmt_radix_digits_into(void *result, uint64_t value,
                                     uint32_t radix, uint32_t uppercase);
void cinder_rt_fmt_f64_fixed_into(void *result, double value,
                                  uint64_t precision);
void cinder_rt_fmt_f64_sci_into(void *result, double value, uint64_t precision,
                                uint32_t uppercase);
void cinder_rt_fmt_f64_general_into(void *result, double value,
                                    uint64_t precision);
void cinder_rt_fmt_char_from_codepoint_into(void *result, uint32_t codepoint);
}
