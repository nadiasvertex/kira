#include "src/parser/text_escape.h"

#include <charconv>

#include "src/utf8/utf8.h"

namespace kira {

auto decode_char_literal(std::string_view text) -> std::optional<uint32_t> {
  if (text.size() < 2 || text.front() != '\'' || text.back() != '\'') {
    return std::nullopt;
  }
  const auto inner = text.substr(1, text.size() - 2);
  if (inner.empty()) {
    return std::nullopt;
  }
  if (inner.front() != '\\') {
    auto pos = size_t{0};
    return decode_utf8_scalar(inner, pos);
  }
  if (inner.size() < 2) {
    return std::nullopt;
  }
  switch (inner[1]) {
  case 'n':
    return static_cast<uint32_t>('\n');
  case 't':
    return static_cast<uint32_t>('\t');
  case 'r':
    return static_cast<uint32_t>('\r');
  case '"':
    return static_cast<uint32_t>('"');
  case '\'':
    return static_cast<uint32_t>('\'');
  case '\\':
    return static_cast<uint32_t>('\\');
  case '{':
    return static_cast<uint32_t>('{');
  case '}':
    return static_cast<uint32_t>('}');
  case 'u': {
    // `\u{XXXX}`.
    if (inner.size() < 5 || inner[2] != '{' || inner.back() != '}') {
      return std::nullopt;
    }
    const auto hex = inner.substr(3, inner.size() - 4);
    auto value = uint32_t{0};
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,bugprone-suspicious-stringview-data-usage)
    const char *hex_end = hex.data() + hex.size();
    const auto result = std::from_chars(hex.data(), hex_end, value, 16);
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,bugprone-suspicious-stringview-data-usage)
    if (result.ec != std::errc{} || result.ptr != hex_end) {
      return std::nullopt;
    }
    return value;
  }
  default:
    return std::nullopt;
  }
}

auto decode_string_body(std::string_view unquoted_text)
    -> std::optional<std::string> {
  const auto inner = unquoted_text;
  auto out = std::string{};
  out.reserve(inner.size());
  size_t pos = 0;
  while (pos < inner.size()) {
    // A doubled brace is a literal single brace (`spec/string-formatting-
    // design.md`: "an interpolation brace is written literally by doubling
    // it") — checked before the backslash-escape switch below since it's
    // not itself a backslash escape. A lone, unescaped `{`/`}` here (from a
    // literal-text segment already split out of an interpolated string) has
    // already had any real `{expr}` removed by the caller, so it can only be
    // a leftover doubled pair.
    if ((inner[pos] == '{' && pos + 1 < inner.size() &&
         inner[pos + 1] == '{') ||
        (inner[pos] == '}' && pos + 1 < inner.size() &&
         inner[pos + 1] == '}')) {
      out.push_back(inner[pos]);
      pos += 2;
      continue;
    }
    if (inner[pos] != '\\') {
      out.push_back(inner[pos]);
      ++pos;
      continue;
    }
    if (pos + 1 >= inner.size()) {
      return std::nullopt;
    }
    switch (inner[pos + 1]) {
    case 'n':
      out.push_back('\n');
      pos += 2;
      break;
    case 't':
      out.push_back('\t');
      pos += 2;
      break;
    case 'r':
      out.push_back('\r');
      pos += 2;
      break;
    case '"':
      out.push_back('"');
      pos += 2;
      break;
    case '\'':
      out.push_back('\'');
      pos += 2;
      break;
    case '\\':
      out.push_back('\\');
      pos += 2;
      break;
    case '{':
      out.push_back('{');
      pos += 2;
      break;
    case '}':
      out.push_back('}');
      pos += 2;
      break;
    case 'u': {
      if (pos + 2 >= inner.size() || inner[pos + 2] != '{') {
        return std::nullopt;
      }
      const auto close = inner.find('}', pos + 3);
      if (close == std::string_view::npos) {
        return std::nullopt;
      }
      const auto hex = inner.substr(pos + 3, close - (pos + 3));
      auto value = uint32_t{0};
      // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,bugprone-suspicious-stringview-data-usage)
      const char *hex_end = hex.data() + hex.size();
      const auto result = std::from_chars(hex.data(), hex_end, value, 16);
      // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,bugprone-suspicious-stringview-data-usage)
      if (result.ec != std::errc{} || result.ptr != hex_end) {
        return std::nullopt;
      }
      encode_utf8_scalar(value, out);
      pos = close + 1;
      break;
    }
    default:
      return std::nullopt;
    }
  }
  return out;
}

auto decode_string_literal(std::string_view text)
    -> std::optional<std::string> {
  if (text.size() < 2 || text.front() != '"' || text.back() != '"') {
    return std::nullopt;
  }
  return decode_string_body(text.substr(1, text.size() - 2));
}

} // namespace kira
