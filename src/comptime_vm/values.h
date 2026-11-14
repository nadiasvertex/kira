#pragma once

#include <expected>
#include <string>
#include <vector>

#include "src/bytecode/value.h"
#include "src/comptime/value.h"
#include "src/runtime/value_layout.h"
#include "src/semantic/comptime_backend.h"

namespace cinder::comptime_vm {

// Moves values between the compiler and code it runs on the VM. A
// compile-time value carries no exact type (an integer is just an integer),
// so each conversion is given the type the checker gave it, and builds or
// reads that type's inline bytes (`src/runtime/value_layout.h`), exactly as
// compiled code builds them: a scalar, address or handle travels in a
// register, and only the data behind an address (a `str`'s bytes, a `list`'s
// elements, a reference's referent) is on the heap.
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

/// Writes `value` as the inline bytes of a `type` at `at`,
/// which has room for its layout (`layouts`). Data behind an address (a
/// `str`'s bytes, a `list`'s elements, a reference's referent) is allocated
/// on the heap. A value held by handle is added to `handles`.
[[nodiscard]] auto
write_inline(semantic::comptime_program &program, handle_table &handles,
             runtime::value_layouts &layouts, semantic::type_id type,
             const comptime::value &value, std::byte *at)
    -> std::expected<void, std::string>;

/// The `type` whose inline bytes are at `at`.
[[nodiscard]] auto read_inline(semantic::comptime_program &program,
                               const handle_table &handles,
                               runtime::value_layouts &layouts,
                               semantic::type_id type, const std::byte *at)
    -> std::expected<comptime::value, std::string>;

/// `value` as the register a `type` that travels by value (a
/// scalar, address or handle) is passed in.
[[nodiscard]] auto
to_register(semantic::comptime_program &program, handle_table &handles,
            runtime::value_layouts &layouts, semantic::type_id type,
            const comptime::value &value)
    -> std::expected<bytecode::slot_value, std::string>;

/// The register `slot` holding a `type`, as a compile-time
/// value.
[[nodiscard]] auto
from_register(semantic::comptime_program &program, const handle_table &handles,
              runtime::value_layouts &layouts, semantic::type_id type,
              bytecode::slot_value slot)
    -> std::expected<comptime::value, std::string>;

} // namespace cinder::comptime_vm
