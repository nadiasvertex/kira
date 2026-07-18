#include "src/runtime/string.h"

#include <cstring>
#include <optional>
#include <string_view>

#include "src/runtime/arena.h"
#include "src/runtime/string_ops.h"

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

/// A 2-slot `find_result { found: bool; pos: usize }` (see
/// `src/std/string.kira`): slot 0 is the found flag, slot 1 the byte offset.
[[nodiscard]] auto make_find_result(std::optional<size_t> hit) -> uint64_t * {
  auto *slots = alloc_slots(2);
  slots[0] = hit.has_value() ? 1U : 0U;
  slots[1] = static_cast<uint64_t>(hit.value_or(0));
  return slots;
}

[[nodiscard]] auto view_of(const uint64_t *str_header) -> std::string_view {
  const auto *data = reinterpret_cast<const char *>(str_header[1]); // NOLINT
  return {data, static_cast<size_t>(str_header[0])};
}

} // namespace

extern "C" {

auto kira_rt_str_eq(uint64_t *a, uint64_t *b) -> uint64_t * {
  return make_box(kira::runtime::str_equal(view_of(a), view_of(b)) ? 1U : 0U);
}

auto kira_rt_str_find(uint64_t *haystack, uint64_t *needle, uint64_t *from)
    -> uint64_t * {
  return make_find_result(kira::runtime::str_find(
      view_of(haystack), view_of(needle), static_cast<size_t>(from[0])));
}

auto kira_rt_str_rfind(uint64_t *haystack, uint64_t *needle) -> uint64_t * {
  return make_find_result(
      kira::runtime::str_rfind(view_of(haystack), view_of(needle)));
}

auto kira_rt_str_to_upper(uint64_t *s) -> uint64_t * {
  return make_str(kira::runtime::str_to_upper(view_of(s)));
}

auto kira_rt_str_to_lower(uint64_t *s) -> uint64_t * {
  return make_str(kira::runtime::str_to_lower(view_of(s)));
}

auto kira_rt_str_reverse(uint64_t *s) -> uint64_t * {
  return make_str(kira::runtime::str_reverse(view_of(s)));
}

auto kira_rt_str_trim(uint64_t *s, uint64_t *mode) -> uint64_t * {
  const auto trim =
      static_cast<kira::runtime::trim_mode>(static_cast<uint8_t>(mode[0]));
  return make_str(kira::runtime::str_trim(view_of(s), trim));
}

auto kira_rt_str_replace(uint64_t *s, uint64_t *from, uint64_t *to)
    -> uint64_t * {
  return make_str(
      kira::runtime::str_replace(view_of(s), view_of(from), view_of(to)));
}

} // extern "C"
