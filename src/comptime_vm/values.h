#pragma once

#include <expected>
#include <string>

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
// way. Anything else (references, closures, slices, compile-time-only values
// such as types and quoted syntax) has no conversion yet, and the error says
// which type stopped it.

/// `value` as a VM value of `type`.
[[nodiscard]] auto to_vm(semantic::comptime_program &program,
                         semantic::type_id type, const comptime::value &value)
    -> std::expected<bytecode::slot_value, std::string>;

/// The VM value `slot` of `type` as a compile-time value.
[[nodiscard]] auto from_vm(semantic::comptime_program &program,
                           semantic::type_id type, bytecode::slot_value slot)
    -> std::expected<comptime::value, std::string>;

} // namespace cinder::comptime_vm
