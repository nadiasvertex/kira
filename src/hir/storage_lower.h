#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "src/hir/nodes.h"
#include "src/hir/storage.h"
#include "src/parser/source_location.h"

namespace cinder::hir {

enum class storage_lower_error_kind : uint8_t {
  /// A construct the storage form does not express yet, such as control flow
  /// or a string literal. The function is refused, never approximated.
  unsupported_construct,
  /// A type used by the function has no layout.
  layout_failed,
  /// The HIR does not have the shape lowering relies on.
  malformed,
};

struct storage_lower_error {
  storage_lower_error_kind kind = storage_lower_error_kind::unsupported_construct;
  source_span span;
  std::string message;
};

/// Lowers one function to storage form.
///
/// Constructors (struct, tuple, array, and sum-variant literals) initialize
/// their destination projection by projection, so a nested aggregate is built
/// in its containing bytes. A call whose result is aggregate builds it
/// directly in the destination the caller supplies: the function's own result
/// slot for `return f(...)`, the local being declared for `let x = f(...)`,
/// or the enclosing field for a constructor operand. Aggregate arguments are
/// built in fresh outgoing slots that the call consumes.
///
/// Supports straight-line bodies: `let`, assignment, expression statements,
/// and `return`, over literals, locals, field and payload projections, unary
/// and binary scalar operators, casts, calls, and constructors. Anything else
/// returns `unsupported_construct` naming it. The result is not verified;
/// call `verify`.
[[nodiscard]] auto lower_to_storage(const hir_function &function,
                                    storage_env &env)
    -> std::expected<storage_function, storage_lower_error>;

/// A function a module lowering refused, and why.
struct refused_function {
  std::string name;
  storage_lower_error error;
};

/// The outcome of lowering a whole module: the functions that lowered, and
/// the ones that were refused. Nothing is dropped silently.
struct storage_lowering {
  storage_module module;
  std::vector<refused_function> refused;
};

[[nodiscard]] auto lower_to_storage(const hir_module &module, storage_env &env)
    -> storage_lowering;

} // namespace cinder::hir
