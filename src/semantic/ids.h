#pragma once

#include <cstdint>

namespace kira::semantic {

using symbol_id = uint32_t;
using scope_id = uint32_t;

inline constexpr symbol_id k_invalid_symbol_id =
    static_cast<symbol_id>(-1);
inline constexpr scope_id k_invalid_scope_id = static_cast<scope_id>(-1);

} // namespace kira::semantic
