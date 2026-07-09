#include "path.h"

namespace kira::util {

/// Convert a filesystem path to the normalized slash-separated form used in
/// reports.
///
/// @param path Filesystem path to normalize.
[[nodiscard]] auto normalize_path(const std::filesystem::path &path)
    -> std::string {
  return path.lexically_normal().generic_string();
}

/// Read an entire source file into memory for parsing.
///
/// @param path Source file path to load.
[[nodiscard]] auto read_source_file(const fs::path &path)
    -> std::expected<std::string, std::string> {
  auto in = std::ifstream(path, std::ios::binary);
  if (!in) {
    return std::unexpected{
        std::format("failed to open `{}`", normalize_path(path))};
  }

  auto buffer = std::ostringstream{};
  buffer << in.rdbuf();
  if (!in.good() && !in.eof()) {
    return std::unexpected{
        std::format("failed while reading `{}`", normalize_path(path))};
  }

  return buffer.str();
}

/// Compute the on-disk metadata path for one compiled module.
///
/// @param metadata_root Root directory configured for metadata output.
/// @param file Parsed AST for the source file.
/// @param source_path Original source file path.
[[nodiscard]] auto metadata_output_path(const fs::path &metadata_root,
                                        const ast::file &file,
                                        const fs::path &source_path)
    -> fs::path {
  auto relative = fs::path{};

  if (file.module_decl != nullptr && !file.module_decl->path.empty()) {
    for (size_t i = 0; i + 1 < file.module_decl->path.size(); ++i) {
      relative /= file.module_decl->path[i];
    }
    relative /=
        file.module_decl->path.back() + std::string(k_metadata_extension);
    return metadata_root / relative;
  }

  relative /=
      source_stem_or_default(source_path) + std::string(k_metadata_extension);
  return metadata_root / relative;
}
} // namespace kira::util
