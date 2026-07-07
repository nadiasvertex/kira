#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace kira {

/// Decodes one UTF-8 scalar value starting at `text[pos]`, advancing `pos`
/// past it. Returns `nullopt` if the sequence is invalid (incomplete or
/// malformed continuation bytes).
[[nodiscard]] auto decode_utf8_scalar(std::string_view text, size_t &pos)
    -> std::optional<uint32_t>;

/// Appends `scalar`'s UTF-8 encoding to `out`. `scalar` must be a valid
/// Unicode scalar value (0x0 to 0x10FFFF, excluding surrogates).
auto encode_utf8_scalar(uint32_t scalar, std::string &out) -> void;

} // namespace kira
