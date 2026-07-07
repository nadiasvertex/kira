#include "src/utf8/utf8.h"

namespace kira {

auto decode_utf8_scalar(std::string_view text, size_t &pos)
    -> std::optional<uint32_t> {
  if (pos >= text.size()) {
    return std::nullopt;
  }
  const auto first = static_cast<unsigned char>(text[pos]);
  if ((first & 0x80) == 0) {
    ++pos;
    return static_cast<uint32_t>(first);
  }
  auto extra_bytes = 0;
  auto value = uint32_t{0};
  if ((first & 0xE0) == 0xC0) {
    extra_bytes = 1;
    value = static_cast<uint32_t>(first & 0x1F);
  } else if ((first & 0xF0) == 0xE0) {
    extra_bytes = 2;
    value = static_cast<uint32_t>(first & 0x0F);
  } else if ((first & 0xF8) == 0xF0) {
    extra_bytes = 3;
    value = static_cast<uint32_t>(first & 0x07);
  } else {
    return std::nullopt;
  }
  ++pos;
  for (auto i = 0; i < extra_bytes; ++i) {
    if (pos >= text.size()) {
      return std::nullopt;
    }
    const auto cont = static_cast<unsigned char>(text[pos]);
    value = (value << 6) | static_cast<uint32_t>(cont & 0x3F);
    ++pos;
  }
  return value;
}

auto encode_utf8_scalar(uint32_t scalar, std::string &out) -> void {
  if (scalar < 0x80) {
    out.push_back(static_cast<char>(scalar));
  } else if (scalar < 0x800) {
    out.push_back(static_cast<char>(0xC0U | (scalar >> 6)));
    out.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
  } else if (scalar < 0x10000) {
    out.push_back(static_cast<char>(0xE0U | (scalar >> 12)));
    out.push_back(static_cast<char>(0x80U | ((scalar >> 6) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
  } else {
    out.push_back(static_cast<char>(0xF0U | (scalar >> 18)));
    out.push_back(static_cast<char>(0x80U | ((scalar >> 12) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | ((scalar >> 6) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
  }
}

} // namespace kira
