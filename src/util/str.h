#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace kira::util {
/// Append text to a diagnostic buffer, inserting a separating newline when
/// needed.
///
/// @param buffer Destination buffer that accumulates rendered diagnostics.
/// @param text Text fragment to append.
auto append_text(std::string &buffer, std::string_view text) -> void;

/// Append one driver error line to the plain-text diagnostic buffer.
///
/// @param buffer Destination buffer that accumulates rendered diagnostics.
/// @param message Error message without the `error:` prefix.
auto append_error(std::string &buffer, std::string_view message) -> void;

/// Join string segments with a caller-provided separator.
///
/// @param parts Ordered string segments to join.
/// @param separator Separator inserted between adjacent segments.
[[nodiscard]] auto join_strings(const std::vector<std::string> &parts,
                                std::string_view separator) -> std::string;

} // namespace kira::util
