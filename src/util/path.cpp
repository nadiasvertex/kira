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
} // namespace kira::util
