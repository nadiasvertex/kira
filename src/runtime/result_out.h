#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "src/runtime/allocator.h"

namespace cinder::runtime::out {

// ==========================================================================
//  Writing runtime results into caller storage (representation v2).
//
//  An intrinsic marked `into` in `src/intrinsics.h` receives the address of
//  storage the caller owns and writes its whole result there, in the layout
//  of `src/runtime/value_layout.h`. No wrapper block is allocated for the
//  result. A `str` result owns its bytes, which are a separate allocation, as
//  they are for every string; only the `{len, data}` descriptor is inline.
//
//  The shapes below are the ones the runtime produces. `value_layout_test`
//  checks every constant against the layout the compiler computes for the
//  matching declaration in `src/std/intrinsics.cn`, so a change to either
//  side fails a test instead of corrupting a result.
//
//  | Shape                         | Bytes                                   |
//  |-------------------------------|-----------------------------------------|
//  | `str`                         | `{len: u64 @0, data: address @8}`       |
//  | `raw_fd`                      | `{value: i64 @0}`                       |
//  | `io_errno`                    | `{code: i32 @0}`                        |
//  | `find_result`                 | `{found: bool @0, pos: u64 @8}`         |
//  | `result[T, io_errno]`         | `{tag: u64 @0, payload @8}`             |
//
//  A `result` is `ok` when the tag is 0 and `err` when it is 1. The payload
//  of every shape here has an alignment of at most 8, so it starts at the end
//  of the tag. The whole value is rounded up to a multiple of 8. Every byte
//  the function does not set is zero.
// ==========================================================================

inline constexpr size_t k_descriptor_size = 16;
inline constexpr size_t k_descriptor_data_offset = 8;
inline constexpr size_t k_raw_fd_size = 8;
inline constexpr size_t k_io_errno_size = 4;
inline constexpr size_t k_find_result_size = 16;
inline constexpr size_t k_find_pos_offset = 8;
inline constexpr size_t k_result_tag_size = 8;
inline constexpr size_t k_result_payload_offset = k_result_tag_size;
inline constexpr uint64_t k_result_ok_tag = 0;
inline constexpr uint64_t k_result_err_tag = 1;

/// The size of `result[T, io_errno]` for a payload of `payload_size` bytes.
[[nodiscard]] constexpr auto result_size(size_t payload_size) -> size_t {
  const auto widest = std::max(payload_size, k_io_errno_size);
  return (k_result_payload_offset + widest + 7) / 8 * 8;
}

inline constexpr size_t k_result_unit_size = result_size(0);
inline constexpr size_t k_result_word_size = result_size(8);
inline constexpr size_t k_result_str_size = result_size(k_descriptor_size);

/// Stores `value` at `base + offset`; the address need not be aligned.
template <typename T>
inline auto store(void *base, size_t offset, T value) -> void {
  std::memcpy(static_cast<std::byte *>(base) + offset, &value, sizeof(T));
}

/// Zeroes the `size` bytes at `base`, so padding is never left stale.
inline auto clear(void *base, size_t size) -> void {
  std::memset(base, 0, size);
}

/// Writes a `str` descriptor at `base + offset`, copying `text` into a new
/// allocation that the descriptor owns.
inline auto store_str(void *base, size_t offset, std::string_view text)
    -> void {
  auto *bytes = static_cast<char *>(cinder_heap_alloc(text.size()));
  if (!text.empty()) {
    std::memcpy(bytes, text.data(), text.size());
  }
  store<uint64_t>(base, offset, text.size());
  store<uint64_t>(base, offset + k_descriptor_data_offset,
                  reinterpret_cast<uint64_t>(bytes)); // NOLINT
}

/// Reads a `str` or `slice[byte]` descriptor the caller owns.
[[nodiscard]] inline auto view_of(const void *descriptor) -> std::string_view {
  uint64_t length = 0;
  uint64_t data = 0;
  std::memcpy(&length, descriptor, sizeof(length));
  std::memcpy(&data,
              static_cast<const std::byte *>(descriptor) +
                  k_descriptor_data_offset,
              sizeof(data));
  return {reinterpret_cast<const char *>(data), // NOLINT
          static_cast<size_t>(length)};
}

/// Starts an `ok` result of `size` bytes at `result`; the caller then stores
/// the payload at `k_result_payload_offset`.
inline auto begin_ok(void *result, size_t size) -> void {
  clear(result, size);
  store<uint64_t>(result, 0, k_result_ok_tag);
}

/// Writes `err(io_errno { code })` as a result of `size` bytes.
inline auto write_err(void *result, size_t size, int code) -> void {
  clear(result, size);
  store<uint64_t>(result, 0, k_result_err_tag);
  store<int32_t>(result, k_result_payload_offset, static_cast<int32_t>(code));
}

} // namespace cinder::runtime::out
