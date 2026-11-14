#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "src/hir/abi.h"

namespace cinder::hir {

// ==========================================================================
//  Intrinsic calls in the storage form.
//
//  A call to an `intrinsic def` carries the signature the checker derived from
//  its declaration, like any other call. The runtime's native symbol has a
//  fixed shape (`src/intrinsics.h`), so before a backend emits the call it
//  checks that the two agree: a result that travels through an address needs
//  the `_into` symbol, and every parameter must cross the way the symbol
//  expects. A mismatch is a compiler or runtime bug, and is reported rather
//  than left to corrupt memory.
// ==========================================================================

/// The index of the runtime intrinsic `name` in `known_intrinsics`, when a
/// call to that name is a call to the runtime. Inline intrinsics (the atomics)
/// have no runtime symbol and are not found.
[[nodiscard]] auto runtime_intrinsic_id(std::string_view name)
    -> std::optional<size_t>;

/// Describes why a call with `signature` cannot reach the runtime symbol of
/// intrinsic `id`, or nothing when it can.
///
/// - A result built in caller storage needs an `into` symbol, and an `into`
///   symbol needs such a result.
/// - The number of parameters that occupy an operand must equal the symbol's
///   arity.
/// - A scalar parameter must be 4 or fewer bytes for a 32-bit wire kind and
///   exactly 8 for a 64-bit one. A descriptor, aggregate, reference, address,
///   or handle parameter must cross as a pointer.
[[nodiscard]] auto check_intrinsic_signature(const abi_signature &signature,
                                             size_t id)
    -> std::optional<std::string>;

} // namespace cinder::hir
