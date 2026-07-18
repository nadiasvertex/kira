#include "src/runtime/string_ops.h"

#include <algorithm>
#include <ranges>
#include <utility>
#include <vector>

#include "src/utf8/utf8.h"

namespace kira::runtime {
namespace {

// -------------------------------------------------------------------------
//  Two-Way (Crochemore–Perrin) substring search over raw bytes.
//
//  Ported from the algorithm glibc's `memmem`/`strstr` use (str-two-way.h):
//  guaranteed O(n + m) worst case with O(1) extra space and no
//  alphabet-sized table, so the linear guarantee holds on every target
//  regardless of the platform `memmem`'s own complexity. The `SIZE_MAX`
//  sentinel arithmetic (`max_suffix + k` wrapping to `k - 1` when
//  `max_suffix == SIZE_MAX`, via `string_view::operator[]`'s unsigned index)
//  is deliberate and mirrors the reference. Bytes are compared as
//  `unsigned char` so the ordering is a stable total order over 0..255.
// -------------------------------------------------------------------------

[[nodiscard]] auto byte_at(std::string_view s, size_t i) -> unsigned char {
  return static_cast<unsigned char>(s[i]);
}

/// Critical factorization: returns the split index (`suffix`) of `needle` and
/// writes the period of its right half to `period`.
[[nodiscard]] auto critical_factorization(std::string_view needle,
                                          size_t &period) -> size_t {
  const size_t needle_len = needle.size();

  // Maximal suffix under `<` ordering.
  size_t max_suffix = SIZE_MAX;
  size_t j = 0;
  size_t k = 1;
  size_t p = 1;
  while (j + k < needle_len) {
    const unsigned char a = byte_at(needle, j + k);
    const unsigned char b = byte_at(needle, max_suffix + k);
    if (a < b) {
      j += k;
      k = 1;
      p = j - max_suffix;
    } else if (a == b) {
      if (k != p) {
        ++k;
      } else {
        j += p;
        k = 1;
      }
    } else { // a > b
      max_suffix = j++;
      k = 1;
      p = 1;
    }
  }
  const size_t period_le = p;

  // Maximal suffix under `>` ordering.
  size_t max_suffix_rev = SIZE_MAX;
  j = 0;
  k = 1;
  p = 1;
  while (j + k < needle_len) {
    const unsigned char a = byte_at(needle, j + k);
    const unsigned char b = byte_at(needle, max_suffix_rev + k);
    if (b < a) {
      j += k;
      k = 1;
      p = j - max_suffix_rev;
    } else if (a == b) {
      if (k != p) {
        ++k;
      } else {
        j += p;
        k = 1;
      }
    } else { // b > a
      max_suffix_rev = j++;
      k = 1;
      p = 1;
    }
  }

  // Longer suffix wins; the `<`-ordering suffix wins ties.
  if (max_suffix_rev + 1 < max_suffix + 1) {
    period = period_le;
    return max_suffix + 1;
  }
  period = p;
  return max_suffix_rev + 1;
}

/// First index of `needle` in `haystack`, or `SIZE_MAX` if absent. `needle`
/// must be non-empty and no longer than `haystack`.
[[nodiscard]] auto two_way(std::string_view haystack, std::string_view needle)
    -> size_t {
  const size_t haystack_len = haystack.size();
  const size_t needle_len = needle.size();
  size_t period = 0;
  const size_t suffix = critical_factorization(needle, period);

  if (needle.substr(0, suffix) == needle.substr(period, suffix)) {
    // Needle is periodic: remember how much of the right half was already
    // matched so repeated periods are not rescanned.
    size_t memory = 0;
    size_t j = 0;
    while (j + needle_len <= haystack_len) {
      size_t i = std::max(suffix, memory);
      while (i < needle_len && byte_at(needle, i) == byte_at(haystack, i + j)) {
        ++i;
      }
      if (i >= needle_len) {
        i = suffix - 1;
        while (i + 1 > memory &&
               byte_at(needle, i) == byte_at(haystack, i + j)) {
          --i;
        }
        if (i + 1 < memory + 1) {
          return j;
        }
        j += period;
        memory = needle_len - period;
      } else {
        j += i - suffix + 1;
        memory = 0;
      }
    }
  } else {
    // Right and left halves are distinct: any mismatch yields a maximal shift.
    const size_t shift = std::max(suffix, needle_len - suffix) + 1;
    size_t j = 0;
    while (j + needle_len <= haystack_len) {
      size_t i = suffix;
      while (i < needle_len && byte_at(needle, i) == byte_at(haystack, i + j)) {
        ++i;
      }
      if (i >= needle_len) {
        i = suffix - 1;
        while (i != SIZE_MAX &&
               byte_at(needle, i) == byte_at(haystack, i + j)) {
          --i;
        }
        if (i == SIZE_MAX) {
          return j;
        }
        j += shift;
      } else {
        j += i - suffix + 1;
      }
    }
  }
  return SIZE_MAX;
}

// -------------------------------------------------------------------------
//  Unicode simple (1:1) case mapping. Contiguous, exception-free ranges only
//  (ASCII, Latin-1, Greek, Cyrillic); Latin Extended and beyond, plus
//  full/special/locale casing, are deferred to the generated-table work in
//  `spec/todo.md`.
// -------------------------------------------------------------------------

[[nodiscard]] auto simple_to_lower(uint32_t c) -> uint32_t {
  if (c <= 0x7F) {
    return (c >= 'A' && c <= 'Z') ? c + 0x20 : c;
  }
  if (c >= 0x00C0 && c <= 0x00DE && c != 0x00D7) { // Latin-1 uppercase
    return c + 0x20;
  }
  if (c == 0x0178) { // Ÿ -> ÿ
    return 0x00FF;
  }
  if (c >= 0x0391 && c <= 0x03A1) { // Greek Α..Ρ
    return c + 0x20;
  }
  if (c >= 0x03A3 && c <= 0x03AB) { // Greek Σ..Ϋ
    return c + 0x20;
  }
  if (c >= 0x0410 && c <= 0x042F) { // Cyrillic А..Я
    return c + 0x20;
  }
  if (c >= 0x0400 && c <= 0x040F) { // Cyrillic Ѐ..Џ
    return c + 0x50;
  }
  return c;
}

[[nodiscard]] auto simple_to_upper(uint32_t c) -> uint32_t {
  if (c <= 0x7F) {
    return (c >= 'a' && c <= 'z') ? c - 0x20 : c;
  }
  if (c == 0x00B5) { // µ (micro sign) -> Μ
    return 0x039C;
  }
  if (c == 0x00FF) { // ÿ -> Ÿ
    return 0x0178;
  }
  if (c >= 0x00E0 && c <= 0x00FE && c != 0x00F7) { // Latin-1 lowercase
    return c - 0x20;
  }
  if (c >= 0x03B1 && c <= 0x03C1) { // Greek α..ρ
    return c - 0x20;
  }
  if (c == 0x03C2) { // final sigma ς -> Σ
    return 0x03A3;
  }
  if (c >= 0x03C3 && c <= 0x03CB) { // Greek σ..ϋ
    return c - 0x20;
  }
  if (c >= 0x0430 && c <= 0x044F) { // Cyrillic а..я
    return c - 0x20;
  }
  if (c >= 0x0450 && c <= 0x045F) { // Cyrillic ѐ..џ
    return c - 0x50;
  }
  return c;
}

[[nodiscard]] auto is_white_space(uint32_t c) -> bool {
  switch (c) {
  case 0x09:
  case 0x0A:
  case 0x0B:
  case 0x0C:
  case 0x0D:
  case 0x20:
  case 0x85:
  case 0xA0:
  case 0x1680:
  case 0x2000:
  case 0x2001:
  case 0x2002:
  case 0x2003:
  case 0x2004:
  case 0x2005:
  case 0x2006:
  case 0x2007:
  case 0x2008:
  case 0x2009:
  case 0x200A:
  case 0x2028:
  case 0x2029:
  case 0x202F:
  case 0x205F:
  case 0x3000:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] auto map_case(std::string_view s, uint32_t (*fold)(uint32_t))
    -> std::string {
  std::string out;
  out.reserve(s.size());
  size_t pos = 0;
  while (pos < s.size()) {
    size_t next = pos;
    const auto scalar = decode_utf8_scalar(s, next);
    if (!scalar.has_value()) { // invalid byte: pass through unchanged
      out.push_back(s[pos]);
      ++pos;
      continue;
    }
    encode_utf8_scalar(fold(*scalar), out);
    pos = next;
  }
  return out;
}

} // namespace

auto str_equal(std::string_view a, std::string_view b) -> bool {
  return a == b;
}

auto str_find(std::string_view haystack, std::string_view needle, size_t from)
    -> std::optional<size_t> {
  if (needle.empty()) {
    return std::min(from, haystack.size());
  }
  if (from >= haystack.size()) {
    return std::nullopt;
  }
  const auto sub = haystack.substr(from);
  if (needle.size() > sub.size()) {
    return std::nullopt;
  }
  const auto hit = two_way(sub, needle);
  if (hit == SIZE_MAX) {
    return std::nullopt;
  }
  return from + hit;
}

auto str_rfind(std::string_view haystack, std::string_view needle)
    -> std::optional<size_t> {
  if (needle.empty()) {
    return haystack.size();
  }
  if (needle.size() > haystack.size()) {
    return std::nullopt;
  }
  // Last match in the original == first match in the byte-reversed inputs.
  const std::string rev_haystack(haystack.rbegin(), haystack.rend());
  const std::string rev_needle(needle.rbegin(), needle.rend());
  const auto hit = two_way(rev_haystack, rev_needle);
  if (hit == SIZE_MAX) {
    return std::nullopt;
  }
  return haystack.size() - hit - needle.size();
}

auto str_to_upper(std::string_view s) -> std::string {
  return map_case(s, simple_to_upper);
}

auto str_to_lower(std::string_view s) -> std::string {
  return map_case(s, simple_to_lower);
}

auto str_reverse(std::string_view s) -> std::string {
  std::vector<std::pair<size_t, size_t>> spans; // (byte offset, byte length)
  size_t pos = 0;
  while (pos < s.size()) {
    size_t next = pos;
    if (decode_utf8_scalar(s, next).has_value()) {
      spans.emplace_back(pos, next - pos);
      pos = next;
    } else { // invalid byte: keep as a lone byte
      spans.emplace_back(pos, size_t{1});
      pos += 1;
    }
  }
  std::string out;
  out.reserve(s.size());
  for (const auto &[offset, length] : std::views::reverse(spans)) {
    out.append(s.substr(offset, length));
  }
  return out;
}

auto str_trim(std::string_view s, trim_mode mode) -> std::string_view {
  size_t start = 0;
  if (mode == trim_mode::both || mode == trim_mode::start) {
    size_t pos = 0;
    while (pos < s.size()) {
      size_t next = pos;
      const auto scalar = decode_utf8_scalar(s, next);
      if (!scalar.has_value() || !is_white_space(*scalar)) {
        break;
      }
      pos = next;
    }
    start = pos;
  }

  size_t end = s.size();
  if (mode == trim_mode::both || mode == trim_mode::end) {
    size_t pos = start;
    size_t last_end = start;
    while (pos < s.size()) {
      size_t next = pos;
      const auto scalar = decode_utf8_scalar(s, next);
      if (!scalar.has_value()) { // invalid tail: keep everything from here
        last_end = s.size();
        break;
      }
      if (!is_white_space(*scalar)) {
        last_end = next;
      }
      pos = next;
    }
    end = last_end;
  }

  return s.substr(start, end - start);
}

auto str_replace(std::string_view s, std::string_view from, std::string_view to)
    -> std::string {
  if (from.empty()) {
    return std::string(s);
  }
  std::string out;
  out.reserve(s.size());
  size_t pos = 0;
  while (true) {
    const auto hit = str_find(s, from, pos);
    if (!hit.has_value()) {
      out.append(s.substr(pos));
      break;
    }
    out.append(s.substr(pos, *hit - pos));
    out.append(to);
    pos = *hit + from.size();
  }
  return out;
}

} // namespace kira::runtime
