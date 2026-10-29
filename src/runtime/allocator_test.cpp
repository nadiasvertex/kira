#include "src/runtime/allocator.h"

#include <cstddef>
#include <cstring>
#include <thread>
#include <vector>

#include "src/testing/test_assert.h"

namespace {

using cinder::testing::expect;

/// Whether every byte in `[begin, begin + count)` is zero.
[[nodiscard]] auto all_zero(const void *begin, size_t count) -> bool {
  const auto *bytes = static_cast<const unsigned char *>(begin);
  for (size_t i = 0; i < count; ++i) {
    if (bytes[i] != 0) { // NOLINT
      return false;
    }
  }
  return true;
}

auto fill(void *begin, size_t count, unsigned char value) -> void {
  std::memset(begin, value, count);
}

auto test_alloc_returns_zeroed_aligned_memory() -> void {
  auto *p = cinder_heap_alloc(64);
  expect(p != nullptr, "expected cinder_heap_alloc to return non-null memory");
  expect(reinterpret_cast<uintptr_t>(p) % 8 == 0, // NOLINT
         "expected an 8-byte-aligned block");
  expect(all_zero(p, 64), "expected a freshly allocated block to be zeroed");
  cinder_heap_free(p, 64);
}

auto test_alloc_of_zero_bytes_is_null() -> void {
  expect(cinder_heap_alloc(0) == nullptr,
         "expected a zero-byte request to return null");
}

/// The guarantee `allocator.h` documents: the *grown tail* of a realloc is
/// zeroed, not merely the freshly-mapped pages a growing allocation happens
/// to land on.
///
/// Written so it cannot pass by accident. An earlier version of this check
/// grew a block and looked for garbage in the tail, and it passed with the
/// zero-fill deliberately removed — the platform allocator had handed back
/// fresh, already-zero pages, so the assertion never exercised the code it
/// named. This version dirties the block itself and then reallocs with an
/// `old_bytes` of 16, so bytes 16..64 are ones this process wrote and the
/// zeroing is the only thing that can have cleared them. Removing the
/// `memset` in `cinder_heap_realloc` makes this fail on any allocator.
auto test_realloc_zeroes_the_grown_tail() -> void {
  auto *block = cinder_heap_alloc(64);
  expect(block != nullptr, "expected the initial allocation to succeed");
  fill(block, 64, 0xAB);

  // Claim the block was only 16 bytes: the realloc must then treat 16..64 as
  // growth and zero it, even though those bytes are physically unchanged.
  auto *grown = cinder_heap_realloc(block, 16, 64);
  expect(grown != nullptr, "expected the realloc to succeed");
  expect(!all_zero(grown, 16),
         "expected the preserved prefix to survive the realloc");
  expect(all_zero(static_cast<std::byte *>(grown) + 16, 48), // NOLINT
         "expected the grown tail of a realloc to be zeroed");
  cinder_heap_free(grown, 64);
}

auto test_realloc_preserves_the_prefix() -> void {
  auto *block = cinder_heap_alloc(16);
  fill(block, 16, 0x5C);
  auto *grown = cinder_heap_realloc(block, 16, 128);
  const auto *bytes = static_cast<const unsigned char *>(grown);
  for (size_t i = 0; i < 16; ++i) {
    expect(bytes[i] == 0x5C, // NOLINT
           "expected the first 16 bytes to survive a growing realloc");
  }
  cinder_heap_free(grown, 128);
}

auto test_realloc_to_zero_frees() -> void {
  auto *block = cinder_heap_alloc(32);
  expect(cinder_heap_realloc(block, 32, 0) == nullptr,
         "expected a realloc to zero bytes to free and return null");
}

auto test_realloc_of_null_allocates() -> void {
  auto *block = cinder_heap_realloc(nullptr, 0, 32);
  expect(block != nullptr, "expected a realloc of null to allocate");
  expect(all_zero(block, 32), "expected that allocation to be zeroed");
  cinder_heap_free(block, 32);
}

auto test_free_of_null_is_a_no_op() -> void {
  cinder_heap_free(nullptr, 0); // must not crash
}

/// Sizes are rounded up to the 8-byte slot granularity, so a request that
/// differs only below that boundary is the same block.
auto test_sub_slot_growth_is_not_a_reallocation() -> void {
  auto *block = cinder_heap_alloc(8);
  expect(cinder_heap_realloc(block, 8, 7) == block,
         "expected a resize within one 8-byte slot to keep the same block");
  cinder_heap_free(block, 8);
}

/// Several threads allocate, grow and free blocks at once, each checking that
/// the bytes it wrote into its own blocks are the bytes it reads back. Two
/// threads handed overlapping memory would corrupt each other's pattern.
auto test_concurrent_allocation_does_not_overlap() -> void {
  constexpr size_t thread_count = 8;
  constexpr size_t rounds = 2000;
  auto failures = std::vector<bool>(thread_count, false);
  auto threads = std::vector<std::thread>{};
  for (size_t id = 0; id < thread_count; ++id) {
    threads.emplace_back([id, &failures] {
      const auto pattern = static_cast<unsigned char>(id + 1);
      for (size_t round = 0; round < rounds; ++round) {
        auto *block = cinder_heap_alloc(64);
        fill(block, 64, pattern);
        block = cinder_heap_realloc(block, 64, 256);
        auto *bytes = static_cast<const unsigned char *>(block);
        for (size_t i = 0; i < 64; ++i) {
          if (bytes[i] != pattern) { // NOLINT
            failures[id] = true;
          }
        }
        if (!all_zero(bytes + 64, 192)) { // NOLINT
          failures[id] = true;
        }
        cinder_heap_free(block, 256);
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  for (const auto failed : failures) {
    expect(!failed, "expected every thread to read back what it wrote");
  }
}

} // namespace

auto main() -> int {
  try {
    test_alloc_returns_zeroed_aligned_memory();
    test_alloc_of_zero_bytes_is_null();
    test_realloc_zeroes_the_grown_tail();
    test_realloc_preserves_the_prefix();
    test_realloc_to_zero_frees();
    test_realloc_of_null_allocates();
    test_free_of_null_is_a_no_op();
    test_sub_slot_growth_is_not_a_reallocation();
    test_concurrent_allocation_does_not_overlap();
  } catch (const std::exception &e) {
    cinder::testing::fail(e.what());
  }
  return 0;
}
