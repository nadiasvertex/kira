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


    /// Compute the on-disk metadata path for one compiled module.
    ///
    /// @param metadata_root Root directory configured for metadata output.
    /// @param file Parsed AST for the source file.
    /// @param source_path Original source file path.
    [[nodiscard]] auto metadata_output_path(const fs::path &metadata_root,
                                            const ast::file &file,
                                            const fs::path &source_path)
        -> std::filesystem:path ;

} // namespace kira::util
