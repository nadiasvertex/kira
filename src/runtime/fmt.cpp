#include "src/runtime/fmt.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

#include "src/runtime/arena.h"
#include "src/utf8/utf8.h"

namespace {

using kira::runtime::global_arena;

[[nodiscard]] auto alloc_slots(size_t count) -> uint64_t * {
  return static_cast<uint64_t *>(
      global_arena().allocate(count * sizeof(uint64_t)));
}

[[nodiscard]] auto make_str(std::string_view text) -> uint64_t * {
  auto *bytes = static_cast<char *>(global_arena().allocate(text.size()));
  if (!text.empty()) {
    std::memcpy(bytes, text.data(), text.size());
  }
  auto *header = alloc_slots(2);
  header[0] = static_cast<uint64_t>(text.size());
  header[1] = reinterpret_cast<uint64_t>(bytes); // NOLINT
  return header;
}

[[nodiscard]] auto make_box(uint64_t v) -> uint64_t * {
  auto *slots = alloc_slots(1);
  slots[0] = v;
  return slots;
}

[[nodiscard]] auto view_of(const uint64_t *str_header) -> std::string_view {
  const auto *data = reinterpret_cast<const char *>(str_header[1]); // NOLINT
  return {data, static_cast<size_t>(str_header[0])};
}

[[nodiscard]] auto as_double(uint64_t bits) -> double {
  return std::bit_cast<double>(bits);
}

[[nodiscard]] auto tidy_scientific_exponent(std::string_view formatted)
    -> std::string {
  const auto e_pos = formatted.find('e');
  if (e_pos == std::string_view::npos) {
    return std::string(formatted);
  }
  auto out = std::string(formatted.substr(0, e_pos + 1));
  auto exp = formatted.substr(e_pos + 1);
  bool negative = false;
  if (!exp.empty() && (exp.front() == '+' || exp.front() == '-')) {
    negative = exp.front() == '-';
    exp.remove_prefix(1);
  }
  while (exp.size() > 1 && exp.front() == '0') {
    exp.remove_prefix(1);
  }
  if (negative) {
    out.push_back('-');
  }
  out += exp;
  return out;
}

} // namespace

extern "C" {

auto kira_rt_str_concat(uint64_t *a, uint64_t *b) -> uint64_t * {
  auto out = std::string(view_of(a));
  out += view_of(b);
  return make_str(out);
}

auto kira_rt_str_len_scalars(uint64_t *s) -> uint64_t * {
  const auto view = view_of(s);
  size_t count = 0;
  size_t pos = 0;
  while (pos < view.size()) {
    if (!kira::decode_utf8_scalar(view, pos).has_value()) {
      break;
    }
    ++count;
  }
  return make_box(count);
}

auto kira_rt_str_repeat_char(uint64_t *codepoint, uint64_t *count)
    -> uint64_t * {
  std::string one;
  kira::encode_utf8_scalar(static_cast<uint32_t>(codepoint[0]), one);
  std::string out;
  out.reserve(one.size() * static_cast<size_t>(count[0]));
  for (uint64_t i = 0; i < count[0]; ++i) {
    out += one;
  }
  return make_str(out);
}

auto kira_rt_str_truncate_scalars(uint64_t *s, uint64_t *count) -> uint64_t * {
  const auto view = view_of(s);
  const auto n = count[0];
  size_t pos = 0;
  uint64_t seen = 0;
  while (seen < n && pos < view.size()) {
    if (!kira::decode_utf8_scalar(view, pos).has_value()) {
      break;
    }
    ++seen;
  }
  return make_str(view.substr(0, pos));
}

auto kira_rt_fmt_radix_digits(uint64_t *value, uint64_t *radix,
                              uint64_t *uppercase) -> uint64_t * {
  const auto v = value[0];
  const auto r = radix[0];
  const bool upper = uppercase[0] != 0;
  if (v == 0) {
    return make_str("0");
  }
  static constexpr std::string_view lower_digits = "0123456789abcdef";
  static constexpr std::string_view upper_digits = "0123456789ABCDEF";
  const auto &digits = upper ? upper_digits : lower_digits;
  std::string out;
  uint64_t rest = v;
  while (rest > 0) {
    out.push_back(digits[rest % r]);
    rest /= r;
  }
  std::ranges::reverse(out);
  return make_str(out);
}

auto kira_rt_fmt_f64_fixed(uint64_t *value, uint64_t *precision) -> uint64_t * {
  const auto v = as_double(value[0]);
  const auto prec = static_cast<int>(precision[0]);
  std::array<char, 512> buf{};
  const auto result = std::to_chars(buf.data(), buf.data() + buf.size(), v,
                                    std::chars_format::fixed, prec);
  if (result.ec != std::errc{}) {
    return make_str("0");
  }
  return make_str(std::string_view(buf.data(), result.ptr));
}

auto kira_rt_fmt_f64_sci(uint64_t *value, uint64_t *precision,
                         uint64_t *uppercase) -> uint64_t * {
  const auto v = as_double(value[0]);
  const auto prec = static_cast<int>(precision[0]);
  const bool upper = uppercase[0] != 0;
  std::array<char, 512> buf{};
  const auto result = std::to_chars(buf.data(), buf.data() + buf.size(), v,
                                    std::chars_format::scientific, prec);
  if (result.ec != std::errc{}) {
    return make_str("0");
  }
  auto tidy =
      tidy_scientific_exponent(std::string_view(buf.data(), result.ptr));
  if (upper) {
    for (auto &c : tidy) {
      if (c == 'e') {
        c = 'E';
      }
    }
  }
  return make_str(tidy);
}

auto kira_rt_fmt_f64_general(uint64_t *value, uint64_t *precision)
    -> uint64_t * {
  const auto v = as_double(value[0]);
  const auto prec = static_cast<int>(precision[0]);
  std::array<char, 512> buf{};
  const auto result = std::to_chars(buf.data(), buf.data() + buf.size(), v,
                                    std::chars_format::general, prec);
  if (result.ec != std::errc{}) {
    return make_str("0");
  }
  return make_str(
      tidy_scientific_exponent(std::string_view(buf.data(), result.ptr)));
}

auto kira_rt_fmt_char_from_codepoint(uint64_t *codepoint) -> uint64_t * {
  std::string out;
  kira::encode_utf8_scalar(static_cast<uint32_t>(codepoint[0]), out);
  return make_str(out);
}

} // extern "C"
