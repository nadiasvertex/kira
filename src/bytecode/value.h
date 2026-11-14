#pragma once

#include <bit>
#include <cstdint>
#include <optional>

#include "src/semantic/type_table.h"

namespace cinder::bytecode {

/// Which of the fixed set of scalar runtime representations a bytecode
/// value holds. This is *not* stored per-value (see `slot_value`) — it's an
/// immediate operand baked into the instruction stream at compile time,
/// since every type is fully resolved before lowering ever reaches bytecode
/// (every type is concrete once HIR is built). `isize`/`int64` and
/// `usize`/`uint64`/`byte` collapse onto the same kind here (see
/// `numeric_kind_of`) because they have identical runtime representation
/// and behavior — there is nothing left to distinguish once the type has
/// done its job of picking a representation. `float128` has no kind here at
/// all: see `numeric_kind_of`'s doc comment.
enum class numeric_kind : uint8_t {
  i8,
  i16,
  i32,
  i64, ///< Also used for `isize`.
  u8,  ///< Also used for `byte`.
  u16,
  u32,
  u64, ///< Also used for `usize`.
  f32,
  f64,
  boolean,
  character, ///< `char` — stored widened, same as `u32`.
  i128,
  u128,
};

[[nodiscard]] constexpr auto is_integer(numeric_kind k) noexcept -> bool {
  switch (k) {
  case numeric_kind::i8:
  case numeric_kind::i16:
  case numeric_kind::i32:
  case numeric_kind::i64:
  case numeric_kind::u8:
  case numeric_kind::u16:
  case numeric_kind::u32:
  case numeric_kind::u64:
  case numeric_kind::i128:
  case numeric_kind::u128:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] constexpr auto is_signed_integer(numeric_kind k) noexcept
    -> bool {
  switch (k) {
  case numeric_kind::i8:
  case numeric_kind::i16:
  case numeric_kind::i32:
  case numeric_kind::i64:
  case numeric_kind::i128:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] constexpr auto is_unsigned_integer(numeric_kind k) noexcept
    -> bool {
  return is_integer(k) && !is_signed_integer(k);
}

[[nodiscard]] constexpr auto is_float(numeric_kind k) noexcept -> bool {
  return k == numeric_kind::f32 || k == numeric_kind::f64;
}

/// Whether `k` is one of the two 128-bit integer kinds — the only kinds
/// whose value does not fit in the low 64 bits of a `slot_value`.
[[nodiscard]] constexpr auto is_wide_integer(numeric_kind k) noexcept -> bool {
  return k == numeric_kind::i128 || k == numeric_kind::u128;
}

/// Bit width used for range checks (checked/saturating arithmetic) and for
/// the truncate-and-extend step narrower integer results need after being
/// computed at the 64-bit width every non-128-bit kind is evaluated at.
/// Meaningless (returns 64) for `boolean`, since it isn't a numeric-range type
/// — callers should not call this for non-integer kinds.
[[nodiscard]] constexpr auto bit_width(numeric_kind k) noexcept -> int {
  switch (k) {
  case numeric_kind::i8:
  case numeric_kind::u8:
    return 8;
  case numeric_kind::i16:
  case numeric_kind::u16:
    return 16;
  case numeric_kind::i32:
  case numeric_kind::u32:
  case numeric_kind::character:
    return 32;
  case numeric_kind::i64:
  case numeric_kind::u64:
    return 64;
  case numeric_kind::f32:
    return 32;
  case numeric_kind::f64:
    return 64;
  case numeric_kind::boolean:
    return 8;
  case numeric_kind::i128:
  case numeric_kind::u128:
    return 128;
  }
  return 64;
}

/// Maps a checked, concrete scalar `type_id` to the `numeric_kind` bytecode
/// arithmetic/comparison opcodes are parameterized on. Returns `nullopt` for
/// anything that isn't one of these scalar kinds. That includes `float128`,
/// which is not supported yet: neither tier has a quad-precision
/// floating-point implementation (Apple arm64 has no 128-bit `long double`
/// and its compiler runtime ships no soft-float routines for one). Callers
/// surface `nullopt` as a compile-time "not supported yet" error rather than
/// guessing a representation. Non-scalar types are not scalars at all; they
/// live in heap memory (see `slot_value`).
[[nodiscard]] auto numeric_kind_of(const semantic::type_table &types,
                                   semantic::type_id id)
    -> std::optional<numeric_kind>;

/// One bytecode-VM register. Deliberately a bare, untagged 16-byte union
/// rather than a tagged variant: every consumer of a `slot_value`
/// (an arithmetic/comparison/cast opcode, `op_get_local`/`op_set_local`)
/// already carries the `numeric_kind` that tells it how to interpret these
/// bits as an immediate operand baked in at compile time (see
/// `numeric_kind`'s own doc comment on why runtime tagging would be
/// redundant). `bool` is `i` 0/1; `char` is `i` holding a widened Unicode
/// scalar value in `u32`'s range. A compound value never lives in a
/// register: it lives in frame, global or heap bytes laid out by
/// `runtime::value_layouts` (the same layout `llvm_codegen` uses), and is
/// reached by address.
///
/// A register is 16 bytes so that an `int128`/`uint128` fits in one (`w`).
/// Every narrower kind lives in the low 8 bytes (`i`/`u`/`f`, which alias
/// the low half of `w` on the little-endian hosts this compiler targets),
/// and every constructor writes all 16 bytes so the high half is never
/// stale. Memory is *not* made of `slot_value`s.
union slot_value {
  int64_t i;
  uint64_t u;
  double f;
  unsigned __int128 w;

  constexpr slot_value() noexcept : w(0) {}
  constexpr explicit slot_value(int64_t v) noexcept
      : w(static_cast<unsigned __int128>(static_cast<__int128>(v))) {}
  constexpr explicit slot_value(uint64_t v) noexcept : w(v) {}
  constexpr explicit slot_value(double v) noexcept
      : w(std::bit_cast<uint64_t>(v)) {}
  constexpr explicit slot_value(unsigned __int128 v) noexcept : w(v) {}
  constexpr explicit slot_value(__int128 v) noexcept
      : w(static_cast<unsigned __int128>(v)) {}
};

static_assert(sizeof(slot_value) == 16);

/// One 8-byte word of a runtime-owned object the VM reads by word: a
/// coroutine or generator object (`src/runtime/coroutine.h`), or the
/// `{ len, data }` words of a descriptor.
union heap_word {
  int64_t i;
  uint64_t u;
  double f;

  constexpr heap_word() noexcept : u(0) {}
  constexpr explicit heap_word(uint64_t v) noexcept : u(v) {}
  /// The low 8 bytes of `v` — what a uniform heap slot holds for every
  /// value narrower than 128 bits.
  constexpr explicit heap_word(slot_value v) noexcept : u(v.u) {}
  [[nodiscard]] constexpr auto as_slot() const noexcept -> slot_value {
    return slot_value{u};
  }
};

static_assert(sizeof(heap_word) == 8);

} // namespace cinder::bytecode
