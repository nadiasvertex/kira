#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace cinder::util {

/// The value of an integer literal's spelling — decimal, or `0x`/`0o`/`0b`
/// prefixed, with `_` separators allowed — as a 128-bit unsigned integer.
/// `nullopt` when the spelling is malformed or the value needs more than
/// 128 bits. The one literal parser the checker and both backends share, so
/// they agree on every value up to `uint128`'s maximum.
[[nodiscard]] auto parse_integer_literal_wide(std::string_view text)
    -> std::optional<unsigned __int128>;

/// `value` in decimal — `std::format` has no 128-bit integer support to
/// rely on, and diagnostics name `int128`/`uint128` limits.
[[nodiscard]] auto format_wide_decimal(unsigned __int128 value) -> std::string;

/// `value` in decimal, with a leading `-` when negative.
[[nodiscard]] auto format_wide_signed_decimal(__int128 value) -> std::string;

} // namespace cinder::util
