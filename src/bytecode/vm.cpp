#include "src/bytecode/vm.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "src/intrinsics.h"
#include "src/runtime/aligned_buffer.h"
#include "src/runtime/allocator.h"
#include "src/runtime/arena.h"
#include "src/runtime/coroutine.h"
#include "src/runtime/fmt.h"
#include "src/runtime/io.h"
#include "src/runtime/platform.h"
#include "src/runtime/platform_query.h"
#include "src/runtime/reactor.h"
#include "src/runtime/scheduler.h"
#include "src/runtime/string.h"
#include "src/runtime/string_ops.h"
#include "src/utf8/utf8.h"

namespace cinder::bytecode {

namespace {

// A generous ceiling for this tier's explicit call-frame stack (see vm.h's
// doc comment on why frames are an explicit std::vector rather than C++
// recursion) — crossing it panics with `stack_overflow` instead of
// exhausting host memory. Revisit once real programs (loops via recursion,
// once `while`/`for` compile) show whether this is too generous or too
// tight.
constexpr size_t k_max_call_depth = 4096;

// ---------------------------------------------------------------------------
// Bit-level helpers. Every scalar value lives in a full 8-byte slot_value
// regardless of its declared numeric_kind's width (i8 through i64 all use
// slot_value::i, etc. — see value.h); these helpers mask down to (or
// sign/zero-extend up from) the declared width so a narrower kind's opcode
// handlers never see stale bits left over from a wider previous use of the
// same register.
// ---------------------------------------------------------------------------

[[nodiscard]] auto mask_for_width(int bits) -> uint64_t {
  if (bits >= 64) {
    return ~uint64_t{0};
  }
  return (uint64_t{1} << bits) - 1;
}

[[nodiscard]] auto sign_extend(uint64_t raw, int bits) -> int64_t {
  if (bits >= 64) {
    return static_cast<int64_t>(raw);
  }
  const auto mask = mask_for_width(bits);
  raw &= mask;
  const auto sign_bit = uint64_t{1} << (bits - 1);
  if ((raw & sign_bit) != 0) {
    raw |= ~mask;
  }
  return static_cast<int64_t>(raw);
}

[[nodiscard]] auto signed_min(int bits) -> int64_t {
  return bits >= 64 ? INT64_MIN : -(int64_t{1} << (bits - 1));
}
[[nodiscard]] auto signed_max(int bits) -> int64_t {
  return bits >= 64 ? INT64_MAX : (int64_t{1} << (bits - 1)) - 1;
}
[[nodiscard]] auto unsigned_max(int bits) -> uint64_t {
  return mask_for_width(bits);
}

[[nodiscard]] auto load_raw(slot_value v, numeric_kind k) -> uint64_t {
  return v.u & mask_for_width(bit_width(k));
}
[[nodiscard]] auto load_signed(slot_value v, numeric_kind k) -> int64_t {
  return sign_extend(load_raw(v, k), bit_width(k));
}
[[nodiscard]] auto load_unsigned(slot_value v, numeric_kind k) -> uint64_t {
  return load_raw(v, k);
}
[[nodiscard]] auto load_f32(slot_value v) -> float {
  return std::bit_cast<float>(static_cast<uint32_t>(v.u));
}
[[nodiscard]] auto load_f64(slot_value v) -> double { return v.f; }

[[nodiscard]] auto store_signed(int64_t v) -> slot_value {
  return slot_value{v};
}
[[nodiscard]] auto store_unsigned(uint64_t v) -> slot_value {
  return slot_value{v};
}
[[nodiscard]] auto store_f32(float v) -> slot_value {
  return slot_value{static_cast<uint64_t>(std::bit_cast<uint32_t>(v))};
}
[[nodiscard]] auto store_f64(double v) -> slot_value { return slot_value{v}; }
[[nodiscard]] auto store_bool(bool b) -> slot_value {
  return store_unsigned(b ? 1U : 0U);
}

/// Stores a raw (already width-masked) bit pattern back into a register,
/// sign-extending it first if `k` is a signed kind — the shared tail end of
/// every wrapping/bitwise/cast op below, which all compute a raw bit
/// pattern and then need it reinterpreted per `k`'s signedness.
[[nodiscard]] auto store_by_kind(numeric_kind k, uint64_t raw_masked)
    -> slot_value {
  if (is_signed_integer(k)) {
    return store_signed(sign_extend(raw_masked, bit_width(k)));
  }
  return store_unsigned(raw_masked);
}

// ---------------------------------------------------------------------------
// Checked (panicking) arithmetic. All three width-dependent overflow checks
// funnel through the host's 64-bit __builtin_*_overflow first (this project
// targets Clang) and then, for kinds narrower than 64 bits, additionally
// range-check the 64-bit result against that kind's true range — operands
// are already sign/zero-extended to fit their declared width, so a 64-bit
// overflow can only occur when the declared width *is* 64 bits.
// ---------------------------------------------------------------------------

[[nodiscard]] auto checked_add_signed(int64_t a, int64_t b, int bits)
    -> int64_t {
  int64_t result = 0;
  bool overflow = __builtin_add_overflow(a, b, &result);
  if (!overflow && bits < 64) {
    overflow = result < signed_min(bits) || result > signed_max(bits);
  }
  if (overflow) {
    throw panic_error(panic_reason::integer_overflow);
  }
  return result;
}
[[nodiscard]] auto checked_sub_signed(int64_t a, int64_t b, int bits)
    -> int64_t {
  int64_t result = 0;
  bool overflow = __builtin_sub_overflow(a, b, &result);
  if (!overflow && bits < 64) {
    overflow = result < signed_min(bits) || result > signed_max(bits);
  }
  if (overflow) {
    throw panic_error(panic_reason::integer_overflow);
  }
  return result;
}
[[nodiscard]] auto checked_mul_signed(int64_t a, int64_t b, int bits)
    -> int64_t {
  int64_t result = 0;
  bool overflow = __builtin_mul_overflow(a, b, &result);
  if (!overflow && bits < 64) {
    overflow = result < signed_min(bits) || result > signed_max(bits);
  }
  if (overflow) {
    throw panic_error(panic_reason::integer_overflow);
  }
  return result;
}

[[nodiscard]] auto checked_add_unsigned(uint64_t a, uint64_t b, int bits)
    -> uint64_t {
  uint64_t result = 0;
  bool overflow = __builtin_add_overflow(a, b, &result);
  if (!overflow && bits < 64) {
    overflow = result > unsigned_max(bits);
  }
  if (overflow) {
    throw panic_error(panic_reason::integer_overflow);
  }
  return result;
}
[[nodiscard]] auto checked_sub_unsigned(uint64_t a, uint64_t b, int bits)
    -> uint64_t {
  uint64_t result = 0;
  bool overflow = __builtin_sub_overflow(a, b, &result);
  if (!overflow && bits < 64) {
    overflow = result > unsigned_max(bits);
  }
  if (overflow) {
    throw panic_error(panic_reason::integer_overflow);
  }
  return result;
}
[[nodiscard]] auto checked_mul_unsigned(uint64_t a, uint64_t b, int bits)
    -> uint64_t {
  uint64_t result = 0;
  bool overflow = __builtin_mul_overflow(a, b, &result);
  if (!overflow && bits < 64) {
    overflow = result > unsigned_max(bits);
  }
  if (overflow) {
    throw panic_error(panic_reason::integer_overflow);
  }
  return result;
}

[[nodiscard]] auto checked_div_signed(int64_t a, int64_t b, int bits)
    -> int64_t {
  if (b == 0) {
    throw panic_error(panic_reason::integer_divide_by_zero);
  }
  if (a == signed_min(bits) && b == -1) {
    throw panic_error(panic_reason::integer_overflow);
  }
  return a / b;
}
[[nodiscard]] auto checked_mod_signed(int64_t a, int64_t b, int bits)
    -> int64_t {
  if (b == 0) {
    throw panic_error(panic_reason::integer_divide_by_zero);
  }
  if (a == signed_min(bits) && b == -1) {
    throw panic_error(panic_reason::integer_overflow);
  }
  return a % b;
}
[[nodiscard]] auto checked_div_unsigned(uint64_t a, uint64_t b) -> uint64_t {
  if (b == 0) {
    throw panic_error(panic_reason::integer_divide_by_zero);
  }
  return a / b;
}
[[nodiscard]] auto checked_mod_unsigned(uint64_t a, uint64_t b) -> uint64_t {
  if (b == 0) {
    throw panic_error(panic_reason::integer_divide_by_zero);
  }
  return a % b;
}
[[nodiscard]] auto checked_neg_signed(int64_t a, int bits) -> int64_t {
  if (a == signed_min(bits)) {
    throw panic_error(panic_reason::integer_overflow);
  }
  return -a;
}

// ---------------------------------------------------------------------------
// Wrapping (`+%`/`-%`/`*%`) arithmetic — plain modulo-2^bits arithmetic on
// the raw bit pattern, indifferent to signedness (two's complement wrap is
// identical either way); `store_by_kind` reinterprets the wrapped bits per
// the destination kind's signedness afterward.
// ---------------------------------------------------------------------------

[[nodiscard]] auto wrap_add(uint64_t a, uint64_t b, int bits) -> uint64_t {
  return (a + b) & mask_for_width(bits);
}
[[nodiscard]] auto wrap_sub(uint64_t a, uint64_t b, int bits) -> uint64_t {
  return (a - b) & mask_for_width(bits);
}
[[nodiscard]] auto wrap_mul(uint64_t a, uint64_t b, int bits) -> uint64_t {
  return (a * b) & mask_for_width(bits);
}

// ---------------------------------------------------------------------------
// Saturating (`+|`/`-|`/`*|`) arithmetic.
// ---------------------------------------------------------------------------

[[nodiscard]] auto sat_add_signed(int64_t a, int64_t b, int bits) -> int64_t {
  int64_t result = 0;
  const auto min_v = signed_min(bits);
  const auto max_v = signed_max(bits);
  if (__builtin_add_overflow(a, b, &result)) {
    return a >= 0 ? max_v : min_v;
  }
  if (result < min_v) {
    return min_v;
  }
  if (result > max_v) {
    return max_v;
  }
  return result;
}
[[nodiscard]] auto sat_sub_signed(int64_t a, int64_t b, int bits) -> int64_t {
  int64_t result = 0;
  const auto min_v = signed_min(bits);
  const auto max_v = signed_max(bits);
  if (__builtin_sub_overflow(a, b, &result)) {
    return a >= 0 ? max_v : min_v;
  }
  if (result < min_v) {
    return min_v;
  }
  if (result > max_v) {
    return max_v;
  }
  return result;
}
[[nodiscard]] auto sat_mul_signed(int64_t a, int64_t b, int bits) -> int64_t {
  int64_t result = 0;
  const auto min_v = signed_min(bits);
  const auto max_v = signed_max(bits);
  if (__builtin_mul_overflow(a, b, &result)) {
    const bool same_sign = (a >= 0) == (b >= 0);
    return same_sign ? max_v : min_v;
  }
  if (result < min_v) {
    return min_v;
  }
  if (result > max_v) {
    return max_v;
  }
  return result;
}
[[nodiscard]] auto sat_add_unsigned(uint64_t a, uint64_t b, int bits)
    -> uint64_t {
  uint64_t result = 0;
  const auto max_v = unsigned_max(bits);
  if (__builtin_add_overflow(a, b, &result)) {
    return max_v;
  }
  return result > max_v ? max_v : result;
}
[[nodiscard]] auto sat_sub_unsigned(uint64_t a, uint64_t b, int /*bits*/)
    -> uint64_t {
  uint64_t result = 0;
  if (__builtin_sub_overflow(a, b, &result)) {
    return 0;
  }
  return result;
}
[[nodiscard]] auto sat_mul_unsigned(uint64_t a, uint64_t b, int bits)
    -> uint64_t {
  uint64_t result = 0;
  const auto max_v = unsigned_max(bits);
  if (__builtin_mul_overflow(a, b, &result)) {
    return max_v;
  }
  return result > max_v ? max_v : result;
}

// ---------------------------------------------------------------------------
// Per-opcode-family dispatch over numeric_kind.
// ---------------------------------------------------------------------------

[[nodiscard]] auto exec_add(numeric_kind k, slot_value lhs, slot_value rhs)
    -> slot_value {
  const auto bits = bit_width(k);
  if (is_signed_integer(k)) {
    return store_signed(
        checked_add_signed(load_signed(lhs, k), load_signed(rhs, k), bits));
  }
  if (is_unsigned_integer(k)) {
    return store_unsigned(checked_add_unsigned(load_unsigned(lhs, k),
                                               load_unsigned(rhs, k), bits));
  }
  if (k == numeric_kind::f32) {
    return store_f32(load_f32(lhs) + load_f32(rhs));
  }
  return store_f64(load_f64(lhs) + load_f64(rhs));
}
[[nodiscard]] auto exec_sub(numeric_kind k, slot_value lhs, slot_value rhs)
    -> slot_value {
  const auto bits = bit_width(k);
  if (is_signed_integer(k)) {
    return store_signed(
        checked_sub_signed(load_signed(lhs, k), load_signed(rhs, k), bits));
  }
  if (is_unsigned_integer(k)) {
    return store_unsigned(checked_sub_unsigned(load_unsigned(lhs, k),
                                               load_unsigned(rhs, k), bits));
  }
  if (k == numeric_kind::f32) {
    return store_f32(load_f32(lhs) - load_f32(rhs));
  }
  return store_f64(load_f64(lhs) - load_f64(rhs));
}
[[nodiscard]] auto exec_mul(numeric_kind k, slot_value lhs, slot_value rhs)
    -> slot_value {
  const auto bits = bit_width(k);
  if (is_signed_integer(k)) {
    return store_signed(
        checked_mul_signed(load_signed(lhs, k), load_signed(rhs, k), bits));
  }
  if (is_unsigned_integer(k)) {
    return store_unsigned(checked_mul_unsigned(load_unsigned(lhs, k),
                                               load_unsigned(rhs, k), bits));
  }
  if (k == numeric_kind::f32) {
    return store_f32(load_f32(lhs) * load_f32(rhs));
  }
  return store_f64(load_f64(lhs) * load_f64(rhs));
}
[[nodiscard]] auto exec_div(numeric_kind k, slot_value lhs, slot_value rhs)
    -> slot_value {
  const auto bits = bit_width(k);
  if (is_signed_integer(k)) {
    return store_signed(
        checked_div_signed(load_signed(lhs, k), load_signed(rhs, k), bits));
  }
  if (is_unsigned_integer(k)) {
    return store_unsigned(
        checked_div_unsigned(load_unsigned(lhs, k), load_unsigned(rhs, k)));
  }
  if (k == numeric_kind::f32) {
    return store_f32(load_f32(lhs) / load_f32(rhs));
  }
  return store_f64(load_f64(lhs) / load_f64(rhs));
}
[[nodiscard]] auto exec_mod(numeric_kind k, slot_value lhs, slot_value rhs)
    -> slot_value {
  const auto bits = bit_width(k);
  if (is_signed_integer(k)) {
    return store_signed(
        checked_mod_signed(load_signed(lhs, k), load_signed(rhs, k), bits));
  }
  return store_unsigned(
      checked_mod_unsigned(load_unsigned(lhs, k), load_unsigned(rhs, k)));
}
[[nodiscard]] auto exec_neg(numeric_kind k, slot_value src) -> slot_value {
  // Precondition (enforced by the type checker upstream, not re-checked
  // here — see CLAUDE.md's "trust internal guarantees"): unary `-` is only
  // ever applied to signed or floating-point operands.
  if (k == numeric_kind::f32) {
    return store_f32(-load_f32(src));
  }
  if (k == numeric_kind::f64) {
    return store_f64(-load_f64(src));
  }
  return store_signed(checked_neg_signed(load_signed(src, k), bit_width(k)));
}

[[nodiscard]] auto exec_add_wrap(numeric_kind k, slot_value lhs, slot_value rhs)
    -> slot_value {
  const auto bits = bit_width(k);
  return store_by_kind(k, wrap_add(load_raw(lhs, k), load_raw(rhs, k), bits));
}
[[nodiscard]] auto exec_sub_wrap(numeric_kind k, slot_value lhs, slot_value rhs)
    -> slot_value {
  const auto bits = bit_width(k);
  return store_by_kind(k, wrap_sub(load_raw(lhs, k), load_raw(rhs, k), bits));
}
[[nodiscard]] auto exec_mul_wrap(numeric_kind k, slot_value lhs, slot_value rhs)
    -> slot_value {
  const auto bits = bit_width(k);
  return store_by_kind(k, wrap_mul(load_raw(lhs, k), load_raw(rhs, k), bits));
}

[[nodiscard]] auto exec_add_sat(numeric_kind k, slot_value lhs, slot_value rhs)
    -> slot_value {
  const auto bits = bit_width(k);
  if (is_signed_integer(k)) {
    return store_signed(
        sat_add_signed(load_signed(lhs, k), load_signed(rhs, k), bits));
  }
  return store_unsigned(
      sat_add_unsigned(load_unsigned(lhs, k), load_unsigned(rhs, k), bits));
}
[[nodiscard]] auto exec_sub_sat(numeric_kind k, slot_value lhs, slot_value rhs)
    -> slot_value {
  const auto bits = bit_width(k);
  if (is_signed_integer(k)) {
    return store_signed(
        sat_sub_signed(load_signed(lhs, k), load_signed(rhs, k), bits));
  }
  return store_unsigned(
      sat_sub_unsigned(load_unsigned(lhs, k), load_unsigned(rhs, k), bits));
}
[[nodiscard]] auto exec_mul_sat(numeric_kind k, slot_value lhs, slot_value rhs)
    -> slot_value {
  const auto bits = bit_width(k);
  if (is_signed_integer(k)) {
    return store_signed(
        sat_mul_signed(load_signed(lhs, k), load_signed(rhs, k), bits));
  }
  return store_unsigned(
      sat_mul_unsigned(load_unsigned(lhs, k), load_unsigned(rhs, k), bits));
}

[[nodiscard]] auto exec_bitand(numeric_kind k, slot_value lhs, slot_value rhs)
    -> slot_value {
  return store_by_kind(k, load_raw(lhs, k) & load_raw(rhs, k));
}
[[nodiscard]] auto exec_bitor(numeric_kind k, slot_value lhs, slot_value rhs)
    -> slot_value {
  return store_by_kind(k, load_raw(lhs, k) | load_raw(rhs, k));
}
[[nodiscard]] auto exec_bitxor(numeric_kind k, slot_value lhs, slot_value rhs)
    -> slot_value {
  return store_by_kind(k, load_raw(lhs, k) ^ load_raw(rhs, k));
}
[[nodiscard]] auto exec_bitnot(numeric_kind k, slot_value src) -> slot_value {
  const auto bits = bit_width(k);
  return store_by_kind(k, (~load_raw(src, k)) & mask_for_width(bits));
}

[[nodiscard]] auto exec_shl(numeric_kind k, slot_value lhs, slot_value rhs)
    -> slot_value {
  const auto bits = bit_width(k);
  const auto shift = load_unsigned(rhs, k);
  if (std::cmp_greater_equal(shift, bits)) {
    throw panic_error(panic_reason::integer_overflow);
  }
  const auto raw = load_raw(lhs, k);
  return store_by_kind(k, (raw << shift) & mask_for_width(bits));
}
[[nodiscard]] auto exec_shr(numeric_kind k, slot_value lhs, slot_value rhs)
    -> slot_value {
  const auto bits = bit_width(k);
  const auto shift = load_unsigned(rhs, k);
  if (std::cmp_greater_equal(shift, bits)) {
    throw panic_error(panic_reason::integer_overflow);
  }
  if (is_signed_integer(k)) {
    // Arithmetic shift: C++20 mandates two's complement and a
    // sign-preserving right shift for negative signed operands, so this
    // needs no manual sign-fill.
    return store_signed(load_signed(lhs, k) >> shift);
  }
  return store_unsigned(load_raw(lhs, k) >> shift);
}

[[nodiscard]] auto exec_eq(numeric_kind k, slot_value lhs, slot_value rhs)
    -> slot_value {
  if (k == numeric_kind::f32) {
    return store_bool(load_f32(lhs) == load_f32(rhs));
  }
  if (k == numeric_kind::f64) {
    return store_bool(load_f64(lhs) == load_f64(rhs));
  }
  return store_bool(load_raw(lhs, k) == load_raw(rhs, k));
}
[[nodiscard]] auto exec_ne(numeric_kind k, slot_value lhs, slot_value rhs)
    -> slot_value {
  if (k == numeric_kind::f32) {
    return store_bool(load_f32(lhs) != load_f32(rhs));
  }
  if (k == numeric_kind::f64) {
    return store_bool(load_f64(lhs) != load_f64(rhs));
  }
  return store_bool(load_raw(lhs, k) != load_raw(rhs, k));
}
[[nodiscard]] auto exec_lt(numeric_kind k, slot_value lhs, slot_value rhs)
    -> slot_value {
  if (k == numeric_kind::f32) {
    return store_bool(load_f32(lhs) < load_f32(rhs));
  }
  if (k == numeric_kind::f64) {
    return store_bool(load_f64(lhs) < load_f64(rhs));
  }
  if (is_signed_integer(k)) {
    return store_bool(load_signed(lhs, k) < load_signed(rhs, k));
  }
  return store_bool(load_raw(lhs, k) < load_raw(rhs, k));
}
[[nodiscard]] auto exec_le(numeric_kind k, slot_value lhs, slot_value rhs)
    -> slot_value {
  if (k == numeric_kind::f32) {
    return store_bool(load_f32(lhs) <= load_f32(rhs));
  }
  if (k == numeric_kind::f64) {
    return store_bool(load_f64(lhs) <= load_f64(rhs));
  }
  if (is_signed_integer(k)) {
    return store_bool(load_signed(lhs, k) <= load_signed(rhs, k));
  }
  return store_bool(load_raw(lhs, k) <= load_raw(rhs, k));
}
[[nodiscard]] auto exec_gt(numeric_kind k, slot_value lhs, slot_value rhs)
    -> slot_value {
  if (k == numeric_kind::f32) {
    return store_bool(load_f32(lhs) > load_f32(rhs));
  }
  if (k == numeric_kind::f64) {
    return store_bool(load_f64(lhs) > load_f64(rhs));
  }
  if (is_signed_integer(k)) {
    return store_bool(load_signed(lhs, k) > load_signed(rhs, k));
  }
  return store_bool(load_raw(lhs, k) > load_raw(rhs, k));
}
[[nodiscard]] auto exec_ge(numeric_kind k, slot_value lhs, slot_value rhs)
    -> slot_value {
  if (k == numeric_kind::f32) {
    return store_bool(load_f32(lhs) >= load_f32(rhs));
  }
  if (k == numeric_kind::f64) {
    return store_bool(load_f64(lhs) >= load_f64(rhs));
  }
  if (is_signed_integer(k)) {
    return store_bool(load_signed(lhs, k) >= load_signed(rhs, k));
  }
  return store_bool(load_raw(lhs, k) >= load_raw(rhs, k));
}

// ---------------------------------------------------------------------------
// 128-bit integers (`int128`/`uint128`). These use the whole 16-byte
// register (`slot_value::w`) and Clang's `__int128`, so unlike the narrower
// kinds above there is no masking or range re-check: the host arithmetic is
// already exactly 128 bits wide. Same panics, same saturation, same shift
// rules as every other integer kind.
// ---------------------------------------------------------------------------

using wide_signed = __int128;
using wide_unsigned = unsigned __int128;

constexpr auto k_wide_unsigned_max = ~wide_unsigned{0};
constexpr auto k_wide_signed_max =
    static_cast<wide_signed>(k_wide_unsigned_max >> 1);
constexpr auto k_wide_signed_min = -k_wide_signed_max - 1;

[[nodiscard]] auto load_wide_signed(slot_value v) -> wide_signed {
  return static_cast<wide_signed>(v.w);
}

template <typename T> [[nodiscard]] auto wide_store(T v) -> slot_value {
  return slot_value{static_cast<wide_unsigned>(v)};
}

template <typename T>
[[nodiscard]] auto wide_checked(bool overflow, T result) -> slot_value {
  if (overflow) {
    throw panic_error(panic_reason::integer_overflow);
  }
  return wide_store(result);
}

[[nodiscard]] auto exec_wide_signed_binary(opcode op, wide_signed a,
                                           wide_signed b) -> slot_value {
  wide_signed r = 0;
  switch (op) {
  case opcode::op_add:
    return wide_checked(__builtin_add_overflow(a, b, &r), r);
  case opcode::op_sub:
    return wide_checked(__builtin_sub_overflow(a, b, &r), r);
  case opcode::op_mul:
    return wide_checked(__builtin_mul_overflow(a, b, &r), r);
  case opcode::op_div:
  case opcode::op_mod:
    if (b == 0) {
      throw panic_error(panic_reason::integer_divide_by_zero);
    }
    if (a == k_wide_signed_min && b == -1) {
      throw panic_error(panic_reason::integer_overflow);
    }
    return wide_store(op == opcode::op_div ? a / b : a % b);
  case opcode::op_add_sat:
    if (__builtin_add_overflow(a, b, &r)) {
      return wide_store(a >= 0 ? k_wide_signed_max : k_wide_signed_min);
    }
    return wide_store(r);
  case opcode::op_sub_sat:
    if (__builtin_sub_overflow(a, b, &r)) {
      return wide_store(a >= 0 ? k_wide_signed_max : k_wide_signed_min);
    }
    return wide_store(r);
  case opcode::op_mul_sat:
    if (__builtin_mul_overflow(a, b, &r)) {
      return wide_store((a >= 0) == (b >= 0) ? k_wide_signed_max
                                             : k_wide_signed_min);
    }
    return wide_store(r);
  case opcode::op_shr: {
    const auto shift = static_cast<wide_unsigned>(b);
    if (shift >= 128) {
      throw panic_error(panic_reason::integer_overflow);
    }
    return wide_store(a >> static_cast<int>(shift));
  }
  case opcode::op_lt:
    return store_bool(a < b);
  case opcode::op_le:
    return store_bool(a <= b);
  case opcode::op_gt:
    return store_bool(a > b);
  case opcode::op_ge:
    return store_bool(a >= b);
  default:
    std::unreachable();
  }
}

[[nodiscard]] auto exec_wide_unsigned_binary(opcode op, wide_unsigned a,
                                             wide_unsigned b) -> slot_value {
  wide_unsigned r = 0;
  switch (op) {
  case opcode::op_add:
    return wide_checked(__builtin_add_overflow(a, b, &r), r);
  case opcode::op_sub:
    return wide_checked(__builtin_sub_overflow(a, b, &r), r);
  case opcode::op_mul:
    return wide_checked(__builtin_mul_overflow(a, b, &r), r);
  case opcode::op_div:
  case opcode::op_mod:
    if (b == 0) {
      throw panic_error(panic_reason::integer_divide_by_zero);
    }
    return wide_store(op == opcode::op_div ? a / b : a % b);
  case opcode::op_add_sat:
    return wide_store(__builtin_add_overflow(a, b, &r) ? k_wide_unsigned_max
                                                       : r);
  case opcode::op_sub_sat:
    return wide_store(__builtin_sub_overflow(a, b, &r) ? wide_unsigned{0} : r);
  case opcode::op_mul_sat:
    return wide_store(__builtin_mul_overflow(a, b, &r) ? k_wide_unsigned_max
                                                       : r);
  case opcode::op_shr:
    if (b >= 128) {
      throw panic_error(panic_reason::integer_overflow);
    }
    return wide_store(a >> static_cast<int>(b));
  case opcode::op_lt:
    return store_bool(a < b);
  case opcode::op_le:
    return store_bool(a <= b);
  case opcode::op_gt:
    return store_bool(a > b);
  case opcode::op_ge:
    return store_bool(a >= b);
  default:
    std::unreachable();
  }
}

/// Every binary opcode on an `int128`/`uint128` pair. The operations whose
/// result is the same bit pattern either way (wrapping arithmetic, bitwise
/// operations, `<<`, equality) are computed once on the unsigned bits.
[[nodiscard]] auto exec_wide_binary(opcode op, numeric_kind k, slot_value lhs,
                                    slot_value rhs) -> slot_value {
  const auto a = lhs.w;
  const auto b = rhs.w;
  switch (op) {
  case opcode::op_add_wrap:
    return wide_store(a + b);
  case opcode::op_sub_wrap:
    return wide_store(a - b);
  case opcode::op_mul_wrap:
    return wide_store(a * b);
  case opcode::op_bitand:
    return wide_store(a & b);
  case opcode::op_bitor:
    return wide_store(a | b);
  case opcode::op_bitxor:
    return wide_store(a ^ b);
  case opcode::op_shl: {
    // A negative signed shift amount reads as a huge unsigned one and
    // panics, the same as the narrower kinds.
    if (b >= 128) {
      throw panic_error(panic_reason::integer_overflow);
    }
    return wide_store(a << static_cast<int>(b));
  }
  case opcode::op_eq:
    return store_bool(a == b);
  case opcode::op_ne:
    return store_bool(a != b);
  default:
    break;
  }
  if (k == numeric_kind::i128) {
    return exec_wide_signed_binary(op, load_wide_signed(lhs),
                                   load_wide_signed(rhs));
  }
  return exec_wide_unsigned_binary(op, a, b);
}

[[nodiscard]] auto exec_wide_unary(opcode op, numeric_kind k, slot_value src)
    -> slot_value {
  if (op == opcode::op_bitnot) {
    return wide_store(~src.w);
  }
  // `-` only reaches here for `int128` (the checker rejects it on unsigned
  // operands).
  (void)k;
  const auto a = load_wide_signed(src);
  if (a == k_wide_signed_min) {
    throw panic_error(panic_reason::integer_overflow);
  }
  return wide_store(-a);
}

/// The source of an `as` conversion as a 128-bit signed or unsigned integer,
/// sign- or zero-extended from its own kind — the common first step of
/// every integer conversion that has a 128-bit end.
[[nodiscard]] auto widen_integer(numeric_kind k, slot_value v)
    -> wide_unsigned {
  if (is_wide_integer(k)) {
    return v.w;
  }
  if (is_signed_integer(k)) {
    return static_cast<wide_unsigned>(
        static_cast<wide_signed>(load_signed(v, k)));
  }
  return wide_unsigned{load_unsigned(v, k)};
}

/// `as` where either end is `int128`/`uint128`.
[[nodiscard]] auto exec_wide_cast(numeric_kind from_kind, numeric_kind to_kind,
                                  slot_value src) -> slot_value {
  if (is_float(from_kind)) {
    const double value = from_kind == numeric_kind::f32
                             ? static_cast<double>(load_f32(src))
                             : load_f64(src);
    if (to_kind == numeric_kind::i128) {
      return wide_store(static_cast<wide_signed>(value));
    }
    return wide_store(static_cast<wide_unsigned>(value));
  }
  if (is_float(to_kind)) {
    const double value = from_kind == numeric_kind::i128
                             ? static_cast<double>(load_wide_signed(src))
                             : static_cast<double>(src.w);
    if (to_kind == numeric_kind::f32) {
      return store_f32(static_cast<float>(value));
    }
    return store_f64(value);
  }
  const auto bits = widen_integer(from_kind, src);
  if (is_wide_integer(to_kind)) {
    return wide_store(bits);
  }
  const auto truncated =
      static_cast<uint64_t>(bits) & mask_for_width(bit_width(to_kind));
  return store_by_kind(to_kind, truncated);
}

[[nodiscard]] auto exec_cast(numeric_kind from_kind, numeric_kind to_kind,
                             slot_value src) -> slot_value {
  if (is_wide_integer(from_kind) || is_wide_integer(to_kind)) {
    return exec_wide_cast(from_kind, to_kind, src);
  }
  const bool from_float = is_float(from_kind);
  const bool to_float = is_float(to_kind);

  if (from_float && to_float) {
    const double value = (from_kind == numeric_kind::f32)
                             ? static_cast<double>(load_f32(src))
                             : load_f64(src);
    if (to_kind == numeric_kind::f32) {
      return store_f32(static_cast<float>(value));
    }
    return store_f64(value);
  }

  if (from_float) {
    const double value = (from_kind == numeric_kind::f32)
                             ? static_cast<double>(load_f32(src))
                             : load_f64(src);
    if (is_signed_integer(to_kind)) {
      return store_signed(
          sign_extend(static_cast<uint64_t>(static_cast<int64_t>(value)),
                      bit_width(to_kind)));
    }
    return store_unsigned(static_cast<uint64_t>(value) &
                          mask_for_width(bit_width(to_kind)));
  }

  if (to_float) {
    const double value =
        is_signed_integer(from_kind)
            ? static_cast<double>(load_signed(src, from_kind))
            : static_cast<double>(load_unsigned(src, from_kind));
    if (to_kind == numeric_kind::f32) {
      return store_f32(static_cast<float>(value));
    }
    return store_f64(value);
  }

  // Integer-to-integer: truncate-or-extend, per opcodes.h's doc comment on
  // op_cast — sign-extend if the *source* was signed, zero-extend
  // otherwise, then truncate to the destination width.
  const uint64_t src_bits =
      is_signed_integer(from_kind)
          ? static_cast<uint64_t>(load_signed(src, from_kind))
          : load_unsigned(src, from_kind);
  const uint64_t truncated = src_bits & mask_for_width(bit_width(to_kind));
  return store_by_kind(to_kind, truncated);
}

[[nodiscard]] auto dispatch_binary(opcode op, numeric_kind k, slot_value lhs,
                                   slot_value rhs) -> slot_value {
  if (is_wide_integer(k)) {
    return exec_wide_binary(op, k, lhs, rhs);
  }
  switch (op) {
  case opcode::op_add:
    return exec_add(k, lhs, rhs);
  case opcode::op_sub:
    return exec_sub(k, lhs, rhs);
  case opcode::op_mul:
    return exec_mul(k, lhs, rhs);
  case opcode::op_div:
    return exec_div(k, lhs, rhs);
  case opcode::op_mod:
    return exec_mod(k, lhs, rhs);
  case opcode::op_add_wrap:
    return exec_add_wrap(k, lhs, rhs);
  case opcode::op_sub_wrap:
    return exec_sub_wrap(k, lhs, rhs);
  case opcode::op_mul_wrap:
    return exec_mul_wrap(k, lhs, rhs);
  case opcode::op_add_sat:
    return exec_add_sat(k, lhs, rhs);
  case opcode::op_sub_sat:
    return exec_sub_sat(k, lhs, rhs);
  case opcode::op_mul_sat:
    return exec_mul_sat(k, lhs, rhs);
  case opcode::op_bitand:
    return exec_bitand(k, lhs, rhs);
  case opcode::op_bitor:
    return exec_bitor(k, lhs, rhs);
  case opcode::op_bitxor:
    return exec_bitxor(k, lhs, rhs);
  case opcode::op_shl:
    return exec_shl(k, lhs, rhs);
  case opcode::op_shr:
    return exec_shr(k, lhs, rhs);
  case opcode::op_eq:
    return exec_eq(k, lhs, rhs);
  case opcode::op_ne:
    return exec_ne(k, lhs, rhs);
  case opcode::op_lt:
    return exec_lt(k, lhs, rhs);
  case opcode::op_le:
    return exec_le(k, lhs, rhs);
  case opcode::op_gt:
    return exec_gt(k, lhs, rhs);
  case opcode::op_ge:
    return exec_ge(k, lhs, rhs);
  default:
    std::unreachable();
  }
}

[[nodiscard]] auto dispatch_unary(opcode op, numeric_kind k, slot_value src)
    -> slot_value {
  if (is_wide_integer(k)) {
    return exec_wide_unary(op, k, src);
  }
  switch (op) {
  case opcode::op_neg:
    return exec_neg(k, src);
  case opcode::op_bitnot:
    return exec_bitnot(k, src);
  default:
    std::unreachable();
  }
}

// ---------------------------------------------------------------------------
// Memory access (op_load_slot/op_store_slot). A slot_value's u64 field
// doubles as an address here, the same untagged-reinterpretation approach
// numeric_kind already relies on.
// ---------------------------------------------------------------------------

[[nodiscard]] auto slots_of(slot_value ptr) -> heap_word * {
  return reinterpret_cast<heap_word *>(static_cast<uintptr_t>(ptr.u));
}
[[nodiscard]] auto ptr_to_slot(void *raw) -> slot_value {
  return slot_value{static_cast<uint64_t>(reinterpret_cast<uintptr_t>(raw))};
}
[[nodiscard]] auto raw_bytes_of(slot_value ptr) -> uint8_t * {
  return reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(ptr.u));
}

/// Zero-extended `size`-byte (1/2/4/8/16) read at `data`, the width-aware
/// sibling of the old "always a whole slot" load — `size` is always one of
/// these values (`runtime::layout_info::size_bytes` for a scalar, 16 for an
/// `int128`/`uint128`, or the fixed 8 every uniform-slot construct still
/// passes). Uses `std::memcpy` rather than a typed `reinterpret_cast` load
/// since a packed struct's byte offset is not generally aligned to `size`.
[[nodiscard]] auto load_sized(const uint8_t *data, uint8_t size) -> slot_value {
  switch (size) {
  case 1:
    return slot_value{uint64_t{*data}};
  case 2: {
    uint16_t v = 0;
    std::memcpy(&v, data, sizeof(v));
    return slot_value{uint64_t{v}};
  }
  case 4: {
    uint32_t v = 0;
    std::memcpy(&v, data, sizeof(v));
    return slot_value{uint64_t{v}};
  }
  case 16: {
    unsigned __int128 v = 0;
    std::memcpy(&v, data, sizeof(v));
    return slot_value{v};
  }
  default: {
    uint64_t v = 0;
    std::memcpy(&v, data, sizeof(v));
    return slot_value{v};
  }
  }
}

/// Writes the low `size` bytes (1/2/4/8/16) of `value` at `data` — the
/// width-aware sibling of the old "always a whole slot" store.
auto store_sized(uint8_t *data, uint8_t size, slot_value value) -> void {
  switch (size) {
  case 1:
    *data = static_cast<uint8_t>(value.u);
    return;
  case 2: {
    const auto v = static_cast<uint16_t>(value.u);
    std::memcpy(data, &v, sizeof(v));
    return;
  }
  case 4: {
    const auto v = static_cast<uint32_t>(value.u);
    std::memcpy(data, &v, sizeof(v));
    return;
  }
  case 16:
    std::memcpy(data, &value.w, sizeof(value.w));
    return;
  default:
    std::memcpy(data, &value.u, sizeof(value.u));
    return;
  }
}

// ---------------------------------------------------------------------------
// Call frames — see vm.h's doc comment for why this is an explicit
// std::vector<frame> rather than C++ call-stack recursion.
// ---------------------------------------------------------------------------

/// Reads one instruction's operands in order, advancing as it goes.
///
/// The dispatch loop below used to index operands by hand — `code[ip + 2]`
/// for the third operand, then `f.pc = ip + 5` to step over the whole
/// instruction. That works, but it encodes every opcode's operand layout as
/// arithmetic scattered across ~52 `case` bodies, and it made the register
/// operand's width a fact restated at every one of them. Reading
/// sequentially states the layout once per opcode, in the order opcodes.h
/// documents it, and makes `pos()` the instruction's end by construction
/// rather than by a hand-computed constant that has to be kept in sync.
///
/// Callers must read each operand as its own statement rather than as
/// arguments to one call: the order of evaluation of function arguments is
/// unspecified, and these reads are order-dependent.
struct operand_cursor {
  const std::vector<uint8_t> &code;
  size_t at;

