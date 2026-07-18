#include "src/bytecode/vm.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <span>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "src/intrinsics.h"
#include "src/runtime/arena.h"
#include "src/runtime/layout.h"
#include "src/runtime/platform_query.h"
#include "src/runtime/string_ops.h"
#include "src/utf8/utf8.h"

namespace kira::bytecode {

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

[[nodiscard]] auto exec_cast(numeric_kind from_kind, numeric_kind to_kind,
                             slot_value src) -> slot_value {
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
// Heap-value slot access (op_alloc/op_load_slot/op_store_slot) —
// src/runtime/layout.h's flat "N 8-byte slots" representation shared with
// llvm_codegen. A slot_value's u64 field doubles as a pointer here, the
// same untagged-reinterpretation approach numeric_kind already relies on.
// ---------------------------------------------------------------------------

[[nodiscard]] auto slots_of(slot_value ptr) -> slot_value * {
  return reinterpret_cast<slot_value *>(static_cast<uintptr_t>(ptr.u));
}
[[nodiscard]] auto ptr_to_slot(void *raw) -> slot_value {
  return slot_value{static_cast<uint64_t>(reinterpret_cast<uintptr_t>(raw))};
}
[[nodiscard]] auto raw_bytes_of(slot_value ptr) -> uint8_t * {
  return reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(ptr.u));
}

/// Zero-extended `size`-byte (1/2/4/8) read at `data`, the width-aware
/// sibling of the old "always a whole `slot_value`" load — `size` is always
/// one of these four values (`runtime::layout_info::size_bytes` for a
/// scalar, or the fixed 8 every uniform-slot construct still passes). Uses
/// `std::memcpy` rather than a typed `reinterpret_cast` load since a packed
/// struct's byte offset is not generally aligned to `size`.
[[nodiscard]] auto load_sized(const uint8_t *data, uint8_t size) -> uint64_t {
  switch (size) {
  case 1:
    return *data;
  case 2: {
    uint16_t v = 0;
    std::memcpy(&v, data, sizeof(v));
    return v;
  }
  case 4: {
    uint32_t v = 0;
    std::memcpy(&v, data, sizeof(v));
    return v;
  }
  default: {
    uint64_t v = 0;
    std::memcpy(&v, data, sizeof(v));
    return v;
  }
  }
}

/// Writes the low `size` bytes (1/2/4/8) of `value` at `data` — the
/// width-aware sibling of the old "always a whole `slot_value`" store.
auto store_sized(uint8_t *data, uint8_t size, uint64_t value) -> void {
  switch (size) {
  case 1:
    *data = static_cast<uint8_t>(value);
    return;
  case 2: {
    const auto v = static_cast<uint16_t>(value);
    std::memcpy(data, &v, sizeof(v));
    return;
  }
  case 4: {
    const auto v = static_cast<uint32_t>(value);
    std::memcpy(data, &v, sizeof(v));
    return;
  }
  default:
    std::memcpy(data, &value, sizeof(value));
    return;
  }
}

// ---------------------------------------------------------------------------
// Call frames — see vm.h's doc comment for why this is an explicit
// std::vector<frame> rather than C++ call-stack recursion.
// ---------------------------------------------------------------------------

struct frame {
  const bytecode_function *function = nullptr;
  std::vector<slot_value> registers;
  size_t pc = 0;
  uint8_t result_reg = 0;
  bool has_caller = false;
};

auto push_frame(std::vector<frame> &frames, const bytecode_function &fn,
                std::span<const slot_value> args, bool has_caller,
                uint8_t result_reg) -> void {
  if (frames.size() >= k_max_call_depth) {
    throw panic_error(panic_reason::stack_overflow);
  }
  frame f;
  f.function = &fn;
  f.registers.assign(fn.register_count, slot_value{});
  for (size_t i = 0; i < args.size(); ++i) {
    f.registers[i] = args[i];
  }
  f.pc = 0;
  f.has_caller = has_caller;
  f.result_reg = result_reg;
  frames.push_back(std::move(f));
}

// ---------------------------------------------------------------------------
// Intrinsics (src/intrinsics.h). Per spec/stdlib.md, this tier's native
// implementation of every intrinsic is straight C++ standard library/POSIX
// calls — the AOT/LLVM tier instead links against a native runtime library
// implementing the same fixed schema. Every intrinsic here follows the heap
// conventions already established elsewhere in this file: a struct is a
// flat N-slot block in field-declaration order (`op_alloc`/`op_store_slot`),
// a `str`/slice value is a 2-slot `{ len; data_ptr }` header
// (`op_load_str_const`), and — since `runtime::layout.cpp` now gives
// `option`/`result` the same tag/payload-slot convention a user sum type
// gets — a `result[T, io_errno]` value is a 2-slot `{ tag; payload }` block
// with `tag == 0` for `@ok(payload)` and `tag == 1` for `@err(payload)`,
// matching `sum_variant_tag`'s hardcoded order for these two builtins.
// ---------------------------------------------------------------------------

[[nodiscard]] auto alloc_struct(std::span<const slot_value> fields)
    -> slot_value {
  auto *raw = kira::runtime::global_arena().allocate(fields.size() *
                                                     sizeof(slot_value));
  auto *slots = static_cast<slot_value *>(raw);
  for (size_t i = 0; i < fields.size(); ++i) {
    slots[i] = fields[i];
  }
  return ptr_to_slot(raw);
}

/// Reads the `value` field out of a heap `raw_fd { value: int64 }` struct.
[[nodiscard]] auto raw_fd_of(slot_value fd_struct) -> int {
  return static_cast<int>(slots_of(fd_struct)[0].i);
}

/// Allocates a heap `raw_fd { value: int64 }` struct holding `fd`.
[[nodiscard]] auto make_raw_fd(int64_t fd) -> slot_value {
  const auto field = slot_value{fd};
  return alloc_struct(std::span<const slot_value>(&field, 1));
}

/// Decodes a heap `str`/`slice[byte]`/`slice_mut[byte]` `{ len; data_ptr }`
/// header into a raw byte span over its bytes.
[[nodiscard]] auto bytes_of(slot_value header) -> std::span<char> {
  const auto *slots = slots_of(header);
  auto *data = reinterpret_cast<char *>(static_cast<uintptr_t>(slots[1].u));
  return {data, static_cast<size_t>(slots[0].u)};
}

/// Builds `@ok(payload)` as `result[T, io_errno]`'s runtime shape.
[[nodiscard]] auto make_result_ok(slot_value payload) -> slot_value {
  const std::array<slot_value, 2> fields = {slot_value{int64_t{0}}, payload};
  return alloc_struct(fields);
}

/// Builds `@err({ code: errno })` as `result[T, io_errno]`'s runtime shape.
[[nodiscard]] auto make_result_err(int errno_code) -> slot_value {
  const auto code_field = slot_value{int64_t{errno_code}};
  const auto io_errno_struct =
      alloc_struct(std::span<const slot_value>(&code_field, 1));
  const std::array<slot_value, 2> fields = {slot_value{int64_t{1}},
                                            io_errno_struct};
  return alloc_struct(fields);
}

[[nodiscard]] auto intrinsic_rt_stdin(std::span<const slot_value>)
    -> slot_value {
  return make_raw_fd(0);
}
[[nodiscard]] auto intrinsic_rt_stdout(std::span<const slot_value>)
    -> slot_value {
  return make_raw_fd(1);
}
[[nodiscard]] auto intrinsic_rt_stderr(std::span<const slot_value>)
    -> slot_value {
  return make_raw_fd(2);
}

[[nodiscard]] auto intrinsic_rt_open(std::span<const slot_value> args)
    -> slot_value {
  // args[0]: str `{ len; data_ptr }` path.
  // args[1]: `open_options { read; write; append; create; truncate }`.
  const auto path_bytes = bytes_of(args[0]);
  const auto path = std::string(path_bytes.data(), path_bytes.size());
  const auto *opts = slots_of(args[1]);
  const bool want_read = opts[0].u != 0;
  const bool want_write = opts[1].u != 0;
  const bool append = opts[2].u != 0;
  const bool create = opts[3].u != 0;
  const bool truncate = opts[4].u != 0;

  int flags = O_RDONLY;
  if (want_read && want_write) {
    flags = O_RDWR;
  } else if (want_write) {
    flags = O_WRONLY;
  }
  if (append) {
    flags |= O_APPEND;
  }
  if (create) {
    flags |= O_CREAT;
  }
  if (truncate) {
    flags |= O_TRUNC;
  }

  const int fd = ::open(path.c_str(), flags, 0644); // NOLINT
  if (fd < 0) {
    return make_result_err(errno);
  }
  return make_result_ok(make_raw_fd(fd));
}

[[nodiscard]] auto intrinsic_rt_close(std::span<const slot_value> args)
    -> slot_value {
  if (::close(raw_fd_of(args[0])) != 0) {
    return make_result_err(errno);
  }
  return make_result_ok(slot_value{});
}

[[nodiscard]] auto intrinsic_rt_read(std::span<const slot_value> args)
    -> slot_value {
  const auto buf = bytes_of(args[1]);
  const auto n = ::read(raw_fd_of(args[0]), buf.data(), buf.size());
  if (n < 0) {
    return make_result_err(errno);
  }
  return make_result_ok(slot_value{static_cast<uint64_t>(n)});
}

[[nodiscard]] auto intrinsic_rt_write(std::span<const slot_value> args)
    -> slot_value {
  const auto buf = bytes_of(args[1]);
  const auto n = ::write(raw_fd_of(args[0]), buf.data(), buf.size());
  if (n < 0) {
    return make_result_err(errno);
  }
  return make_result_ok(slot_value{static_cast<uint64_t>(n)});
}

[[nodiscard]] auto intrinsic_rt_flush(std::span<const slot_value>)
    -> slot_value {
  // Every read/write above goes straight through the raw ::read/::write
  // syscalls with no userspace buffering layer, so there is nothing for
  // this tier to flush.
  return make_result_ok(slot_value{});
}

// ---------------------------------------------------------------------------
// String-formatting intrinsics (`spec/string-formatting-design.md`). These
// back `std.fmt`'s `pad_str`/`pad_integral` and the builtin-only numeric
// format styles. Every scalar argument/return is boxed in a single-field
// struct (see `src/std/fmt.kira`'s `box_*` types) so it stays
// heap-representable, matching this file's existing struct-is-a-flat-slot-
// block convention rather than special-casing scalar intrinsic arguments.
// ---------------------------------------------------------------------------

[[nodiscard]] auto make_runtime_str(std::string_view text) -> slot_value {
  auto *bytes = kira::runtime::global_arena().allocate(text.size());
  if (!text.empty()) {
    std::memcpy(bytes, text.data(), text.size());
  }
  const std::array<slot_value, 2> fields = {
      slot_value{static_cast<uint64_t>(text.size())}, ptr_to_slot(bytes)};
  return alloc_struct(fields);
}

[[nodiscard]] auto make_box(slot_value v) -> slot_value {
  return alloc_struct(std::span<const slot_value>(&v, 1));
}

[[nodiscard]] auto unbox(slot_value boxed) -> slot_value {
  return slots_of(boxed)[0];
}

[[nodiscard]] auto view_of(slot_value str_header) -> std::string_view {
  const auto bytes = bytes_of(str_header);
  return {bytes.data(), bytes.size()};
}

auto intrinsic_rt_str_concat(std::span<const slot_value> args) -> slot_value {
  auto out = std::string(view_of(args[0]));
  out += view_of(args[1]);
  return make_runtime_str(out);
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
  return make_box(slot_value{static_cast<uint64_t>(count)});
}

auto intrinsic_rt_str_repeat_char(std::span<const slot_value> args)
    -> slot_value {
  const auto codepoint = static_cast<uint32_t>(unbox(args[0]).u);
  const auto n = unbox(args[1]).u;
  std::string one;
  encode_utf8_scalar(codepoint, one);
  std::string out;
  out.reserve(one.size() * static_cast<size_t>(n));
  for (uint64_t i = 0; i < n; ++i) {
    out += one;
  }
  return make_runtime_str(out);
}

auto intrinsic_rt_str_truncate_scalars(std::span<const slot_value> args)
    -> slot_value {
  const auto view = view_of(args[0]);
  const auto n = unbox(args[1]).u;
  size_t pos = 0;
  uint64_t count = 0;
  while (count < n && pos < view.size()) {
    if (!decode_utf8_scalar(view, pos).has_value()) {
      break;
    }
    ++count;
  }
  return make_runtime_str(view.substr(0, pos));
}

// `std.string` UTF-8 intrinsics (spec/std-string.md). The actual algorithms
// live in `src/runtime/string_ops.h` and are shared verbatim with the LLVM
// tier's C-ABI wrappers (`src/runtime/string.cpp`); these handlers only
// marshal the VM's slot representation in and out.

/// A 2-slot `find_result { found: bool; pos: usize }` (see
/// `src/std/string.kira`).
[[nodiscard]] auto make_find_result(std::optional<size_t> hit) -> slot_value {
  const std::array<slot_value, 2> fields = {
      slot_value{static_cast<uint64_t>(hit.has_value() ? 1 : 0)},
      slot_value{static_cast<uint64_t>(hit.value_or(0))}};
  return alloc_struct(fields);
}

auto intrinsic_rt_str_eq(std::span<const slot_value> args) -> slot_value {
  const auto equal =
      kira::runtime::str_equal(view_of(args[0]), view_of(args[1]));
  return make_box(slot_value{static_cast<uint64_t>(equal ? 1 : 0)});
}

auto intrinsic_rt_str_find(std::span<const slot_value> args) -> slot_value {
  return make_find_result(
      kira::runtime::str_find(view_of(args[0]), view_of(args[1]),
                              static_cast<size_t>(unbox(args[2]).u)));
}

auto intrinsic_rt_str_rfind(std::span<const slot_value> args) -> slot_value {
  return make_find_result(
      kira::runtime::str_rfind(view_of(args[0]), view_of(args[1])));
}

auto intrinsic_rt_str_to_upper(std::span<const slot_value> args) -> slot_value {
  return make_runtime_str(kira::runtime::str_to_upper(view_of(args[0])));
}

auto intrinsic_rt_str_to_lower(std::span<const slot_value> args) -> slot_value {
  return make_runtime_str(kira::runtime::str_to_lower(view_of(args[0])));
}

auto intrinsic_rt_str_reverse(std::span<const slot_value> args) -> slot_value {
  return make_runtime_str(kira::runtime::str_reverse(view_of(args[0])));
}

auto intrinsic_rt_str_trim(std::span<const slot_value> args) -> slot_value {
  const auto mode = static_cast<kira::runtime::trim_mode>(
      static_cast<uint8_t>(unbox(args[1]).u));
  return make_runtime_str(kira::runtime::str_trim(view_of(args[0]), mode));
}

auto intrinsic_rt_str_replace(std::span<const slot_value> args) -> slot_value {
  return make_runtime_str(kira::runtime::str_replace(
      view_of(args[0]), view_of(args[1]), view_of(args[2])));
}

auto intrinsic_rt_fmt_radix_digits(std::span<const slot_value> args)
    -> slot_value {
  const auto value = unbox(args[0]).u;
  const auto radix = static_cast<uint64_t>(unbox(args[1]).u);
  const bool uppercase = unbox(args[2]).u != 0;
  if (value == 0) {
    return make_runtime_str("0");
  }
  static constexpr std::string_view lower_digits = "0123456789abcdef";
  static constexpr std::string_view upper_digits = "0123456789ABCDEF";
  const auto &digits = uppercase ? upper_digits : lower_digits;
  std::string out;
  uint64_t v = value;
  while (v > 0) {
    out.push_back(digits[v % radix]);
    v /= radix;
  }
  std::ranges::reverse(out);
  return make_runtime_str(out);
}

/// Rewrites `std::to_chars`' scientific-notation exponent (`e+03`/`e-03`)
/// into the design doc's leaner form (`e3`/`e-3`): no `+` sign, no leading
/// zero padding.
[[nodiscard]] auto tidy_scientific_exponent(std::string_view formatted)
    -> std::string {
  const auto e_pos = formatted.find('e');
  if (e_pos == std::string_view::npos) {
    return std::string(formatted);
  }
  auto out = std::string(formatted.substr(0, e_pos + 1));
  auto exp = formatted.substr(e_pos + 1);
  bool negative = false;
  if (!exp.empty() && (exp.front() == '+' || exp.front() == '-')) {
    negative = exp.front() == '-';
    exp.remove_prefix(1);
  }
  while (exp.size() > 1 && exp.front() == '0') {
    exp.remove_prefix(1);
  }
  if (negative) {
    out.push_back('-');
  }
  out += exp;
  return out;
}

auto intrinsic_rt_fmt_f64_fixed(std::span<const slot_value> args)
    -> slot_value {
  const auto value = unbox(args[0]).f;
  const auto precision = static_cast<int>(unbox(args[1]).u);
  std::array<char, 512> buf{};
  const auto result = std::to_chars(buf.data(), buf.data() + buf.size(), value,
                                    std::chars_format::fixed, precision);
  if (result.ec != std::errc{}) {
    return make_runtime_str("0");
  }
  return make_runtime_str(std::string_view(buf.data(), result.ptr));
}

auto intrinsic_rt_fmt_f64_sci(std::span<const slot_value> args) -> slot_value {
  const auto value = unbox(args[0]).f;
  const auto precision = static_cast<int>(unbox(args[1]).u);
  const bool uppercase = unbox(args[2]).u != 0;
  std::array<char, 512> buf{};
  const auto result = std::to_chars(buf.data(), buf.data() + buf.size(), value,
                                    std::chars_format::scientific, precision);
  if (result.ec != std::errc{}) {
    return make_runtime_str("0");
  }
  auto tidy =
      tidy_scientific_exponent(std::string_view(buf.data(), result.ptr));
  if (uppercase) {
    for (auto &c : tidy) {
      if (c == 'e') {
        c = 'E';
      }
    }
  }
  return make_runtime_str(tidy);
}

auto intrinsic_rt_fmt_f64_general(std::span<const slot_value> args)
    -> slot_value {
  const auto value = unbox(args[0]).f;
  const auto precision = static_cast<int>(unbox(args[1]).u);
  std::array<char, 512> buf{};
  const auto result = std::to_chars(buf.data(), buf.data() + buf.size(), value,
                                    std::chars_format::general, precision);
  if (result.ec != std::errc{}) {
    return make_runtime_str("0");
  }
  return make_runtime_str(
      tidy_scientific_exponent(std::string_view(buf.data(), result.ptr)));
}

auto intrinsic_rt_fmt_char_from_codepoint(std::span<const slot_value> args)
    -> slot_value {
  const auto codepoint = static_cast<uint32_t>(unbox(args[0]).u);
  std::string out;
  encode_utf8_scalar(codepoint, out);
  return make_runtime_str(out);
}

// ---------------------------------------------------------------------------
// `std.platform` runtime intrinsics (spec/std-platform.md). Each queries the
// host through `src/runtime/platform_query.h` (shared with this tier's
// AOT/JIT counterpart, `src/runtime/platform.cpp`, so the OS-specific query
// logic itself isn't duplicated a third time) and encodes the result per
// this file's existing struct/result conventions.
// ---------------------------------------------------------------------------

namespace platform_query = kira::runtime::platform_query;

[[nodiscard]] auto intrinsic_rt_uname(std::span<const slot_value>)
    -> slot_value {
  const auto info = platform_query::query_uname();
  if (!info) {
    return make_result_err(errno);
  }
  const std::array<slot_value, 5> fields = {
      make_runtime_str(info->sysname), make_runtime_str(info->nodename),
      make_runtime_str(info->release), make_runtime_str(info->version),
      make_runtime_str(info->machine)};
  return make_result_ok(alloc_struct(fields));
}

[[nodiscard]] auto intrinsic_rt_gethostname(std::span<const slot_value>)
    -> slot_value {
  const auto name = platform_query::query_hostname();
  if (!name) {
    return make_result_err(errno);
  }
  return make_result_ok(make_runtime_str(*name));
}

[[nodiscard]] auto intrinsic_rt_processor_name(std::span<const slot_value>)
    -> slot_value {
  const auto name = platform_query::query_processor_name();
  if (!name) {
    return make_result_err(errno);
  }
  return make_result_ok(make_runtime_str(*name));
}

[[nodiscard]] auto intrinsic_rt_libc_version(std::span<const slot_value>)
    -> slot_value {
  const auto info = platform_query::query_libc_version();
  if (!info) {
    return make_result_err(errno);
  }
  const std::array<slot_value, 2> fields = {make_runtime_str(info->name),
                                            make_runtime_str(info->version)};
  return make_result_ok(alloc_struct(fields));
}

[[nodiscard]] auto intrinsic_rt_windows_version(std::span<const slot_value>)
    -> slot_value {
  const auto info = platform_query::query_windows_version();
  if (!info) {
    return make_result_err(errno);
  }
  const std::array<slot_value, 5> fields = {
      slot_value{static_cast<uint64_t>(info->major)},
      slot_value{static_cast<uint64_t>(info->minor)},
      slot_value{static_cast<uint64_t>(info->build)},
      slot_value{static_cast<uint64_t>(info->platform_id)},
      make_runtime_str(info->csd_version)};
  return make_result_ok(alloc_struct(fields));
}

[[nodiscard]] auto intrinsic_rt_macos_version(std::span<const slot_value>)
    -> slot_value {
  const auto info = platform_query::query_macos_version();
  if (!info) {
    return make_result_err(errno);
  }
  const std::array<slot_value, 5> fields = {
      make_runtime_str(info->release), make_runtime_str(info->version),
      make_runtime_str(info->dev_stage),
      make_runtime_str(info->non_release_version),
      make_runtime_str(info->machine)};
  return make_result_ok(alloc_struct(fields));
}

using intrinsic_fn = slot_value (*)(std::span<const slot_value>);

/// Indexed by `op_call_intrinsic`'s `intrinsic_id` operand — must stay in
/// the exact order of `kira::known_intrinsic_names` (src/intrinsics.h),
/// which is also the order the semantic checker validated `intrinsic def`
/// names against.
constexpr std::array<intrinsic_fn, 31> k_intrinsics = {{
    intrinsic_rt_stdin,
    intrinsic_rt_stdout,
    intrinsic_rt_stderr,
    intrinsic_rt_open,
    intrinsic_rt_close,
    intrinsic_rt_read,
    intrinsic_rt_write,
    intrinsic_rt_flush,
    intrinsic_rt_str_concat,
    intrinsic_rt_str_len_scalars,
    intrinsic_rt_str_repeat_char,
    intrinsic_rt_str_truncate_scalars,
    intrinsic_rt_fmt_radix_digits,
    intrinsic_rt_fmt_f64_fixed,
    intrinsic_rt_fmt_f64_sci,
    intrinsic_rt_fmt_f64_general,
    intrinsic_rt_fmt_char_from_codepoint,
    intrinsic_rt_str_eq,
    intrinsic_rt_str_find,
    intrinsic_rt_str_rfind,
    intrinsic_rt_str_to_upper,
    intrinsic_rt_str_to_lower,
    intrinsic_rt_str_reverse,
    intrinsic_rt_str_trim,
    intrinsic_rt_str_replace,
    intrinsic_rt_uname,
    intrinsic_rt_gethostname,
    intrinsic_rt_processor_name,
    intrinsic_rt_libc_version,
    intrinsic_rt_windows_version,
    intrinsic_rt_macos_version,
}};

static_assert(k_intrinsics.size() == kira::known_intrinsic_names.size(),
              "every known intrinsic needs exactly one native implementation, "
              "in the same order");

} // namespace

