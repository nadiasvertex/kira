#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "src/hir/nodes.h"
#include "src/hir/storage.h"
#include "src/hir/storage_frame.h"
#include "src/hir/storage_nrvo.h"
#include "src/parser/source_location.h"

namespace cinder::hir {

enum class storage_lower_error_kind : uint8_t {
  /// A construct the storage form does not express yet, such as `match` or a
  /// string literal. The function is refused, never approximated.
  unsupported_construct,
  /// A type used by the function has no layout.
  layout_failed,
  /// The HIR does not have the shape lowering relies on.
  malformed,
  /// The function's frame storage is over the limit (see `plan_frame`). The
  /// message names the largest contributors, and `span` is the largest one's.
  frame_too_large,
};

struct storage_lower_error {
  storage_lower_error_kind kind =
      storage_lower_error_kind::unsupported_construct;
  source_span span;
  std::string message;
};

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
  /// The returned local each function considered for NRVO, and what became
  /// of it.
  std::vector<nrvo_decision> nrvo;
  /// The frame bytes of each function that lowered, after NRVO.
  std::vector<frame_plan> frames;
};

/// Switches for the lowering that tests use to compare forms.
struct storage_lower_options {
  /// Build a returned local in the result storage when it qualifies (see
  /// `apply_nrvo`). Mandatory RVO is not optional.
  bool nrvo = true;
  /// The most frame storage one function may use, in bytes. A function over
  /// it is refused with `frame_too_large`. Tests lower it to reach the check.
  uint64_t max_frame_bytes = k_max_frame_stack_bytes;
};

/// Lowers a module's functions to storage form.
///
/// Constructors (struct, tuple, array, and sum-variant literals) initialize
/// their destination projection by projection, so a nested aggregate is built
/// in its containing bytes. A call whose result is aggregate builds it
/// directly in the destination the caller supplies: the function's own result
/// slot for `return f(...)`, the local being declared for `let x = f(...)`,
/// or the enclosing field for a constructor operand. Aggregate arguments are
/// built in fresh outgoing slots that the call consumes.
///
/// Supports `let`, assignment (compound on scalars), expression statements,
/// `return`, `if`/`elif`/`else`, `while`, `break`, and `continue`, over
/// literals, locals, field and payload projections, unary and binary scalar
/// operators, casts, calls, and constructors. Anything else returns
/// `unsupported_construct` naming it. The result is not verified;
/// call `verify`.
///
/// A lambda becomes a `make_closure` operation plus two functions of the
/// module, named after the function that contains it: the body
/// (`<name>$closure<N>`) and, when the environment is not empty, the drop glue
/// (`<name>$closure<N>$drop`). Both take the environment block as a hidden
/// first operand (`abi_signature::takes_environment`); each captured variable
/// is a `closure_environment` slot of the body whose bytes are in that block.
[[nodiscard]] auto lower_to_storage(const hir_module &module, storage_env &env,
                                    const storage_lower_options &options = {})
    -> storage_lowering;

} // namespace cinder::hir
