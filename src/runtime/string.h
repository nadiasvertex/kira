#pragma once

#include <cstdint>

/// C-ABI entry points for the `std.string` intrinsics
/// (`spec/std-reference.md`). The UTF-8 algorithms live once in
/// `src/runtime/string_ops.h`; these wrappers only marshal the ABI.
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
/// `rt_str_eq` returns a native `uint32_t` (0 or 1, standing for `bool`);
/// `find_result` is `{ found: bool @0, pos: usize @8 }`.
extern "C" {
auto cinder_rt_str_eq(uint64_t *a, uint64_t *b) -> uint32_t;
auto cinder_rt_str_cmp(uint64_t *a, uint64_t *b) -> int32_t;

void cinder_rt_str_find_into(void *result, const void *haystack,
                             const void *needle, uint64_t from);
void cinder_rt_str_rfind_into(void *result, const void *haystack,
                              const void *needle);
void cinder_rt_str_reverse_into(void *result, const void *s);
void cinder_rt_str_trim_into(void *result, const void *s, uint32_t mode);
void cinder_rt_str_replace_into(void *result, const void *s, const void *from,
                                const void *to);
}
