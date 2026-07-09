#include "str.h"

#include <format>

namespace kira::util {

auto append_text(std::string &buffer, std::string_view text) -> void {
  if (text.empty()) {
    return;
  }
  if (!buffer.empty() && buffer.back() != '\n') {
    buffer += '\n';
  }
  buffer += text;
}

auto append_error(std::string &buffer, std::string_view message) -> void {
  append_text(buffer, std::format("error: {}", message));
}

[[nodiscard]] auto join_strings(const std::vector<std::string> &parts,
                                std::string_view separator) -> std::string {
  if (parts.empty()) {
    return {};
  }

  auto out = parts.front();
  for (size_t i = 1; i < parts.size(); ++i) {
    out += separator;
    out += parts[i];
  }
  return out;
}
} // namespace kira::util
