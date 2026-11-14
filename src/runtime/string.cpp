#include "src/runtime/string.h"

#include <cstring>
#include <optional>
#include <string_view>

#include "src/runtime/allocator.h"
#include "src/runtime/result_out.h"
#include "src/runtime/string_ops.h"

namespace {

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
