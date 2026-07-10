#include "path.h"

#include <cstdint>
#include <format>
#include <fstream>
#include <sstream>
#include <system_error>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace kira::util {

/// Convert a filesystem path to the normalized slash-separated form used in
/// reports.
///
/// @param path Filesystem path to normalize.
[[nodiscard]] auto normalize_path(const std::filesystem::path &path)
    -> std::string {
  return path.lexically_normal().generic_string();
}

/// Resolve the canonical, symlink-free path to the currently running
/// executable, independent of how it was invoked (bare name found via
/// `$PATH`, a relative path, `argv[0]` mangled by the caller, ...). Used to
/// locate a distributed binary's own install prefix so it can find sibling
/// support files (stdlib sources, AOT link archives) without depending on
/// the process's working directory. Returns `std::nullopt` on platforms
/// without a supported self-lookup or if the lookup fails.
[[nodiscard]] auto resolve_self_executable()
    -> std::optional<std::filesystem::path> {
#if defined(__linux__)
  auto ec = std::error_code{};
  const auto target = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec) {
    return std::nullopt;
  }
  auto canonical = std::filesystem::canonical(target, ec);
  if (ec) {
    return std::nullopt;
  }
  return canonical;
#elif defined(__APPLE__)
  auto size = std::uint32_t{0};
  // First call always fails when `size` is too small; it also writes the
  // required buffer size (including the trailing NUL) back into `size`.
  static_cast<void>(_NSGetExecutablePath(nullptr, &size));
  auto buffer = std::string(size, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    return std::nullopt;
  }
  auto ec = std::error_code{};
  auto canonical =
      std::filesystem::canonical(std::filesystem::path(buffer.c_str()), ec);
  if (ec) {
    return std::nullopt;
  }
  return canonical;
#else
  return std::nullopt;
#endif
}

/// Read an entire source file into memory for parsing.
///
/// @param path Source file path to load.
[[nodiscard]] auto read_source_file(const std::filesystem::path &path)
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
