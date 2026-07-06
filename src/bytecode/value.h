#pragma once

#include <cstdint>
#include <optional>

#include "src/semantic/types.h"

namespace kira::bytecode {

/// Which of the fixed set of scalar runtime representations a bytecode
/// value holds. This is *not* stored per-value (see `slot_value`) — it's an
/// immediate operand baked into the instruction stream at compile time,
/// since every type is fully resolved before lowering ever reaches bytecode
/// (mirrors `spec/typed-ir-design.md` Decision 1). `isize`/`int64` and
/// `usize`/`uint64`/`byte` collapse onto the same kind here (see
/// `numeric_kind_of`) because they have identical runtime representation
/// and behavior — there is nothing left to distinguish once the type has
/// done its job of picking a representation. `int128`/`uint128`/`float128`
/// have no kind here at all: see `numeric_kind_of`'s doc comment for why
/// they're deferred rather than shoehorned in.
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

/// Bit width used for range checks (checked/saturating arithmetic) and for
/// the truncate-and-extend step narrower integer results need after being
/// computed at the uniform 64-bit register width `slot_value` stores
/// everything in. Meaningless (returns 64) for `boolean`, since it isn't a
/// numeric-range type — callers should not call this for non-integer kinds.
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
  }
  return 64;
}

/// Maps a checked, concrete scalar `type_id` to the `numeric_kind` bytecode
/// arithmetic/comparison opcodes are parameterized on. Returns `nullopt` for
/// anything that isn't one of these scalar kinds — including `int128`/
/// `uint128`/`float128`, which this first bytecode-design increment does not
/// support: a `slot_value` is a single 8-byte register-width slot (see its
/// own doc comment), and a 128-bit value needs two, which is a real (if
/// mechanical) extension to the value/frame model this increment doesn't
/// need to solve to cover the scalar/control-flow subset HIR already lowers
/// today (`spec/codegen-design.md` increment 1). Callers should surface
/// `nullopt` as a compile-time "not supported yet" error, the same way
/// `hir::lowering_error_kind::unsupported_construct` already fails closed
/// rather than guessing.
[[nodiscard]] auto numeric_kind_of(const semantic::type_table &types,
                                   semantic::type_id id)
    -> std::optional<numeric_kind>;

/// One bytecode-VM stack/local slot. Deliberately a bare, untagged 8-byte
/// union rather than a tagged variant: every consumer of a `slot_value`
/// (an arithmetic/comparison/cast opcode, `op_get_local`/`op_set_local`)
/// already carries the `numeric_kind` that tells it how to interpret these
/// bits as an immediate operand baked in at compile time (see
/// `numeric_kind`'s own doc comment on why runtime tagging would be
/// redundant). `bool` is `i` 0/1; `char` is `i` holding a widened Unicode
/// scalar value in `u32`'s range. This is a placeholder scalar-only value
/// representation — heap-backed values (`list`/`str`/sum-type payloads
/// wider than one scalar) are explicitly out of scope until
/// `spec/codegen-design.md` increment 6 gives both the bytecode VM and
/// `llvm_codegen` a shared heap-value ABI (Decision 3) at the same time.
union slot_value {
  int64_t i;
  uint64_t u;
  double f;

  constexpr slot_value() noexcept : i(0) {}
  constexpr explicit slot_value(int64_t v) noexcept : i(v) {}
  constexpr explicit slot_value(uint64_t v) noexcept : u(v) {}
  constexpr explicit slot_value(double v) noexcept : f(v) {}
};

} // namespace kira::bytecode
