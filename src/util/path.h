#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>

namespace kira::util {

/// Convert a filesystem path to the normalized slash-separated form used in
/// reports.
///
/// @param path Filesystem path to normalize.
[[nodiscard]] auto normalize_path(const std::filesystem::path &path)
    -> std::string;

/// Resolve the canonical, symlink-free path to the currently running
/// executable, independent of how it was invoked (bare name found via
/// `$PATH`, a relative path, `argv[0]` mangled by the caller, ...). Used to
/// locate a distributed binary's own install prefix so it can find sibling
/// support files (stdlib sources, AOT link archives) without depending on
/// the process's working directory. Returns `std::nullopt` on platforms
/// without a supported self-lookup or if the lookup fails.
[[nodiscard]] auto resolve_self_executable()
    -> std::optional<std::filesystem::path>;

/// Read an entire source file into memory for parsing.
///
/// @param path Source file path to load.
[[nodiscard]] auto read_source_file(const std::filesystem::path &path)
    -> std::expected<std::string, std::string>;

} // namespace kira::util
