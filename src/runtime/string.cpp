#include "src/runtime/string.h"

#include <cstring>
#include <optional>
#include <string_view>

#include "src/runtime/allocator.h"
#include "src/runtime/result_out.h"
#include "src/runtime/string_ops.h"

namespace {

[[nodiscard]] auto alloc_slots(size_t count) -> uint64_t * {
  return static_cast<uint64_t *>(cinder_heap_alloc(count * sizeof(uint64_t)));
}

[[nodiscard]] auto make_str(std::string_view text) -> uint64_t * {
  auto *bytes = static_cast<char *>(cinder_heap_alloc(text.size()));
  if (!text.empty()) {
    std::memcpy(bytes, text.data(), text.size());
  }
  auto *header = alloc_slots(2);
  header[0] = static_cast<uint64_t>(text.size());
  header[1] = reinterpret_cast<uint64_t>(bytes); // NOLINT
  return header;
}

/// A 2-slot `find_result { found: bool; pos: usize }` (see
/// `src/std/string.cn`): slot 0 is the found flag, slot 1 the byte offset.
[[nodiscard]] auto make_find_result(std::optional<size_t> hit) -> uint64_t * {
  auto *slots = alloc_slots(2);
  slots[0] = hit.has_value() ? 1U : 0U;
  slots[1] = static_cast<uint64_t>(hit.value_or(0));
  return slots;
}

/// Writes `find_result { found: bool, pos: usize }` into caller storage:
/// the flag is one byte at offset 0 and the position a word at offset 8.
void write_find_result(void *result, std::optional<size_t> hit) {
  namespace out = cinder::runtime::out;
  out::clear(result, out::k_find_result_size);
  out::store<uint8_t>(result, 0, hit.has_value() ? 1U : 0U);
  out::store<uint64_t>(result, out::k_find_pos_offset, hit.value_or(0));
}

[[nodiscard]] auto view_of(const uint64_t *str_header) -> std::string_view {
  const auto *data = reinterpret_cast<const char *>(str_header[1]); // NOLINT
  return {data, static_cast<size_t>(str_header[0])};
}

} // namespace

extern "C" {

auto cinder_rt_str_eq(uint64_t *a, uint64_t *b) -> uint32_t {
  return cinder::runtime::str_equal(view_of(a), view_of(b)) ? 1U : 0U;
}

auto cinder_rt_str_cmp(uint64_t *a, uint64_t *b) -> int32_t {
  return static_cast<int32_t>(
      cinder::runtime::str_compare(view_of(a), view_of(b)));
}

auto cinder_rt_str_find(uint64_t *haystack, uint64_t *needle, uint64_t from)
    -> uint64_t * {
  return make_find_result(cinder::runtime::str_find(
      view_of(haystack), view_of(needle), static_cast<size_t>(from)));
}

auto cinder_rt_str_rfind(uint64_t *haystack, uint64_t *needle) -> uint64_t * {
  return make_find_result(
      cinder::runtime::str_rfind(view_of(haystack), view_of(needle)));
}

auto cinder_rt_str_reverse(uint64_t *s) -> uint64_t * {
  return make_str(cinder::runtime::str_reverse(view_of(s)));
}

auto cinder_rt_str_trim(uint64_t *s, uint32_t mode) -> uint64_t * {
  const auto trim =
      static_cast<cinder::runtime::trim_mode>(static_cast<uint8_t>(mode));
  return make_str(cinder::runtime::str_trim(view_of(s), trim));
}

auto cinder_rt_str_replace(uint64_t *s, uint64_t *from, uint64_t *to)
    -> uint64_t * {
  return make_str(
      cinder::runtime::str_replace(view_of(s), view_of(from), view_of(to)));
}

// The `_into` entry points (see `src/runtime/result_out.h`): the result is
// built in the caller's storage, and arguments are addresses of the caller's
// own `str` descriptors.

void cinder_rt_str_find_into(void *result, const void *haystack,
                             const void *needle, uint64_t from) {
  namespace out = cinder::runtime::out;
  write_find_result(result, cinder::runtime::str_find(
                                out::view_of(haystack), out::view_of(needle),
                                static_cast<size_t>(from)));
}

void cinder_rt_str_rfind_into(void *result, const void *haystack,
                              const void *needle) {
  namespace out = cinder::runtime::out;
  write_find_result(result, cinder::runtime::str_rfind(
                                out::view_of(haystack), out::view_of(needle)));
}

void cinder_rt_str_reverse_into(void *result, const void *s) {
  namespace out = cinder::runtime::out;
  out::store_str(result, 0, cinder::runtime::str_reverse(out::view_of(s)));
}

void cinder_rt_str_trim_into(void *result, const void *s, uint32_t mode) {
  namespace out = cinder::runtime::out;
  const auto trim =
      static_cast<cinder::runtime::trim_mode>(static_cast<uint8_t>(mode));
  out::store_str(result, 0, cinder::runtime::str_trim(out::view_of(s), trim));
}

void cinder_rt_str_replace_into(void *result, const void *s, const void *from,
                                const void *to) {
  namespace out = cinder::runtime::out;
  out::store_str(result, 0,
                 cinder::runtime::str_replace(out::view_of(s),
                                              out::view_of(from),
                                              out::view_of(to)));
}

} // extern "C"
