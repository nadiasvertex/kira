#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kira {

/// Default output directory for serialized module metadata artifacts.
inline constexpr std::string_view kDefaultMetadataDir = "kira-out/module-metadata";

/// Parsed command-line inputs for one `kira` invocation.
struct cli_config {
  std::string program_name; ///< Executable name shown in usage and diagnostics.
  std::vector<std::string> sources; ///< Source file paths to compile in this session.
  std::string metadata_dir = std::string(kDefaultMetadataDir); ///< Output root for metadata files.
  bool show_help = false; ///< True when argument parsing requested help output.
  bool parse_only = false; ///< Skip name resolution and type checking (parser-focused drivers).
};

/// Metadata artifact written for one successfully compiled module file.
struct compiled_module {
  std::string source_path; ///< Normalized source file path that produced the artifact.
  std::vector<std::string> module_path; ///< Canonical module path declared by the source file.
  std::string metadata_path; ///< Serialized metadata file emitted for the module.
};

/// Aggregate result of compiling all requested source files.
struct compile_report {
  std::vector<compiled_module> modules; ///< Metadata artifacts emitted during the session.
  std::string diagnostics; ///< Rendered diagnostics and driver I/O errors.
  uint32_t error_count = 0; ///< Total error count across parsing and driver validation.
};

/// Parse command-line arguments into driver configuration.
///
/// @param argv Argument vector passed to `main`, including the program name at index 0.
[[nodiscard]] auto parse_args(std::span<char *const> argv)
    -> std::expected<cli_config, std::string>;

/// Render the user-facing CLI help text.
///
/// @param program_name Executable name to display in the usage line.
[[nodiscard]] auto render_help(std::string_view program_name) -> std::string;

/// Parse, validate, and emit metadata for each requested source file.
///
/// @param cfg Command-line configuration that names source inputs and outputs.
/// @param use_color Whether rendered diagnostics should include ANSI colors.
[[nodiscard]] auto compile_sources(const cli_config &cfg,
                                   bool use_color = false)
    -> std::expected<compile_report, std::string>;

/// Render a short CLI summary of emitted metadata artifacts and errors.
///
/// @param report Aggregate result of `compile_sources`.
[[nodiscard]] auto render_compile_summary(const compile_report &report)
    -> std::string;

} // namespace kira
