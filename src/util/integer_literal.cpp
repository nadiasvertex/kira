#include "src/util/integer_literal.h"

#include <cstdint>

namespace cinder::util {

namespace {

[[nodiscard]] auto digit_value(char ch) -> std::optional<unsigned> {
  if (ch >= '0' && ch <= '9') {
    return static_cast<unsigned>(ch - '0');
  }
  if (ch >= 'a' && ch <= 'f') {
    return static_cast<unsigned>(ch - 'a' + 10);
  }
  if (ch >= 'A' && ch <= 'F') {
    return static_cast<unsigned>(ch - 'A' + 10);
  }
  return std::nullopt;
}

} // namespace

auto parse_integer_literal_wide(std::string_view text)
    -> std::optional<unsigned __int128> {
  auto base = 10U;
  if (text.size() > 2 && text[0] == '0') {
    if (text[1] == 'x' || text[1] == 'X') {
      base = 16;
    } else if (text[1] == 'o' || text[1] == 'O') {
      base = 8;
    } else if (text[1] == 'b' || text[1] == 'B') {
      base = 2;
    }
    if (base != 10) {
      text.remove_prefix(2);
    }
  }
  constexpr auto k_max = ~static_cast<unsigned __int128>(0);
  auto value = static_cast<unsigned __int128>(0);
  auto any_digit = false;
  for (const auto ch : text) {
    if (ch == '_') {
      continue;
    }
    const auto digit = digit_value(ch);
    if (!digit.has_value() || *digit >= base) {
      return std::nullopt;
    }
    if (value > (k_max - *digit) / base) {
      return std::nullopt;
    }
    value = value * base + *digit;
    any_digit = true;
  }
  if (!any_digit) {
    return std::nullopt;
  }
  return value;
}

auto format_wide_decimal(unsigned __int128 value) -> std::string {
  if (value == 0) {
    return "0";
  }
  std::string digits;
  while (value != 0) {
    digits.insert(digits.begin(), static_cast<char>('0' + (value % 10)));
    value /= 10;
  }
  return digits;
}

auto format_wide_signed_decimal(__int128 value) -> std::string {
  if (value >= 0) {
    return format_wide_decimal(static_cast<unsigned __int128>(value));
  }
  // Negating in the unsigned domain keeps `int128`'s minimum defined.
  return "-" + format_wide_decimal(-static_cast<unsigned __int128>(value));
}

} // namespace cinder::util