  [[nodiscard]] auto reg() -> uint16_t {
    static_assert(k_register_operand_bytes == 2,
                  "a register operand is decoded here as one little-endian "
                  "u16, matching function_compiler::emit_register");
    const auto value = read_u16(code, at);
    at += k_register_operand_bytes;
    return value;
  }
  [[nodiscard]] auto imm8() -> uint8_t { return code[at++]; }
  [[nodiscard]] auto imm16() -> uint16_t {
    const auto value = read_u16(code, at);
    at += 2;
    return value;
  }
  [[nodiscard]] auto imm32() -> uint32_t {
    const auto value = read_u32(code, at);
    at += 4;
    return value;
  }
  [[nodiscard]] auto imm64() -> uint64_t {
    const auto value = read_u64(code, at);
    at += 8;
    return value;
  }
  [[nodiscard]] auto rel32() -> int32_t {
    const auto value = read_i32(code, at);
    at += 4;
    return value;
  }
  /// The offset just past the operands read so far — i.e. the start of the
  /// next instruction, once every operand has been read.
  [[nodiscard]] auto pos() const -> size_t { return at; }
};

struct frame {
  const bytecode_function *function = nullptr;
  std::vector<slot_value> registers;
  /// This frame's `uninit[T, N]` storage (`op_stack_alloc`). Sized once,
  /// from `bytecode_function::stack_byte_size`, and never resized — every
  /// offset into it is fixed at compile time, and a pointer taken into it
  /// must stay valid for the life of the frame.
  runtime::aligned_buffer stack_bytes;
  size_t pc = 0;
  uint16_t result_reg = 0;
  bool has_caller = false;
};

auto push_frame(std::vector<frame> &frames, const bytecode_function &fn,
                std::span<const slot_value> args, bool has_caller,
                uint16_t result_reg) -> void {
  if (frames.size() >= k_max_call_depth) {
    throw panic_error(panic_reason::stack_overflow);
  }
  frame f;
  f.function = &fn;
  f.registers.assign(fn.register_count, slot_value{});
  // Zero-filled, matching the allocator's guarantee and the LLVM tier's
  // `memset` of the same buffer, so a read of an unwritten slot is at least
  // deterministic across tiers rather than exposing whatever the previous
  // frame left behind.
  f.stack_bytes = runtime::aligned_buffer(fn.stack_byte_size);
  for (size_t i = 0; i < args.size(); ++i) {
    f.registers[i] = args[i];
  }
  f.pc = 0;
  f.has_caller = has_caller;
  f.result_reg = result_reg;
  frames.push_back(std::move(f));
}

// ---------------------------------------------------------------------------
// Intrinsics (src/intrinsics.h) whose result comes back in a register. A
// descriptor argument (`str`, `slice[byte]`) is the address of the caller's
// `{ len, data }` bytes (`src/runtime/value_layout.h`). The intrinsics that
// build a descriptor or aggregate result are the runtime's own `_into`
// functions, below.
// ---------------------------------------------------------------------------

/// The bytes of the `{ len, data }` descriptor at the address in `header`.
[[nodiscard]] auto bytes_of(slot_value header) -> std::span<char> {
  const auto *slots = slots_of(header);
  auto *data = reinterpret_cast<char *>(static_cast<uintptr_t>(slots[1].u));
  return {data, static_cast<size_t>(slots[0].u)};
}

[[nodiscard]] auto view_of(slot_value str_header) -> std::string_view {
  const auto bytes = bytes_of(str_header);
  return {bytes.data(), bytes.size()};
}

auto intrinsic_rt_str_len_scalars(std::span<const slot_value> args)
    -> slot_value {
  const auto view = view_of(args[0]);
  size_t count = 0;
  size_t pos = 0;
  while (pos < view.size()) {
    if (!decode_utf8_scalar(view, pos).has_value()) {
      break;
    }
    ++count;
  }
  return slot_value{static_cast<uint64_t>(count)};
}

// `std.string` UTF-8 intrinsics (spec/std-reference.md). The algorithms live
// in `src/runtime/string_ops.h` and are shared verbatim with the LLVM tier's
// C-ABI wrappers (`src/runtime/string.cpp`).

auto intrinsic_rt_str_eq(std::span<const slot_value> args) -> slot_value {
  const auto equal =
      cinder::runtime::str_equal(view_of(args[0]), view_of(args[1]));
  return slot_value{static_cast<uint64_t>(equal ? 1 : 0)};
}

auto intrinsic_rt_str_cmp(std::span<const slot_value> args) -> slot_value {
  const auto cmp =
      cinder::runtime::str_compare(view_of(args[0]), view_of(args[1]));
  return slot_value{static_cast<int64_t>(cmp)};
}

// Bitcast intrinsics (`spec/todo.md` item 7): reinterpret a float's bit
// pattern as an equal-width unsigned integer, which `as uint64`/`as uint32`
// cannot do (that's a value conversion, `exec_cast`). `slot_value::f` is the
// active union member here, so the reinterpret goes through `std::bit_cast`
// rather than reading `.u` directly off it.

/// `rt_alloc`/`rt_realloc`/`rt_free` (`src/runtime/allocator.h`). A
/// `*mut byte` is a raw address in the slot's `.u`, exactly as it is in the
/// LLVM tier's `ptr` -- the two tiers hand the same pointer values to the
/// same allocator, which is what keeps a `list` built on these intrinsics
/// byte-for-byte identical across them.
auto intrinsic_rt_alloc(std::span<const slot_value> args) -> slot_value {
  auto *block = cinder_heap_alloc(args[0].u);
  return slot_value{reinterpret_cast<uint64_t>(block)}; // NOLINT
}

auto intrinsic_rt_realloc(std::span<const slot_value> args) -> slot_value {
  auto *block =
      cinder_heap_realloc(reinterpret_cast<void *>(args[0].u), // NOLINT
                          args[1].u, args[2].u);
  return slot_value{reinterpret_cast<uint64_t>(block)}; // NOLINT
}

auto intrinsic_rt_free(std::span<const slot_value> args) -> slot_value {
  cinder_heap_free(reinterpret_cast<void *>(args[0].u), args[1].u); // NOLINT
  return slot_value{};                                              // `unit`
}

/// The `vm` whose `execute` is running on this thread, so a task intrinsic
/// can hand the scheduler a runner that polls tasks on the same module.
thread_local const vm *g_current_vm = nullptr;

/// Makes `v` the current `vm` for the lifetime of the scope.
class current_vm_scope {
public:
  explicit current_vm_scope(const vm *v) : saved_(g_current_vm) {
    g_current_vm = v;
  }
  current_vm_scope(const current_vm_scope &) = delete;
  auto operator=(const current_vm_scope &) -> current_vm_scope & = delete;
  current_vm_scope(current_vm_scope &&) = delete;
  auto operator=(current_vm_scope &&) -> current_vm_scope & = delete;
  ~current_vm_scope() { g_current_vm = saved_; }

private:
  const vm *saved_;
};

auto run_task_on_vm(void *context, uint64_t *task) -> uint64_t {
  return static_cast<const vm *>(context)->poll_task(task);
}

[[nodiscard]] auto current_task_runner() -> cinder::runtime::scheduler::runner {
  return {.run = run_task_on_vm, .context = const_cast<vm *>(g_current_vm)};
}

[[nodiscard]] auto task_of(slot_value v) -> uint64_t * {
  return reinterpret_cast<uint64_t *>(v.u); // NOLINT
}

auto intrinsic_rt_task_block_on(std::span<const slot_value> args)
    -> slot_value {
  cinder::runtime::scheduler::block_on(
      task_of(args[0]),
      static_cast<cinder::runtime::scheduler::pool>(args[1].u),
      current_task_runner());
  return slot_value{}; // `unit`
}

auto intrinsic_rt_task_await_poll(std::span<const slot_value> args)
    -> slot_value {
  return store_bool(cinder::runtime::scheduler::await_poll(task_of(args[0]),
                                                           task_of(args[1])));
}

auto intrinsic_rt_task_spawn(std::span<const slot_value> args) -> slot_value {
  cinder::runtime::scheduler::spawn(
      task_of(args[0]),
      static_cast<cinder::runtime::scheduler::pool>(args[1].u),
      current_task_runner());
  return slot_value{}; // `unit`
}

auto intrinsic_rt_task_cancel(std::span<const slot_value> args) -> slot_value {
  return store_bool(cinder::runtime::scheduler::cancel(task_of(args[0])));
}

auto intrinsic_rt_task_is_finished(std::span<const slot_value> args)
    -> slot_value {
  return store_bool(cinder::runtime::scheduler::is_finished(task_of(args[0])));
}

auto intrinsic_rt_task_await_any_poll(std::span<const slot_value> args)
    -> slot_value {
  return store_bool(cinder::runtime::scheduler::await_any_poll(
      task_of(args[0]),
      reinterpret_cast<uint64_t *const *>(args[1].u), // NOLINT
      static_cast<size_t>(args[2].u)));
}

auto intrinsic_rt_task_block_on_any(std::span<const slot_value> args)
    -> slot_value {
  cinder::runtime::scheduler::block_on_any(
      reinterpret_cast<uint64_t *const *>(args[0].u), // NOLINT
      static_cast<size_t>(args[1].u), cinder::runtime::scheduler::pool::io,
      current_task_runner());
  return slot_value{}; // `unit`
}

auto intrinsic_rt_task_await_all_poll(std::span<const slot_value> args)
    -> slot_value {
  return store_bool(cinder::runtime::scheduler::await_all_poll(
      task_of(args[0]),
      reinterpret_cast<uint64_t *const *>(args[1].u), // NOLINT
      static_cast<size_t>(args[2].u)));
}

auto intrinsic_rt_task_block_on_all(std::span<const slot_value> args)
    -> slot_value {
  cinder::runtime::scheduler::block_on_all(
      reinterpret_cast<uint64_t *const *>(args[0].u), // NOLINT
      static_cast<size_t>(args[1].u), cinder::runtime::scheduler::pool::io,
      current_task_runner());
  return slot_value{}; // `unit`
}

auto intrinsic_rt_task_drop(std::span<const slot_value> args) -> slot_value {
  return store_bool(
      cinder::runtime::scheduler::cancel_on_drop(task_of(args[0])));
}

auto intrinsic_rt_task_release(std::span<const slot_value> args) -> slot_value {
  cinder::runtime::scheduler::release(task_of(args[0]));
  return slot_value{}; // `unit`
}

auto intrinsic_rt_crew_new(std::span<const slot_value> /*args*/) -> slot_value {
  return slot_value{reinterpret_cast<uint64_t>( // NOLINT
      cinder::runtime::scheduler::crew_new())};
}

auto intrinsic_rt_crew_spawn(std::span<const slot_value> args) -> slot_value {
  namespace sched = cinder::runtime::scheduler;
  sched::crew_spawn(reinterpret_cast<sched::crew *>(args[0].u), // NOLINT
                    task_of(args[1]), sched::pool::io, current_task_runner());
  return args[1];
}

auto intrinsic_rt_crew_join_poll(std::span<const slot_value> args)
    -> slot_value {
  namespace sched = cinder::runtime::scheduler;
  return store_bool(sched::crew_join_poll(
      task_of(args[0]),
      reinterpret_cast<sched::crew *>(args[1].u))); // NOLINT
}

auto intrinsic_rt_crew_free(std::span<const slot_value> args) -> slot_value {
  namespace sched = cinder::runtime::scheduler;
  sched::crew_free(reinterpret_cast<sched::crew *>(args[0].u)); // NOLINT
  return slot_value{};                                          // `unit`
}

auto intrinsic_rt_crew_join(std::span<const slot_value> args) -> slot_value {
  namespace sched = cinder::runtime::scheduler;
  sched::crew_join(reinterpret_cast<sched::crew *>(args[0].u), // NOLINT
                   sched::pool::io, current_task_runner());
  return slot_value{}; // `unit`
}

auto intrinsic_rt_crew_len(std::span<const slot_value> args) -> slot_value {
  namespace sched = cinder::runtime::scheduler;
  return store_unsigned(
      sched::crew_size(reinterpret_cast<sched::crew *>(args[0].u))); // NOLINT
}

auto intrinsic_rt_str_scalar_at(std::span<const slot_value> args) -> slot_value {
  return slot_value{static_cast<uint64_t>(cinder::runtime::cinder_rt_str_scalar_at(
      reinterpret_cast<const char *>(args[0].u), args[1].u, args[2].u))}; // NOLINT
}

auto intrinsic_rt_str_scalar_width(std::span<const slot_value> args)
    -> slot_value {
  return slot_value{cinder::runtime::cinder_rt_str_scalar_width(
      reinterpret_cast<const char *>(args[0].u), args[1].u, args[2].u)}; // NOLINT
}

auto intrinsic_rt_crew_task(std::span<const slot_value> args) -> slot_value {
  namespace sched = cinder::runtime::scheduler;
  return slot_value{reinterpret_cast<uint64_t>(sched::crew_task( // NOLINT
      reinterpret_cast<sched::crew *>(args[0].u), args[1].u))};  // NOLINT
}

[[nodiscard]] auto word_of(slot_value v) -> uint64_t * {
  return reinterpret_cast<uint64_t *>(v.u); // NOLINT
}

auto intrinsic_rt_atomic_load(std::span<const slot_value> args) -> slot_value {
  return slot_value{cinder_rt_atomic_load(word_of(args[0]))};
}

auto intrinsic_rt_atomic_store(std::span<const slot_value> args) -> slot_value {
  (void)cinder_rt_atomic_store(word_of(args[0]), args[1].u);
  return slot_value{}; // `unit`
}

auto intrinsic_rt_atomic_compare_exchange(std::span<const slot_value> args)
    -> slot_value {
  return slot_value{cinder_rt_atomic_compare_exchange(word_of(args[0]),
                                                      args[1].u, args[2].u)};
}

auto intrinsic_rt_word_wait(std::span<const slot_value> args) -> slot_value {
  cinder::runtime::scheduler::wait_word(word_of(args[0]), args[1].u);
  return slot_value{}; // `unit`
}

auto intrinsic_rt_word_waiter(std::span<const slot_value> args) -> slot_value {
  return slot_value{reinterpret_cast<uint64_t>( // NOLINT
      cinder::runtime::scheduler::word_waiter(word_of(args[0]), args[1].u))};
}

auto intrinsic_rt_word_wake(std::span<const slot_value> args) -> slot_value {
  cinder::runtime::scheduler::wake_word(word_of(args[0]));
  return slot_value{}; // `unit`
}

auto intrinsic_rt_fd_readable(std::span<const slot_value> args) -> slot_value {
  return slot_value{reinterpret_cast<uint64_t>( // NOLINT
      cinder::runtime::reactor::fd_waiter(
          static_cast<int>(args[0].i),
          cinder::runtime::reactor::interest::read))};
}

auto intrinsic_rt_fd_writable(std::span<const slot_value> args) -> slot_value {
  return slot_value{reinterpret_cast<uint64_t>( // NOLINT
      cinder::runtime::reactor::fd_waiter(
          static_cast<int>(args[0].i),
          cinder::runtime::reactor::interest::write))};
}

auto intrinsic_rt_sleep_until(std::span<const slot_value> args) -> slot_value {
  return slot_value{reinterpret_cast<uint64_t>( // NOLINT
      cinder::runtime::reactor::timer_waiter(args[0].u))};
}

auto intrinsic_rt_monotonic_now(std::span<const slot_value> /*args*/)
    -> slot_value {
  return slot_value{cinder::runtime::reactor::monotonic_now()};
}

auto intrinsic_rt_errno_would_block(std::span<const slot_value> /*args*/)
    -> slot_value {
  return slot_value{int64_t{EAGAIN}};
}

auto intrinsic_rt_bitcast_f64_to_u64(std::span<const slot_value> args)
    -> slot_value {
  return slot_value{std::bit_cast<uint64_t>(args[0].f)};
}

auto intrinsic_rt_bitcast_f32_to_u32(std::span<const slot_value> args)
    -> slot_value {
  // A `float32` value's slot was written by `store_f32`, whose active union
  // member is `.u` (the bit pattern, zero-extended) rather than `.f` --
  // unlike `float64`, which stores straight into `.f`.
  const auto bits = static_cast<uint32_t>(args[0].u);
  return slot_value{static_cast<uint64_t>(bits)};
}

// ---------------------------------------------------------------------------
// `std.platform` runtime intrinsics (spec/std-reference.md). Each queries the
// host through `src/runtime/platform_query.h` (shared with this tier's
// AOT/JIT counterpart, `src/runtime/platform.cpp`, so the OS-specific query
// logic itself isn't duplicated a third time) and encodes the result per
// this file's existing struct/result conventions.
// ---------------------------------------------------------------------------

namespace platform_query = cinder::runtime::platform_query;

/// Mirrors `cinder_rt_panic` (`src/runtime/io.cpp`) for the bytecode tier:
/// reports `msg` on stderr, then unwinds to the scheduler on a worker and
/// terminates with `k_panic_exit_code` elsewhere. Both tiers do the same, so
/// a panicking program behaves identically whichever backend ran it — which
/// is what lets `codegen_stress_test` cross-check them.
[[noreturn]] auto intrinsic_rt_panic(std::span<const slot_value> args)
    -> slot_value {
  const auto text = view_of(args[0]);
  if (panics_are_hosted()) {
    throw panic_error(panic_reason::explicit_panic, std::string(text));
  }
  cinder::runtime::scheduler::report_panic(text);
  if (cinder::runtime::scheduler::on_worker()) {
    throw panic_error(panic_reason::explicit_panic, std::string(text));
  }
  cinder::runtime::scheduler::exit_after_panic();
}

/// Whether intrinsic `id` reaches outside the VM: the console and files, or
/// a query about the machine the program runs on. A hosted run refuses these
/// unless its `run_limits` allow them.
[[nodiscard]] auto intrinsic_has_effects(uint8_t id) -> bool {
  const auto name = cinder::known_intrinsic_names.at(id);
  return name == "rt_stdin" || name == "rt_stdout" || name == "rt_stderr" ||
         name == "rt_open" || name == "rt_close" || name == "rt_read" ||
         name == "rt_write" || name == "rt_flush" || name == "rt_uname" ||
         name == "rt_gethostname" || name == "rt_processor_name" ||
         name == "rt_libc_version" || name == "rt_windows_version" ||
         name == "rt_macos_version" || name == "rt_task_block_on" ||
         name == "rt_task_await_poll" || name == "rt_task_spawn" ||
         name == "rt_task_cancel" || name == "rt_task_is_finished" ||
         name == "rt_task_await_any_poll" || name == "rt_task_block_on_any" ||
         name == "rt_task_await_all_poll" || name == "rt_task_block_on_all" ||
         name == "rt_task_drop" || name == "rt_crew_new" ||
         name == "rt_crew_spawn" || name == "rt_crew_join_poll" ||
         name == "rt_crew_join" || name == "rt_crew_free" ||
         name == "rt_crew_len" || name == "rt_crew_task" ||
         name == "rt_word_wait" || name == "rt_word_waiter" ||
         name == "rt_word_wake" || name == "rt_fd_readable" ||
         name == "rt_fd_writable" || name == "rt_sleep_until" ||
         name == "rt_monotonic_now" || name == "rt_pipe" ||
         name == "rt_set_nonblocking";
}

using intrinsic_fn = slot_value (*)(std::span<const slot_value>);

/// Indexed by `op_call_intrinsic`'s `intrinsic_id` operand — must stay in
/// the exact order of `cinder::known_intrinsic_names` (src/intrinsics.h),
/// which is also the order the semantic checker validated `intrinsic def`
/// names against.
constexpr std::array<intrinsic_fn, cinder::known_intrinsics.size()>
    k_intrinsics = {{
        nullptr, // rt_stdin: built in caller storage
        nullptr, // rt_stdout: built in caller storage
        nullptr, // rt_stderr: built in caller storage
        nullptr, // rt_open: built in caller storage
        nullptr, // rt_close: built in caller storage
        nullptr, // rt_read: built in caller storage
        nullptr, // rt_write: built in caller storage
        nullptr, // rt_flush: built in caller storage
        nullptr, // rt_str_concat: built in caller storage
        intrinsic_rt_str_len_scalars,
        nullptr, // rt_str_repeat_char: built in caller storage
        nullptr, // rt_str_truncate_scalars: built in caller storage
        nullptr, // rt_fmt_radix_digits: built in caller storage
        nullptr, // rt_fmt_f64_fixed: built in caller storage
        nullptr, // rt_fmt_f64_sci: built in caller storage
        nullptr, // rt_fmt_f64_general: built in caller storage
        nullptr, // rt_fmt_char_from_codepoint: built in caller storage
        intrinsic_rt_str_eq,
        intrinsic_rt_str_cmp,
        nullptr, // rt_str_find: built in caller storage
        nullptr, // rt_str_rfind: built in caller storage
        nullptr, // rt_str_reverse: built in caller storage
        nullptr, // rt_str_trim: built in caller storage
        nullptr, // rt_str_replace: built in caller storage
        nullptr, // rt_uname: built in caller storage
        nullptr, // rt_gethostname: built in caller storage
        nullptr, // rt_processor_name: built in caller storage
        nullptr, // rt_libc_version: built in caller storage
        nullptr, // rt_windows_version: built in caller storage
        nullptr, // rt_macos_version: built in caller storage
        intrinsic_rt_panic,
        intrinsic_rt_bitcast_f64_to_u64,
        intrinsic_rt_bitcast_f32_to_u32,
        intrinsic_rt_alloc,
        intrinsic_rt_realloc,
        intrinsic_rt_free,
        intrinsic_rt_task_block_on,
        intrinsic_rt_task_await_poll,
        intrinsic_rt_task_spawn,
        intrinsic_rt_task_cancel,
        intrinsic_rt_task_is_finished,
        intrinsic_rt_task_await_any_poll,
        intrinsic_rt_task_block_on_any,
        intrinsic_rt_task_await_all_poll,
        intrinsic_rt_task_block_on_all,
        intrinsic_rt_task_drop,
        intrinsic_rt_task_release,
        intrinsic_rt_crew_new,
        intrinsic_rt_crew_spawn,
        intrinsic_rt_crew_join_poll,
        intrinsic_rt_crew_join,
        intrinsic_rt_crew_free,
        intrinsic_rt_crew_len,
        intrinsic_rt_crew_task,
        intrinsic_rt_atomic_load,
        intrinsic_rt_atomic_store,
        intrinsic_rt_atomic_compare_exchange,
        intrinsic_rt_word_wait,
        intrinsic_rt_word_waiter,
        intrinsic_rt_word_wake,
        intrinsic_rt_fd_readable,
        intrinsic_rt_fd_writable,
        intrinsic_rt_sleep_until,
        intrinsic_rt_monotonic_now,
        nullptr, // rt_pipe: built in caller storage
        nullptr, // rt_set_nonblocking: built in caller storage
        intrinsic_rt_errno_would_block,
        intrinsic_rt_str_scalar_at,
        intrinsic_rt_str_scalar_width,
    }};

static_assert(k_intrinsics.size() == cinder::known_intrinsic_names.size(),
              "every known intrinsic needs exactly one native implementation, "
              "in the same order");

// ---------------------------------------------------------------------------
// Intrinsics that build their result in caller storage (`into`, see
// `src/intrinsics.h`). The storage compiler (`bytecode_compiler/
// storage_compile.cpp`) calls every intrinsic with `op_call_intrinsic`, and
// for these the first argument register holds the address of the destination
// and the rest follow the declared parameters: a scalar in its register, a
// descriptor or aggregate as the address of the caller's bytes. They are the
// runtime's own `cinder_rt_*_into` functions, so the two tiers cannot write a
// result differently; no heap block is made for the result.
// ---------------------------------------------------------------------------

using into_fn = void (*)(std::span<const slot_value>);

[[nodiscard]] auto address_arg(const slot_value &value) -> void * {
  return reinterpret_cast<void *>(static_cast<uintptr_t>(value.u)); // NOLINT
}

[[nodiscard]] auto word_arg(const slot_value &value) -> uint32_t {
  return static_cast<uint32_t>(value.u);
}

/// Handler `id`, or null when the intrinsic returns its result directly.
[[nodiscard]] auto into_handler(uint8_t id) -> into_fn {
  static const auto table = [] {
    auto handlers = std::array<into_fn, cinder::known_intrinsics.size()>{};
    const auto set = [&handlers](std::string_view name, into_fn handler) {
      handlers.at(cinder::intrinsic_index_of(name).value()) = handler;
    };
    set("rt_stdin", [](auto a) { cinder_rt_stdin_into(address_arg(a[0])); });
    set("rt_stdout", [](auto a) { cinder_rt_stdout_into(address_arg(a[0])); });
    set("rt_stderr", [](auto a) { cinder_rt_stderr_into(address_arg(a[0])); });
    set("rt_open", [](auto a) {
      cinder_rt_open_into(address_arg(a[0]), address_arg(a[1]),
                          address_arg(a[2]));
    });
    set("rt_close", [](auto a) {
      cinder_rt_close_into(address_arg(a[0]), address_arg(a[1]));
    });
    set("rt_read", [](auto a) {
      cinder_rt_read_into(address_arg(a[0]), address_arg(a[1]),
                          address_arg(a[2]));
    });
    set("rt_write", [](auto a) {
      cinder_rt_write_into(address_arg(a[0]), address_arg(a[1]),
                           address_arg(a[2]));
    });
    set("rt_flush", [](auto a) {
      cinder_rt_flush_into(address_arg(a[0]), address_arg(a[1]));
    });
    set("rt_pipe", [](auto a) { cinder_rt_pipe_into(address_arg(a[0])); });
    set("rt_set_nonblocking", [](auto a) {
      cinder_rt_set_nonblocking_into(address_arg(a[0]), address_arg(a[1]));
    });
    set("rt_str_concat", [](auto a) {
      cinder_rt_str_concat_into(address_arg(a[0]), address_arg(a[1]),
                                address_arg(a[2]));
    });
    set("rt_str_repeat_char", [](auto a) {
      cinder_rt_str_repeat_char_into(address_arg(a[0]), word_arg(a[1]), a[2].u);
    });
    set("rt_str_truncate_scalars", [](auto a) {
      cinder_rt_str_truncate_scalars_into(address_arg(a[0]), address_arg(a[1]),
                                          a[2].u);
    });
    set("rt_fmt_radix_digits", [](auto a) {
      cinder_rt_fmt_radix_digits_into(address_arg(a[0]), a[1].u, word_arg(a[2]),
                                      word_arg(a[3]));
    });
    set("rt_fmt_f64_fixed", [](auto a) {
      cinder_rt_fmt_f64_fixed_into(address_arg(a[0]), a[1].f, a[2].u);
    });
    set("rt_fmt_f64_sci", [](auto a) {
      cinder_rt_fmt_f64_sci_into(address_arg(a[0]), a[1].f, a[2].u,
                                 word_arg(a[3]));
    });
    set("rt_fmt_f64_general", [](auto a) {
      cinder_rt_fmt_f64_general_into(address_arg(a[0]), a[1].f, a[2].u);
    });
    set("rt_fmt_char_from_codepoint", [](auto a) {
      cinder_rt_fmt_char_from_codepoint_into(address_arg(a[0]), word_arg(a[1]));
    });
    set("rt_str_find", [](auto a) {
      cinder_rt_str_find_into(address_arg(a[0]), address_arg(a[1]),
                              address_arg(a[2]), a[3].u);
    });
    set("rt_str_rfind", [](auto a) {
      cinder_rt_str_rfind_into(address_arg(a[0]), address_arg(a[1]),
                               address_arg(a[2]));
    });
    set("rt_str_reverse", [](auto a) {
      cinder_rt_str_reverse_into(address_arg(a[0]), address_arg(a[1]));
    });
    set("rt_str_trim", [](auto a) {
      cinder_rt_str_trim_into(address_arg(a[0]), address_arg(a[1]),
                              word_arg(a[2]));
    });
    set("rt_str_replace", [](auto a) {
      cinder_rt_str_replace_into(address_arg(a[0]), address_arg(a[1]),
                                 address_arg(a[2]), address_arg(a[3]));
    });
    set("rt_uname", [](auto a) { cinder_rt_uname_into(address_arg(a[0])); });
    set("rt_gethostname",
        [](auto a) { cinder_rt_gethostname_into(address_arg(a[0])); });
    set("rt_processor_name",
        [](auto a) { cinder_rt_processor_name_into(address_arg(a[0])); });
    set("rt_libc_version",
        [](auto a) { cinder_rt_libc_version_into(address_arg(a[0])); });
    set("rt_windows_version",
        [](auto a) { cinder_rt_windows_version_into(address_arg(a[0])); });
    set("rt_macos_version",
        [](auto a) { cinder_rt_macos_version_into(address_arg(a[0])); });
    // A row marked `into` without a handler here would run the older entry
    // point and treat its heap block as the destination. Refuse to start.
    for (size_t id = 0; id < handlers.size(); ++id) {
      if (cinder::intrinsic_builds_into(id) != (handlers.at(id) != nullptr)) {
        std::abort();
      }
    }
    return handlers;
  }();
  return table.at(id);
}

} // namespace

