#pragma once

#include <expected>
#include <string>
#include <vector>

#include "src/bytecode/value.h"
#include "src/comptime/value.h"
#include "src/semantic/comptime_backend.h"

namespace cinder::comptime_vm {

// Moves values between the compile-time evaluator and the VM. A compile-time
// value carries no exact type (an integer is just an integer), so each
// conversion is given the type the checker gave it, and builds or reads the
// VM's representation of that type: scalars in the slot itself, everything
// else in heap memory laid out by `src/runtime/layout.h`, exactly as compiled
// code builds it.
//
// Supported: integers, floats, `bool`, `char`, `unit`, `str`, tuples, arrays,
// `list[T]`, structs, user sum types, `option` and `result`, nested in any
// way, and quoted syntax, by handle. Anything else (closures, slices, types)
// has no conversion yet, and the error says which type stopped it.

/// Values that exist only while compiling — quoted syntax — which a VM run
/// holds by handle: handle `n` is `values[n - 1]`. Zero is never a handle.
struct handle_table {
  std::vector<comptime::value> values;
};

/// Whether values of `type` are held by handle: `expr`, `stmt`,
/// `def_expr` and `type_expr`.
[[nodiscard]] auto is_handle_type(const semantic::type_table &types,
                                  semantic::type_id type) -> bool;

/// `value` as a VM value of `type`. A value held by handle is added to
/// `handles`.
[[nodiscard]] auto to_vm(semantic::comptime_program &program,
                         handle_table &handles, semantic::type_id type,
                         const comptime::value &value)
    -> std::expected<bytecode::slot_value, std::string>;

/// The VM value `slot` of `type` as a compile-time value.
[[nodiscard]] auto from_vm(semantic::comptime_program &program,
                           const handle_table &handles, semantic::type_id type,
                           bytecode::slot_value slot)
    -> std::expected<comptime::value, std::string>;

} // namespace cinder::comptime_vm
