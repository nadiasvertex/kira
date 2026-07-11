#pragma once

#include <cstdint>
#include <string>

namespace kira::comptime {

/// Which shape a `value` currently holds. Deliberately a small, closed set
/// for this first milestone — no structs/lists/closures/quote-fragment
/// values yet (those land in later milestones of the compile-time
/// evaluation subsystem; see the design plan's M2/M3 split).
enum class value_kind : uint8_t {
  unit,
  boolean,
  integer,
  floating,
  string,
  /// Sentinel meaning "evaluation already failed and a diagnostic was
  /// already reported" — mirrors `k_unknown_type`'s role in the type
  /// checker (`src/semantic/types.h`): once emitted, it propagates through
  /// the rest of the expression tree so one root cause doesn't cascade
  /// into a wall of follow-on diagnostics.
  diagnostic_marker,
};

/// A compile-time value produced by `evaluator`. Integers are held as a
/// signed 64-bit value regardless of the expression's declared Kira width
/// — by the time the evaluator runs, `checker::infer_expr` has already
/// validated the expression against its declared type, so the evaluator
/// itself doesn't need to re-derive or enforce width/signedness; it only
/// needs a value.
struct value {
  value_kind kind = value_kind::unit;
  bool boolean = false;
  int64_t integer = 0;
  double floating = 0.0;
  std::string string;

  [[nodiscard]] static auto make_unit() -> value {
    return value{.kind = value_kind::unit};
  }
  [[nodiscard]] static auto make_bool(bool v) -> value {
    return value{.kind = value_kind::boolean, .boolean = v};
  }
  [[nodiscard]] static auto make_int(int64_t v) -> value {
    return value{.kind = value_kind::integer, .integer = v};
  }
  [[nodiscard]] static auto make_float(double v) -> value {
    return value{.kind = value_kind::floating, .floating = v};
  }
  [[nodiscard]] static auto make_string(std::string v) -> value {
    return value{.kind = value_kind::string, .string = std::move(v)};
  }
  [[nodiscard]] static auto make_error() -> value {
    return value{.kind = value_kind::diagnostic_marker};
  }

  [[nodiscard]] auto is_error() const noexcept -> bool {
    return kind == value_kind::diagnostic_marker;
  }

  /// Truthiness for use as a `static assert`/`static if` condition; only a
  /// real `boolean` value is meaningful (checking already required
  /// `bool`-ness, so anything else here means an upstream error already
  /// happened).
  [[nodiscard]] auto is_true() const noexcept -> bool {
    return kind == value_kind::boolean && boolean;
  }
};

} // namespace kira::comptime
