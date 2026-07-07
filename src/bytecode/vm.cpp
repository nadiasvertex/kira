#include "src/bytecode/vm.h"

#include <bit>
#include <cstdint>
#include <utility>
#include <vector>

#include "src/runtime/arena.h"
#include "src/runtime/layout.h"

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

      case opcode::op_alloc: {
        const uint8_t dst = code[ip];
        const uint16_t slot_count = read_u16(code, ip + 1);
        auto *raw = kira::runtime::global_arena().allocate(
            static_cast<size_t>(slot_count) * sizeof(slot_value));
        f.registers[dst] = ptr_to_slot(raw);
        f.pc = ip + 3;
        break;
      }
      case opcode::op_load_slot: {
        const uint8_t dst = code[ip];
        const uint8_t ptr_reg = code[ip + 1];
        const uint16_t slot_index = read_u16(code, ip + 2);
        f.registers[dst] = slots_of(f.registers[ptr_reg])[slot_index];
        f.pc = ip + 4;
        break;
      }
      case opcode::op_store_slot: {
        const uint8_t ptr_reg = code[ip];
        const uint16_t slot_index = read_u16(code, ip + 1);
        const uint8_t src = code[ip + 3];
        slots_of(f.registers[ptr_reg])[slot_index] = f.registers[src];
        f.pc = ip + 4;
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
        const auto index = f.registers[index_reg].u;
        f.registers[dst] = slots_of(f.registers[ptr_reg])[index];
        f.pc = ip + 3;
        break;
      }
      case opcode::op_store_indexed: {
        const uint8_t ptr_reg = code[ip];
        const uint8_t index_reg = code[ip + 1];
        const uint8_t src = code[ip + 2];
        const auto index = f.registers[index_reg].u;
        slots_of(f.registers[ptr_reg])[index] = f.registers[src];
        f.pc = ip + 3;
        break;
      }
      case opcode::op_list_push: {
        const uint8_t header_reg = code[ip];
        const uint8_t value_reg = code[ip + 1];
        auto *header = reinterpret_cast<uint64_t *>(
            static_cast<uintptr_t>(f.registers[header_reg].u));
        auto *slot = kira::runtime::list_reserve_slot(header);
        *slot = f.registers[value_reg].u;
        f.pc = ip + 2;
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
      }
    }
  } catch (const panic_error &err) {
    return std::unexpected(err.reason());
  }
}

} // namespace kira::bytecode
