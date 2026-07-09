#pragma once

#include <expected>
#include <span>
#include <string>
#include <string_view>

#include "driver.h"

namespace kira::driver {

/// Parse command-line arguments into driver configuration.
///
/// @param argv Argument vector passed to `main`, including the program name at
/// index 0.
/// @return Configuration on success with `show_help` set to true when --help /
/// -h was requested (the caller should print `render_help()` and exit). A
/// diagnostic error message when arguments are invalid.
[[nodiscard]] auto parse_args(std::span<char *const> argv)
    -> std::expected<cli_config, std::string>;

/// Render the user-facing CLI help text.
///
/// @param program_name Executable name to display in the usage line.
[[nodiscard]] auto render_help(std::string_view program_name) -> std::string;

} // namespace kira::driver
