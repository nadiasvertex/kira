#pragma once

#include <algorithm>
#include <array>
#include <string_view>

namespace kira {

// ==========================================================================
//  Shared intrinsic schema.
//
//  This is the single list of recognized `intrinsic def` names. It is the
//  one place that should ever need to change when an intrinsic is added or
//  renamed — the semantic checker validates `intrinsic def` declarations
//  against it, and (per spec/stdlib.md) the bytecode VM's dispatch table and
//  the LLVM codegen's `declare` list should read from it too, so the three
//  cannot drift apart.
//
//  See spec/stdlib.md for the full signature of each intrinsic; this table
//  only tracks names, since the signature itself is written and typechecked
//  as ordinary Kira source at the `intrinsic def` site.
// ==========================================================================
inline constexpr std::array<std::string_view, 8> known_intrinsic_names = {{
    "rt_stdin",
    "rt_stdout",
    "rt_stderr",
    "rt_open",
    "rt_close",
    "rt_read",
    "rt_write",
    "rt_flush",
}};

/// @brief Returns whether `name` is a recognized intrinsic.
[[nodiscard]] inline auto is_known_intrinsic(std::string_view name) noexcept
    -> bool {
  return std::ranges::any_of(known_intrinsic_names,
                             [name](std::string_view known) {
                               return known == name;
                             });
}

} // namespace kira