auto vm::run(uint16_t function_index, std::span<const slot_value> args) const
    -> std::expected<vm_result, panic_reason> {
  auto result = execute(function_index, args, run_limits{});
  if (!result.has_value()) {
    return std::unexpected(result.error().reason);
  }
  return *result;
}

auto vm::poll_task(uint64_t *task) const -> uint64_t {
  namespace co = cinder::runtime::coroutine;
  const auto step_function =
      static_cast<uint16_t>(task[co::k_step_function_slot]);
  const std::array<slot_value, co::k_step_arg_count> args = {
      slot_value{task[co::k_state_ptr_slot]},
      slot_value{task[co::k_resume_index_slot]},
      slot_value{reinterpret_cast<uint64_t>(task)}}; // NOLINT
  auto result = execute(step_function, args, run_limits{});
  if (!result.has_value()) {
    // Most of the VM's panics are thrown without a report. One already
    // reported, or rethrown from an awaited task, is not printed again.
    cinder::runtime::scheduler::report_panic(result.error().message);
    throw panic_error(result.error().reason, result.error().message);
  }
  return result->value.u;
}

auto vm::run_hosted(uint16_t function_index, std::span<const slot_value> args,
                    const run_limits &limits) const
    -> std::expected<vm_result, vm_failure> {
  const auto hosted = hosted_panic_scope{};
  return execute(function_index, args, limits);
}

