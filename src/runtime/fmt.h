#pragma once

#include <cstdint>

/// C-ABI entry points for the string-formatting intrinsics
/// (`spec/string-formatting-design.md`).
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
/// A scalar argument or result crosses as a plain native value
/// (`src/intrinsics.h`'s `intrinsic_wire_kind`); a `str` result is a
/// `{ len, data }` descriptor whose bytes the caller then owns.
extern "C" {
auto cinder_rt_str_len_scalars(uint64_t *s) -> uint64_t;
auto cinder_rt_bitcast_f64_to_u64(double value) -> uint64_t;
auto cinder_rt_bitcast_f32_to_u32(float value) -> uint32_t;

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
