#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

#include "src/hir/storage.h"
#include "src/parser/source_location.h"

namespace cinder::hir {

/// Which rule a storage form breaks.
enum class verify_error_kind : uint8_t {
  bad_slot,          ///< A slot is malformed or the function's slots disagree
                     ///< with its signature.
  bad_place,         ///< A projection leaves the bounds of its type.
  type_mismatch,     ///< An operation joins places or operands of other types.
  bad_operation,     ///< An operation names fields its kind does not use.
  uninitialized,     ///< A value is read before it is complete.
  already_initialized, ///< A write targets bytes that still hold a value.
  move_from_reference, ///< A move would take a value out of borrowed bytes.
  partial_move,      ///< A move or drop of part of a sum or array.
  not_copy,          ///< A copy of a type that moves.
  incomplete_variant, ///< A sum is published before its payload is ready.
  overlapping_transfer, ///< Source and destination share bytes.
  signature_mismatch, ///< A call disagrees with its callee's ABI.
  bad_argument,      ///< An argument is not what the signature asks for.
  result_aliases_argument, ///< A call's result overlaps a borrowed argument.
  use_after_dead,    ///< A slot is used after its lifetime ended.
  bad_lifetime_end,  ///< `storage_dead` on storage that cannot end here, or
                     ///< an argument slot that still holds a value.
  bad_result,        ///< The result is not complete on a normal return.
  leaked_value,      ///< A value that needs a drop is left behind.
  double_drop,       ///< A value's own drop runs twice.
  use_after_drop,    ///< A value is used after its drop ran, and before it was
                     ///< written again.
  missing_return,    ///< The function can fall off its end.
  code_after_return, ///< An operation follows `return_value`.
  layout_failed,     ///< A type in the form has no layout.
};

[[nodiscard]] auto verify_error_kind_name(verify_error_kind kind) -> const char *;

/// The first rule a form breaks, with where.
struct verify_error {
  verify_error_kind kind = verify_error_kind::bad_operation;
  std::string function;
  /// Index into `ops`, or `npos` for a problem with the slots or signature.
  size_t op_index = static_cast<size_t>(-1);
  source_span span;
  std::string message;
};

struct verify_options {
  /// Report a value that needs a drop and still owes one when its slot ends
  /// or the function returns. A value owes its own drop, and one for each
  /// droppable field it still holds, unless it was moved out.
  bool check_leaks = true;
};

/// Replays the form and reports the first violation. Checks:
///
/// - every projection stays inside its type's layout, and arrives at the
///   type the place claims;
/// - every read sees a complete value, and a sum is readable only after its
///   variant is published;
/// - a copy needs a `copy` type, a move invalidates its source (part by part),
///   and neither takes bytes out of a reference;
/// - a call agrees with its callee's ABI, consumes each argument slot, and
///   builds its result in fresh storage that no borrowed argument overlaps;
/// - no slot is used after `storage_dead`;
/// - a value's own drop runs at most once, nothing touches the value after it
///   until it is written again, and (with `check_leaks`) every value that
///   needs a drop has had it before its slot ends;
/// - a write over a value that needs a drop happens only after that value's
///   drops have run;
/// - on return the result is complete, and nothing follows the return.
[[nodiscard]] auto verify(const storage_function &function, storage_env &env,
                          const storage_module *callees = nullptr,
                          const verify_options &options = {})
    -> std::expected<void, verify_error>;

/// Verifies every function in `module`, using the module for callee lookup.
[[nodiscard]] auto verify(const storage_module &module, storage_env &env,
                          const verify_options &options = {})
    -> std::expected<void, verify_error>;

} // namespace cinder::hir