auto vm::entry_with_result(uint16_t function_index, std::span<std::byte> result,
                           std::span<const slot_value> args) const
    -> std::expected<std::vector<slot_value>, vm_failure> {
  const auto refuse = [](std::string message) {
    return std::unexpected(vm_failure{.reason = panic_reason::host_failed,
                                      .message = std::move(message)});
  };
  if (function_index >= module_.functions.size()) {
    return refuse(std::format("there is no function {}", function_index));
  }
  const auto &function = module_.functions[function_index];
  if (!function.abi.has_value() || !function.abi->returns_through_address) {
    return refuse(std::format("`{}` does not build its result through an "
                              "address",
                              function.name));
  }
  const auto &abi = *function.abi;
  if (result.size() < abi.result.size) {
    return refuse(std::format("`{}` needs {} bytes of result storage but {} "
                              "were given",
                              function.name, abi.result.size, result.size()));
  }
  if (reinterpret_cast<uintptr_t>(result.data()) % abi.result.align != 0) {
    return refuse(std::format("the result storage for `{}` must be aligned "
                              "to {} bytes",
                              function.name, abi.result.align));
  }
  if (args.size() + 1 != function.param_count) {
    return refuse(std::format("`{}` takes {} argument registers besides its "
                              "result address but {} were given",
                              function.name, function.param_count - 1,
                              args.size()));
  }
  auto entry = std::vector<slot_value>{};
  entry.reserve(args.size() + 1);
  entry.push_back(ptr_to_slot(result.data()));
  entry.insert(entry.end(), args.begin(), args.end());
  return entry;
}

