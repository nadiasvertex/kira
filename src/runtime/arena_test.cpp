#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>

#include "src/runtime/arena.h"
#include "src/testing/test_assert.h"

namespace {

using kira::testing::expect;

auto test_allocations_are_distinct_and_zeroed() -> void {
  auto arena = kira::runtime::bump_arena{};
  auto *a = arena.allocate(16);
  auto *b = arena.allocate(16);
  expect(a != nullptr && b != nullptr, "expected non-null allocations");
  expect(a != b, "expected distinct allocations to not overlap");
  auto zeros = std::array<std::byte, 16>{};
  expect(std::memcmp(a, zeros.data(), 16) == 0, "expected fresh memory zeroed");
}

auto test_allocation_across_block_boundary() -> void {
  auto arena = kira::runtime::bump_arena{};
  // Force more than one 1 MiB block to be carved.
  for (auto i = 0; i < 3; ++i) {
    auto *p = arena.allocate(1 << 20);
    expect(p != nullptr, "expected large allocation to succeed");
  }
}

auto test_c_abi_entry_point_allocates() -> void {
  auto *p = kira_rt_alloc(64);
  expect(p != nullptr, "expected kira_rt_alloc to return non-null memory");
}

} // namespace

auto main() -> int {
  try {
    test_allocations_are_distinct_and_zeroed();
    test_allocation_across_block_boundary();
    test_c_abi_entry_point_allocates();
  } catch (const std::exception &ex) {
    std::cerr << "arena_test failed: unhandled exception: " << ex.what()
              << '\n';
    std::exit(1);
  }
  return 0;
}
