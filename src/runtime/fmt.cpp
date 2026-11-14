#include "src/runtime/fmt.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

#include "src/runtime/allocator.h"
#include "src/runtime/result_out.h"
#include "src/utf8/utf8.h"

namespace {

[[nodiscard]] auto view_of(const uint64_t *str_header) -> std::string_view {
  const auto *data = reinterpret_cast<const char *>(str_header[1]); // NOLINT
  return {data, static_cast<size_t>(str_header[0])};
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

// The text each formatting intrinsic produces.

[[nodiscard]] auto concat_text(std::string_view a, std::string_view b)
    -> std::string {
  auto text = std::string(a);
  text += b;
  return text;
}

[[nodiscard]] auto repeat_text(uint32_t codepoint, uint64_t count)
    -> std::string {
  std::string one;
  cinder::encode_utf8_scalar(codepoint, one);
  std::string text;
  text.reserve(one.size() * static_cast<size_t>(count));
  for (uint64_t i = 0; i < count; ++i) {
    text += one;
  }
  return text;
}

[[nodiscard]] auto truncate_text(std::string_view view, uint64_t count)
    -> std::string_view {
  size_t pos = 0;
  uint64_t seen = 0;
  while (seen < count && pos < view.size()) {
    if (!cinder::decode_utf8_scalar(view, pos).has_value()) {
      break;
    }
    ++seen;
  }
  return view.substr(0, pos);
}

[[nodiscard]] auto radix_text(uint64_t value, uint32_t radix, bool upper)
    -> std::string {
  if (value == 0) {
    return "0";
  }
  static constexpr std::string_view lower_digits = "0123456789abcdef";
  static constexpr std::string_view upper_digits = "0123456789ABCDEF";
  const auto &digits = upper ? upper_digits : lower_digits;
  std::string text;
  uint64_t rest = value;
  while (rest > 0) {
    text.push_back(digits[rest % radix]);
    rest /= radix;
  }
  std::ranges::reverse(text);
  return text;
}

[[nodiscard]] auto fixed_text(double value, uint64_t precision) -> std::string {
  std::array<char, 512> buf{};
  const auto result =
      std::to_chars(buf.data(), buf.data() + buf.size(), value,
                    std::chars_format::fixed, static_cast<int>(precision));
  if (result.ec != std::errc{}) {
    return "0";
  }
  return std::string(buf.data(), result.ptr);
}

[[nodiscard]] auto scientific_text(double value, uint64_t precision, bool upper)
    -> std::string {
  std::array<char, 512> buf{};
  const auto result =
      std::to_chars(buf.data(), buf.data() + buf.size(), value,
                    std::chars_format::scientific, static_cast<int>(precision));
  if (result.ec != std::errc{}) {
    return "0";
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
  return tidy;
}

[[nodiscard]] auto general_text(double value, uint64_t precision)
    -> std::string {
  std::array<char, 512> buf{};
  const auto result =
      std::to_chars(buf.data(), buf.data() + buf.size(), value,
                    std::chars_format::general, static_cast<int>(precision));
  if (result.ec != std::errc{}) {
    return "0";
  }
  return tidy_scientific_exponent(std::string_view(buf.data(), result.ptr));
}

[[nodiscard]] auto codepoint_text(uint32_t codepoint) -> std::string {
  std::string text;
  cinder::encode_utf8_scalar(codepoint, text);
  return text;
}

} // namespace

extern "C" {

auto cinder_rt_str_len_scalars(uint64_t *s) -> uint64_t {
  const auto view = view_of(s);
  size_t count = 0;
  size_t pos = 0;
  while (pos < view.size()) {
    if (!cinder::decode_utf8_scalar(view, pos).has_value()) {
      break;
    }
    ++count;
  }
  return count;
}

// The `_into` entry points (see `src/runtime/result_out.h`): the `str` result
// is a descriptor written into the caller's storage; its bytes are the only
// allocation. A `str` argument is the address of the caller's descriptor.

void cinder_rt_str_concat_into(void *result, const void *a, const void *b) {
  namespace out = cinder::runtime::out;
  out::store_str(result, 0, concat_text(out::view_of(a), out::view_of(b)));
}

void cinder_rt_str_repeat_char_into(void *result, uint32_t codepoint,
                                    uint64_t count) {
  cinder::runtime::out::store_str(result, 0, repeat_text(codepoint, count));
}

void cinder_rt_str_truncate_scalars_into(void *result, const void *s,
                                         uint64_t count) {
  namespace out = cinder::runtime::out;
  out::store_str(result, 0, truncate_text(out::view_of(s), count));
}

void cinder_rt_fmt_radix_digits_into(void *result, uint64_t value,
                                     uint32_t radix, uint32_t uppercase) {
  cinder::runtime::out::store_str(result, 0,
                                  radix_text(value, radix, uppercase != 0));
}

void cinder_rt_fmt_f64_fixed_into(void *result, double value,
                                  uint64_t precision) {
  cinder::runtime::out::store_str(result, 0, fixed_text(value, precision));
}

void cinder_rt_fmt_f64_sci_into(void *result, double value, uint64_t precision,
                                uint32_t uppercase) {
  cinder::runtime::out::store_str(
      result, 0, scientific_text(value, precision, uppercase != 0));
}

void cinder_rt_fmt_f64_general_into(void *result, double value,
                                    uint64_t precision) {
  cinder::runtime::out::store_str(result, 0, general_text(value, precision));
}

void cinder_rt_fmt_char_from_codepoint_into(void *result, uint32_t codepoint) {
  cinder::runtime::out::store_str(result, 0, codepoint_text(codepoint));
}

// Reinterprets a float's bit pattern as an equal-width unsigned integer
// (`spec/todo.md` item 7): `hash` on `float32`/`float64` needs the raw bits,
// and `as uint64`/`as uint32` on a float is a value conversion, not a
// reinterpret. `value` arrives as a genuine native float/double now (the
// wire kind for a `float32`/`float64` intrinsic parameter is the IEEE-754
// value itself, not a boxed bit pattern), so the reinterpret is a plain
// `std::bit_cast`.
auto cinder_rt_bitcast_f64_to_u64(double value) -> uint64_t {
  return std::bit_cast<uint64_t>(value);
}

auto cinder_rt_bitcast_f32_to_u32(float value) -> uint32_t {
  return std::bit_cast<uint32_t>(value);
}

} // extern "C"
