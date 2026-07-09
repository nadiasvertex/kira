#pragma once

#include <expected>
#include <filesystem>

namespace kira::util {

/// Convert a filesystem path to the normalized slash-separated form used in
/// reports.
///
/// @param path Filesystem path to normalize.
[[nodiscard]] auto normalize_path(const std::filesystem::path &path)
    -> std::string;

/// Read an entire source file into memory for parsing.
///
/// @param path Source file path to load.
[[nodiscard]] auto read_source_file(const std::filesystem::path &path)
    -> std::expected<std::string, std::string>;

} // namespace kira::util
