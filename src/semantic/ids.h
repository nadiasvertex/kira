#pragma once

#include <cstdint>

namespace kira::semantic {

/// Stable index into a `semantic_session`'s symbol table.
using symbol_id = uint32_t;
/// Stable index into a `semantic_session`'s scope table.
using scope_id = uint32_t;

/// Sentinel meaning "no symbol" (e.g. an unresolved reference).
inline constexpr symbol_id k_invalid_symbol_id = static_cast<symbol_id>(-1);
/// Sentinel meaning "no scope" (e.g. the parent of a root scope).
inline constexpr scope_id k_invalid_scope_id = static_cast<scope_id>(-1);

} // namespace kira::semantic