auto vm::run_into(uint16_t function_index, std::span<std::byte> result,
                  std::span<const slot_value> args) const
    -> std::expected<vm_result, vm_failure> {
  const auto entry = entry_with_result(function_index, result, args);
  if (!entry.has_value()) {
    return std::unexpected(entry.error());
  }
  return execute(function_index, *entry, run_limits{});
}

auto vm::run_hosted_into(uint16_t function_index, std::span<std::byte> result,
                         std::span<const slot_value> args,
                         const run_limits &limits) const
    -> std::expected<vm_result, vm_failure> {
  const auto entry = entry_with_result(function_index, result, args);
  if (!entry.has_value()) {
    return std::unexpected(entry.error());
  }
  const auto hosted = hosted_panic_scope{};
  return execute(function_index, *entry, limits);
}

auto vm::execute(uint16_t function_index, std::span<const slot_value> args,
                 const run_limits &limits) const
    -> std::expected<vm_result, vm_failure> {
  // The shared part of the VM is `module_` and the statics; everything an
  // execution mutates (its frames and counters) is local to this call, so
  // concurrent task polls never touch each other's state.
  const auto current = current_vm_scope(this);
  std::vector<frame> frames;
  auto steps = uint64_t{0};
  // The start of the instruction running now, which is where a failure in
  // the innermost frame points. A caller's `pc` is already past its call.
  auto instruction_start = size_t{0};
  const auto budget = limits.step_budget.value_or(UINT64_MAX);

  try {
    push_frame(frames, module_.functions.at(function_index), args, false, 0);

    while (true) {
      frame &f = frames.back();
      const auto &code = f.function->code;
      const auto op = static_cast<opcode>(code[f.pc]);
      const size_t ip = f.pc + 1;
      instruction_start = f.pc;
      if (++steps > budget) {
        throw panic_error(panic_reason::step_budget_exhausted);
      }

      switch (op) {
      case opcode::op_load_const: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto dst = ops.reg();
        const auto idx = ops.imm32();
        f.registers[dst] = f.function->constants[idx];
        f.pc = ops.pos();
        break;
      }
      case opcode::op_move: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto dst = ops.reg();
        const auto src = ops.reg();
        f.registers[dst] = f.registers[src];
        f.pc = ops.pos();
        break;
      }
      case opcode::op_add:
      case opcode::op_sub:
      case opcode::op_mul:
      case opcode::op_div:
      case opcode::op_mod:
      case opcode::op_add_wrap:
      case opcode::op_sub_wrap:
      case opcode::op_mul_wrap:
      case opcode::op_add_sat:
      case opcode::op_sub_sat:
      case opcode::op_mul_sat:
      case opcode::op_bitand:
      case opcode::op_bitor:
      case opcode::op_bitxor:
      case opcode::op_shl:
      case opcode::op_shr:
      case opcode::op_eq:
      case opcode::op_ne:
      case opcode::op_lt:
      case opcode::op_le:
      case opcode::op_gt:
      case opcode::op_ge: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto dst = ops.reg();
        const auto lhs = ops.reg();
        const auto rhs = ops.reg();
        const auto kind = static_cast<numeric_kind>(ops.imm8());
        f.registers[dst] =
            dispatch_binary(op, kind, f.registers[lhs], f.registers[rhs]);
        f.pc = ops.pos();
        break;
      }

      case opcode::op_neg:
      case opcode::op_bitnot: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto dst = ops.reg();
        const auto src = ops.reg();
        const auto kind = static_cast<numeric_kind>(ops.imm8());
        f.registers[dst] = dispatch_unary(op, kind, f.registers[src]);
        f.pc = ops.pos();
        break;
      }

      case opcode::op_not_bool: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto dst = ops.reg();
        const auto src = ops.reg();
        f.registers[dst] = store_bool((f.registers[src].u & 1U) == 0U);
        f.pc = ops.pos();
        break;
      }

      case opcode::op_cast: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto dst = ops.reg();
        const auto src = ops.reg();
        const auto from_kind = static_cast<numeric_kind>(ops.imm8());
        const auto to_kind = static_cast<numeric_kind>(ops.imm8());
        f.registers[dst] = exec_cast(from_kind, to_kind, f.registers[src]);
        f.pc = ops.pos();
        break;
      }

      case opcode::op_jump: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto offset = ops.rel32();
        f.pc = static_cast<size_t>(static_cast<int64_t>(ops.pos()) + offset);
        break;
      }
      case opcode::op_jump_if_false: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto cond = ops.reg();
        const auto offset = ops.rel32();
        const size_t after = ops.pos();
        const bool value = (f.registers[cond].u & 1U) != 0U;
        f.pc = value
                   ? after
                   : static_cast<size_t>(static_cast<int64_t>(after) + offset);
        break;
      }
      case opcode::op_jump_if_true: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto cond = ops.reg();
        const auto offset = ops.rel32();
        const size_t after = ops.pos();
        const bool value = (f.registers[cond].u & 1U) != 0U;
        f.pc = value ? static_cast<size_t>(static_cast<int64_t>(after) + offset)
                     : after;
        break;
      }

      case opcode::op_call: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto dst = ops.reg();
        const auto fn_idx = ops.imm16();
        const auto first_arg = ops.reg();
        const auto argc = ops.imm8();
        f.pc = ops.pos();
        const std::vector<slot_value> call_args(f.registers.begin() + first_arg,
                                                f.registers.begin() +
                                                    first_arg + argc);
        push_frame(frames, module_.functions.at(fn_idx), call_args, true, dst);
        continue; // `f` is invalidated by push_frame's push_back.
      }
      case opcode::op_return_value: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto src = ops.reg();
        const slot_value result = f.registers[src];
        const bool has_caller = f.has_caller;
        const auto result_reg = f.result_reg;
        frames.pop_back();
        if (!has_caller) {
          return vm_result{.has_value = true, .value = result};
        }
        frames.back().registers[result_reg] = result;
        continue;
      }
      case opcode::op_return_unit: {
        const bool has_caller = f.has_caller;
        frames.pop_back();
        if (!has_caller) {
          return vm_result{.has_value = false, .value = slot_value{}};
        }
        continue;
      }

      case opcode::op_tail_call: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto fn_idx = ops.imm16();
        const auto first_arg = ops.reg();
        const auto argc = ops.imm8();
        const std::vector<slot_value> call_args(f.registers.begin() + first_arg,
                                                f.registers.begin() +
                                                    first_arg + argc);
        // Reuse the top frame in place — the whole of the VM's tail-call
        // guarantee: `frames.size()` never grows, so an arbitrarily long
        // tail-call chain runs in constant stack space. `has_caller`/
        // `result_reg` are untouched: an eventual return still hands its
        // result to whoever called into this frame originally, exactly as
        // if the whole chain had been one call.
        const auto &callee = module_.functions.at(fn_idx);
        f.function = &callee;
        f.registers.assign(callee.register_count, slot_value{});
        // The callee's own `uninit` storage, sized and zeroed exactly as
        // `push_frame` would. Nothing can still point into the old range:
        // `hir::mark_tail_calls` never marks a call out of a function that
        // owns a buffer.
        f.stack_bytes = runtime::aligned_buffer(callee.stack_byte_size);
        for (size_t i = 0; i < call_args.size(); ++i) {
          f.registers[i] = call_args[i];
        }
        f.pc = 0;
        continue;
      }

      case opcode::op_call_intrinsic: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto dst = ops.reg();
        const auto intrinsic_id = ops.imm8();
        const auto first_arg = ops.reg();
        const auto argc = ops.imm8();
        f.pc = ops.pos();
        const std::span<const slot_value> call_args(
            f.registers.data() + first_arg, argc);
        if (!limits.allow_effects && intrinsic_has_effects(intrinsic_id)) {
          throw panic_error(
              panic_reason::effect_refused,
              std::format("`{}` reaches outside the program, which code run "
                          "while compiling may not do",
                          cinder::known_intrinsic_names.at(intrinsic_id)));
        }
        const auto handler = k_intrinsics.at(intrinsic_id);
        if (handler == nullptr) {
          throw panic_error(
              panic_reason::host_failed,
              std::format("`{}` builds its result in caller storage; call it "
                          "with `op_call_intrinsic_into`",
                          cinder::known_intrinsic_names.at(intrinsic_id)));
        }
        f.registers[dst] = handler(call_args);
        break;
      }

      case opcode::op_call_intrinsic_into: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto intrinsic_id = ops.imm8();
        const auto first_arg = ops.reg();
        const auto argc = ops.imm8();
        f.pc = ops.pos();
        const std::span<const slot_value> call_args(
            f.registers.data() + first_arg, argc);
        if (!limits.allow_effects && intrinsic_has_effects(intrinsic_id)) {
          throw panic_error(
              panic_reason::effect_refused,
              std::format("`{}` reaches outside the program, which code run "
                          "while compiling may not do",
                          cinder::known_intrinsic_names.at(intrinsic_id)));
        }
        const auto into = into_handler(intrinsic_id);
        if (into == nullptr) {
          throw panic_error(
              panic_reason::host_failed,
              std::format("`{}` does not build its result in caller storage",
                          cinder::known_intrinsic_names.at(intrinsic_id)));
        }
        // The destination is `call_args[0]`; nothing comes back.
        into(call_args);
        break;
      }

      case opcode::op_host_call: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto dst = ops.reg();
        const auto site = ops.imm16();
        const auto first_arg = ops.reg();
        const auto argc = ops.imm8();
        f.pc = ops.pos();
        const auto &described = f.function->host_sites.at(site);
        if (limits.host == nullptr) {
          throw panic_error(
              panic_reason::host_failed,
              std::format("`{}` works on quoted syntax, which exists only "
                          "while compiling",
                          described.operation));
        }
        const std::span<const slot_value> call_args(
            f.registers.data() + first_arg, argc);
        auto result = (*limits.host)(described, call_args);
        if (!result.has_value()) {
          throw panic_error(panic_reason::host_failed,
                            std::move(result.error()));
        }
        f.registers[dst] = *result;
        break;
      }

      case opcode::op_stack_alloc: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto dst = ops.reg();
        const auto byte_offset = ops.imm32();
        // The range was reserved whole on frame entry, so this is address
        // arithmetic and nothing else — no allocation, and the result stays
        // valid until the frame is popped.
        f.registers[dst] = ptr_to_slot(f.stack_bytes.data() + byte_offset);
        f.pc = ops.pos();
        break;
      }
      case opcode::op_alloc: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto dst = ops.reg();
        const auto byte_size = ops.imm64();
        auto *raw = cinder_heap_alloc(byte_size);
        f.registers[dst] = ptr_to_slot(raw);
        f.pc = ops.pos();
        break;
      }
      case opcode::op_load_slot: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto dst = ops.reg();
        const auto ptr_reg = ops.reg();
        const auto byte_offset = ops.imm16();
        const auto field_size = ops.imm8();
        f.registers[dst] = load_sized(
            raw_bytes_of(f.registers[ptr_reg]) + byte_offset, field_size);
        f.pc = ops.pos();
        break;
      }
      case opcode::op_store_slot: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto ptr_reg = ops.reg();
        const auto byte_offset = ops.imm16();
        const auto src = ops.reg();
        const auto field_size = ops.imm8();
        store_sized(raw_bytes_of(f.registers[ptr_reg]) + byte_offset,
                    field_size, f.registers[src]);
        f.pc = ops.pos();
        break;
      }

      case opcode::op_addr_slot: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto dst = ops.reg();
        const auto ptr_reg = ops.reg();
        const auto byte_offset = ops.imm16();
        f.registers[dst].u = f.registers[ptr_reg].u + byte_offset;
        f.pc = ops.pos();
        break;
      }
      case opcode::op_addr_indexed: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto dst = ops.reg();
        const auto ptr_reg = ops.reg();
        const auto index_reg = ops.reg();
        const auto elem_size = ops.imm8();
        f.registers[dst].u = f.registers[ptr_reg].u +
                             (f.registers[index_reg].u * uint64_t{elem_size});
        f.pc = ops.pos();
        break;
      }

      case opcode::op_global_addr: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto dst = ops.reg();
        const auto offset = ops.imm32();
        f.registers[dst] =
            ptr_to_slot(reinterpret_cast<std::byte *>(global_bytes_.data()) +
                        offset);
        f.pc = ops.pos();
        break;
      }
      case opcode::op_str_const_data: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto dst = ops.reg();
        const auto idx = ops.imm32();
        const auto &text = f.function->string_constants[idx];
        f.registers[dst] = ptr_to_slot(const_cast<char *>(text.data()));
        f.pc = ops.pos();
        break;
      }
      case opcode::op_panic_if: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto cond = ops.reg();
        const auto reason = static_cast<panic_reason>(ops.imm8());
        if ((f.registers[cond].u & 1U) != 0U) {
          raise_panic(reason);
        }
        f.pc = ops.pos();
        break;
      }

      case opcode::op_panic:
        raise_panic(panic_reason::explicit_panic);

      case opcode::op_call_function: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto dst = ops.reg();
        const auto entry_reg = ops.reg();
        const auto first_arg = ops.reg();
        const auto argc = ops.imm8();
        f.pc = ops.pos();
        const auto entry = static_cast<size_t>(f.registers[entry_reg].u);
        const std::vector<slot_value> call_args(
            f.registers.begin() + first_arg,
            f.registers.begin() + first_arg + argc);
        push_frame(frames, module_.functions.at(entry), call_args, true, dst);
        continue; // `f` is invalidated by push_frame's push_back.
      }
      case opcode::op_drop_function_value: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto dst = ops.reg();
        const auto value_reg = ops.reg();
        f.pc = ops.pos();
        const auto *bytes = raw_bytes_of(f.registers[value_reg]);
        const auto glue = load_sized(bytes + 16, 8).u;
        if (glue == 0) {
          f.registers[dst] = slot_value{};
          break;
        }
        const std::array<slot_value, 1> call_args = {load_sized(bytes + 8, 8)};
        push_frame(frames, module_.functions.at(static_cast<size_t>(glue - 1)),
                   call_args, true, dst);
        continue; // `f` is invalidated by push_frame's push_back.
      }

      case opcode::op_coroutine_resume: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto dst = ops.reg();
        const auto coroutine_reg = ops.reg();
        f.pc = ops.pos();
        auto *gen_slots = slots_of(f.registers[coroutine_reg]);
        // Atomic: the scheduler may write the status from another thread.
        if (const auto status =
                std::atomic_ref(gen_slots[runtime::coroutine::k_status_slot].u)
                    .load(std::memory_order_acquire);
            status !=
            runtime::coroutine::raw(runtime::coroutine::status::suspended)) {
          f.registers[dst] = slot_value{status};
          break;
        }
        const auto step_fn_idx = static_cast<uint16_t>(
            gen_slots[runtime::coroutine::k_step_function_slot].u);
        const std::array<slot_value, runtime::coroutine::k_step_arg_count>
            call_args = {
                gen_slots[runtime::coroutine::k_state_ptr_slot].as_slot(),
                gen_slots[runtime::coroutine::k_resume_index_slot].as_slot(),
                f.registers[coroutine_reg]};
        push_frame(frames, module_.functions.at(step_fn_idx), call_args, true,
                   dst);
        continue; // `f` is invalidated by push_frame's push_back.
      }
      case opcode::op_coroutine_cancel: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto dst = ops.reg();
        const auto generator_reg = ops.reg();
        f.pc = ops.pos();
        auto *gen_slots = slots_of(f.registers[generator_reg]);
        // Atomic: a worker may be finishing a task the scheduler owns.
        if (std::atomic_ref(gen_slots[runtime::coroutine::k_status_slot].u)
                .load(std::memory_order_acquire) !=
            runtime::coroutine::raw(runtime::coroutine::status::suspended)) {
          f.registers[dst] = slot_value{};
          break;
        }
        // A task the scheduler owns may be queued or running on another
        // thread, so its owner unwinds it; anything else is resumed here.
        if (cinder::runtime::scheduler::cancel_on_drop(
                reinterpret_cast<uint64_t *>(gen_slots))) { // NOLINT
          f.registers[dst] = slot_value{};
          break;
        }
        std::atomic_ref(gen_slots[runtime::coroutine::k_status_slot].u)
            .store(runtime::coroutine::raw(
                       runtime::coroutine::status::cancel_requested),
                   std::memory_order_release);
        const auto step_fn_idx = static_cast<uint16_t>(
            gen_slots[runtime::coroutine::k_step_function_slot].u);
        const std::array<slot_value, runtime::coroutine::k_step_arg_count>
            call_args = {
                gen_slots[runtime::coroutine::k_state_ptr_slot].as_slot(),
                gen_slots[runtime::coroutine::k_resume_index_slot].as_slot(),
                f.registers[generator_reg]};
        push_frame(frames, module_.functions.at(step_fn_idx), call_args, true,
                   dst);
        continue; // `f` is invalidated by push_frame's push_back.
      }
      case opcode::op_coroutine_cancelled: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto dst = ops.reg();
        const auto generator_reg = ops.reg();
        f.pc = ops.pos();
        // Atomic: the scheduler may write the status from another thread.
        f.registers[dst] = store_bool(
            std::atomic_ref(
                slots_of(f.registers[generator_reg])
                    [runtime::coroutine::k_status_slot]
                        .u)
                .load(std::memory_order_acquire) ==
            runtime::coroutine::raw(
                runtime::coroutine::status::cancel_requested));
        break;
      }
      case opcode::op_coroutine_finish: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto generator_reg = ops.reg();
        f.pc = ops.pos();
        std::atomic_ref(
            slots_of(f.registers[generator_reg])[runtime::coroutine::k_status_slot]
                .u)
            .store(runtime::coroutine::raw(runtime::coroutine::status::done),
                   std::memory_order_release);
        break;
      }
      case opcode::op_copy_bytes: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto dst_reg = ops.reg();
        const auto src_reg = ops.reg();
        const auto byte_count = ops.imm32();
        f.pc = ops.pos();
        if (byte_count != 0) {
          std::memmove(raw_bytes_of(f.registers[dst_reg]),
                       raw_bytes_of(f.registers[src_reg]), byte_count);
        }
        break;
      }
      case opcode::op_zero_bytes: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto dst_reg = ops.reg();
        const auto byte_count = ops.imm32();
        f.pc = ops.pos();
        if (byte_count != 0) {
          std::memset(raw_bytes_of(f.registers[dst_reg]), 0, byte_count);
        }
        break;
      }
      case opcode::op_atomic_fetch_add:
      case opcode::op_atomic_fetch_sub: {
        auto ops = operand_cursor{.code = code, .at = ip};
        const auto dst = ops.reg();
        const auto ptr_reg = ops.reg();
        const auto n_reg = ops.reg();
        f.pc = ops.pos();
        auto word = std::atomic_ref<uint64_t>(
            *reinterpret_cast<uint64_t *>(raw_bytes_of(f.registers[ptr_reg])));
        const auto n = f.registers[n_reg].u;
        f.registers[dst] =
            slot_value{op == opcode::op_atomic_fetch_add
                           ? word.fetch_add(n, std::memory_order_acq_rel)
                           : word.fetch_sub(n, std::memory_order_acq_rel)};
        break;
      }
      }
    }
  } catch (const panic_error &err) {
    auto failure = vm_failure{.reason = err.reason(), .message = err.what()};
    for (const auto &active : std::views::reverse(frames)) {
      const auto index =
          static_cast<size_t>(active.function - module_.functions.data());
      const auto innermost = failure.frames.empty();
      failure.frames.push_back(
          vm_frame_location{.function_index = static_cast<uint16_t>(index),
                            .offset = innermost       ? instruction_start
                                      : active.pc > 0 ? active.pc - 1
                                                      : 0});
    }
    return std::unexpected(std::move(failure));
  }
}

} // namespace cinder::bytecode