auto vm::run(uint16_t function_index, std::span<const slot_value> args) const
    -> std::expected<vm_result, panic_reason> {
  std::vector<frame> frames;

  try {
    push_frame(frames, module_.functions.at(function_index), args, false, 0);

    while (true) {
      frame &f = frames.back();
      const auto &code = f.function->code;
      const auto op = static_cast<opcode>(code[f.pc]);
      const size_t ip = f.pc + 1;

      switch (op) {
      case opcode::op_load_const: {
        const uint8_t dst = code[ip];
        const uint16_t idx = read_u16(code, ip + 1);
        f.registers[dst] = f.function->constants[idx];
        f.pc = ip + 3;
        break;
      }
      case opcode::op_move: {
        const uint8_t dst = code[ip];
        const uint8_t src = code[ip + 1];
        f.registers[dst] = f.registers[src];
        f.pc = ip + 2;
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
        const uint8_t dst = code[ip];
        const uint8_t lhs = code[ip + 1];
        const uint8_t rhs = code[ip + 2];
        const auto kind = static_cast<numeric_kind>(code[ip + 3]);
        f.registers[dst] =
            dispatch_binary(op, kind, f.registers[lhs], f.registers[rhs]);
        f.pc = ip + 4;
        break;
      }

      case opcode::op_neg:
      case opcode::op_bitnot: {
        const uint8_t dst = code[ip];
        const uint8_t src = code[ip + 1];
        const auto kind = static_cast<numeric_kind>(code[ip + 2]);
        f.registers[dst] = dispatch_unary(op, kind, f.registers[src]);
        f.pc = ip + 3;
        break;
      }

      case opcode::op_not_bool: {
        const uint8_t dst = code[ip];
        const uint8_t src = code[ip + 1];
        f.registers[dst] = store_bool((f.registers[src].u & 1U) == 0U);
        f.pc = ip + 2;
        break;
      }

      case opcode::op_cast: {
        const uint8_t dst = code[ip];
        const uint8_t src = code[ip + 1];
        const auto from_kind = static_cast<numeric_kind>(code[ip + 2]);
        const auto to_kind = static_cast<numeric_kind>(code[ip + 3]);
        f.registers[dst] = exec_cast(from_kind, to_kind, f.registers[src]);
        f.pc = ip + 4;
        break;
      }

      case opcode::op_jump: {
        const int32_t offset = read_i32(code, ip);
        f.pc = static_cast<size_t>(static_cast<int64_t>(ip + 4) + offset);
        break;
      }
      case opcode::op_jump_if_false: {
        const uint8_t cond = code[ip];
        const int32_t offset = read_i32(code, ip + 1);
        const size_t after = ip + 1 + 4;
        const bool value = (f.registers[cond].u & 1U) != 0U;
        f.pc = value
                   ? after
                   : static_cast<size_t>(static_cast<int64_t>(after) + offset);
        break;
      }
      case opcode::op_jump_if_true: {
        const uint8_t cond = code[ip];
        const int32_t offset = read_i32(code, ip + 1);
        const size_t after = ip + 1 + 4;
        const bool value = (f.registers[cond].u & 1U) != 0U;
        f.pc = value ? static_cast<size_t>(static_cast<int64_t>(after) + offset)
                     : after;
        break;
      }

      case opcode::op_call: {
        const uint8_t dst = code[ip];
        const uint16_t fn_idx = read_u16(code, ip + 1);
        const uint8_t first_arg = code[ip + 3];
        const uint8_t argc = code[ip + 4];
        f.pc = ip + 5;
        const std::vector<slot_value> call_args(f.registers.begin() + first_arg,
                                                f.registers.begin() +
                                                    first_arg + argc);
        push_frame(frames, module_.functions.at(fn_idx), call_args, true, dst);
        continue; // `f` is invalidated by push_frame's push_back.
      }
      case opcode::op_return_value: {
        const uint8_t src = code[ip];
        const slot_value result = f.registers[src];
        const bool has_caller = f.has_caller;
        const uint8_t result_reg = f.result_reg;
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

      case opcode::op_call_intrinsic: {
        const uint8_t dst = code[ip];
        const uint8_t intrinsic_id = code[ip + 1];
        const uint8_t first_arg = code[ip + 2];
        const uint8_t argc = code[ip + 3];
        f.pc = ip + 4;
        const std::span<const slot_value> call_args(
            f.registers.data() + first_arg, argc);
        f.registers[dst] = k_intrinsics.at(intrinsic_id)(call_args);
        break;
      }

      case opcode::op_alloc: {
        const uint8_t dst = code[ip];
        const uint16_t byte_size = read_u16(code, ip + 1);
        auto *raw = kira::runtime::global_arena().allocate(byte_size);
        f.registers[dst] = ptr_to_slot(raw);
        f.pc = ip + 3;
        break;
      }
      case opcode::op_load_slot: {
        const uint8_t dst = code[ip];
        const uint8_t ptr_reg = code[ip + 1];
        const uint16_t byte_offset = read_u16(code, ip + 2);
        const uint8_t field_size = code[ip + 4];
        f.registers[dst] = slot_value{load_sized(
            raw_bytes_of(f.registers[ptr_reg]) + byte_offset, field_size)};
        f.pc = ip + 5;
        break;
      }
      case opcode::op_store_slot: {
        const uint8_t ptr_reg = code[ip];
        const uint16_t byte_offset = read_u16(code, ip + 1);
        const uint8_t src = code[ip + 3];
        const uint8_t field_size = code[ip + 4];
        store_sized(raw_bytes_of(f.registers[ptr_reg]) + byte_offset,
                    field_size, f.registers[src].u);
        f.pc = ip + 5;
        break;
      }

      case opcode::op_load_str_const: {
        const uint8_t dst = code[ip];
        const uint16_t idx = read_u16(code, ip + 1);
        const auto &text = f.function->string_constants[idx];
        auto *header =
            kira::runtime::global_arena().allocate(2 * sizeof(slot_value));
        auto *slots = static_cast<slot_value *>(header);
        slots[0] = slot_value{static_cast<uint64_t>(text.size())};
        slots[1] = ptr_to_slot(const_cast<char *>(text.data()));
        f.registers[dst] = ptr_to_slot(header);
        f.pc = ip + 3;
        break;
      }

      case opcode::op_load_indexed: {
        const uint8_t dst = code[ip];
        const uint8_t ptr_reg = code[ip + 1];
        const uint8_t index_reg = code[ip + 2];
        const uint8_t elem_size = code[ip + 3];
        const auto index = f.registers[index_reg].u;
        f.registers[dst] = slot_value{load_sized(
            raw_bytes_of(f.registers[ptr_reg]) + index * elem_size, elem_size)};
        f.pc = ip + 4;
        break;
      }
      case opcode::op_store_indexed: {
        const uint8_t ptr_reg = code[ip];
        const uint8_t index_reg = code[ip + 1];
        const uint8_t src = code[ip + 2];
        const uint8_t elem_size = code[ip + 3];
        const auto index = f.registers[index_reg].u;
        store_sized(raw_bytes_of(f.registers[ptr_reg]) + index * elem_size,
                    elem_size, f.registers[src].u);
        f.pc = ip + 4;
        break;
      }
      case opcode::op_list_push: {
        const uint8_t header_reg = code[ip];
        const uint8_t value_reg = code[ip + 1];
        const uint8_t elem_size = code[ip + 2];
        auto *header = reinterpret_cast<uint64_t *>(
            static_cast<uintptr_t>(f.registers[header_reg].u));
        auto *slot = kira::runtime::list_reserve_slot(header, elem_size);
        store_sized(static_cast<uint8_t *>(slot), elem_size,
                    f.registers[value_reg].u);
        f.pc = ip + 3;
        break;
      }

      case opcode::op_panic_if: {
        const uint8_t cond = code[ip];
        const auto reason = static_cast<panic_reason>(code[ip + 1]);
        if ((f.registers[cond].u & 1U) != 0U) {
          throw panic_error(reason);
        }
        f.pc = ip + 2;
        break;
      }

      case opcode::op_panic:
        throw panic_error(panic_reason::explicit_panic);

      case opcode::op_make_closure: {
        const uint8_t dst = code[ip];
        const uint16_t fn_idx = read_u16(code, ip + 1);
        const uint8_t env_reg = code[ip + 3];
        f.pc = ip + 4;
        auto *header =
            kira::runtime::global_arena().allocate(2 * sizeof(slot_value));
        auto *slots = static_cast<slot_value *>(header);
        slots[0] = slot_value{static_cast<uint64_t>(fn_idx)};
        slots[1] = f.registers[env_reg];
        f.registers[dst] = ptr_to_slot(header);
        break;
      }
      case opcode::op_call_indirect: {
        const uint8_t dst = code[ip];
        const uint8_t closure_reg = code[ip + 1];
        const uint8_t first_arg = code[ip + 2];
        const uint8_t argc = code[ip + 3];
        f.pc = ip + 4;
        const auto *closure = slots_of(f.registers[closure_reg]);
        const auto fn_idx = static_cast<uint16_t>(closure[0].u);
        const auto env_ptr = closure[1];
        std::vector<slot_value> call_args;
        call_args.reserve(static_cast<size_t>(argc) + 1);
        call_args.push_back(env_ptr);
        call_args.insert(call_args.end(), f.registers.begin() + first_arg,
                         f.registers.begin() + first_arg + argc);
        push_frame(frames, module_.functions.at(fn_idx), call_args, true, dst);
        continue; // `f` is invalidated by push_frame's push_back.
      }

      case opcode::op_make_generator: {
        const uint8_t dst = code[ip];
        const uint16_t step_fn_idx = read_u16(code, ip + 1);
        const uint8_t state_ptr_reg = code[ip + 3];
        f.pc = ip + 4;
        auto *header =
            kira::runtime::global_arena().allocate(4 * sizeof(slot_value));
        auto *slots = static_cast<slot_value *>(header);
        slots[0] = slot_value{static_cast<uint64_t>(step_fn_idx)};
        slots[1] = f.registers[state_ptr_reg];
        slots[2] = slot_value{uint64_t{0}}; // resume_index
        slots[3] = slot_value{uint64_t{0}}; // finished
        f.registers[dst] = ptr_to_slot(header);
        break;
      }
      case opcode::op_yield: {
        const uint8_t value_reg = code[ip];
        const uint8_t generator_reg = code[ip + 1];
        const uint8_t next_resume_index = code[ip + 2];
        f.pc = ip + 3;
        // `option::some(value)` — a 2-slot `{ tag=0; payload }` block,
        // matching `option`'s hardcoded variant order (`some`=0, `none`=1;
        // see `runtime::layout.cpp`'s `make_two_variants("some", 1, "none",
        // 0)` and `sum_variant_tag`'s declaration-index tagging).
        auto *header =
            kira::runtime::global_arena().allocate(2 * sizeof(slot_value));
        auto *slots = static_cast<slot_value *>(header);
        slots[0] = slot_value{int64_t{0}};
        slots[1] = f.registers[value_reg];
        const slot_value result = ptr_to_slot(header);

        auto *gen_slots = slots_of(f.registers[generator_reg]);
        gen_slots[2] = slot_value{static_cast<uint64_t>(next_resume_index)};

        const bool has_caller = f.has_caller;
        const uint8_t result_reg = f.result_reg;
        frames.pop_back();
        if (!has_caller) {
          return vm_result{.has_value = true, .value = result};
        }
        frames.back().registers[result_reg] = result;
        continue;
      }
      case opcode::op_generator_next: {
        const uint8_t dst = code[ip];
        const uint8_t generator_reg = code[ip + 1];
        f.pc = ip + 2;
        auto *gen_slots = slots_of(f.registers[generator_reg]);
        if (gen_slots[3].u != 0) {
          // `option::none` — a 2-slot `{ tag=1; payload }` block; the
          // payload slot is never read back for `none`, left zeroed.
          auto *header =
              kira::runtime::global_arena().allocate(2 * sizeof(slot_value));
          auto *slots = static_cast<slot_value *>(header);
          slots[0] = slot_value{int64_t{1}};
          slots[1] = slot_value{};
          f.registers[dst] = ptr_to_slot(header);
          break;
        }
        const auto step_fn_idx = static_cast<uint16_t>(gen_slots[0].u);
        const std::array<slot_value, 3> call_args = {
            gen_slots[1], gen_slots[2], f.registers[generator_reg]};
        push_frame(frames, module_.functions.at(step_fn_idx), call_args, true,
                   dst);
        continue; // `f` is invalidated by push_frame's push_back.
      }
      }
    }
  } catch (const panic_error &err) {
    return std::unexpected(err.reason());
  }
}

} // namespace kira::bytecode
