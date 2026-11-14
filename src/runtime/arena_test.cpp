#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>

#include "src/runtime/allocator.h"
#include "src/runtime/arena.h"
#include "src/testing/test_assert.h"

namespace {

using cinder::testing::expect;

auto test_allocations_are_distinct_and_zeroed() -> void {
  auto arena = cinder::runtime::bump_arena{};
  auto *a = arena.allocate(16);
  auto *b = arena.allocate(16);
  expect(a != nullptr && b != nullptr, "expected non-null allocations");
  expect(a != b, "expected distinct allocations to not overlap");
  auto zeros = std::array<std::byte, 16>{};
  expect(std::memcmp(a, zeros.data(), 16) == 0, "expected fresh memory zeroed");
}

auto test_allocation_across_block_boundary() -> void {
  auto arena = cinder::runtime::bump_arena{};
  // Force more than one 1 MiB block to be carved.
  for (auto i = 0; i < 3; ++i) {
    auto *p = arena.allocate(1 << 20);
    expect(p != nullptr, "expected large allocation to succeed");
  }
}

auto test_c_abi_entry_point_allocates() -> void {
  auto *p = cinder_heap_alloc(64);
  expect(p != nullptr, "expected cinder_heap_alloc to return non-null memory");
}

} // namespace

namespace {

/// An odd-sized request leaves the bump pointer off any 16- or 64-byte
/// boundary, so each following aligned request has to pad to reach one. Blocks
/// must still not overlap and must be zeroed.
auto test_aligned_allocation_pads_and_does_not_overlap() -> void {
  auto arena = cinder::runtime::bump_arena{};
  auto previous_end = uintptr_t{0};
  for (const size_t align : {size_t{16}, size_t{64}, size_t{16}, size_t{256}}) {
    auto *odd = arena.allocate(24);
    expect(odd != nullptr, "expected an odd-sized allocation");
    auto *aligned = arena.allocate(40, align);
    const auto address = reinterpret_cast<uintptr_t>(aligned); // NOLINT
    expect(address % align == 0, "expected the block on its alignment");
    expect(address >= reinterpret_cast<uintptr_t>(odd) + 24, // NOLINT
           "expected the aligned block not to overlap the one before it");
    expect(address >= previous_end, "expected blocks to be disjoint");
    auto zeros = std::array<std::byte, 40>{};
    expect(std::memcmp(aligned, zeros.data(), 40) == 0,
           "expected padding to leave the block zeroed");
    previous_end = address + 40;
  }
}

/// A request larger than a whole block, with a large alignment, still lands on
/// the alignment.
auto test_aligned_allocation_larger_than_a_block() -> void {
  auto arena = cinder::runtime::bump_arena{};
  auto *p = arena.allocate((size_t{1} << 20) + 64, 128);
  expect(reinterpret_cast<uintptr_t>(p) % 128 == 0, // NOLINT
         "expected an oversized block on its alignment");
  auto *q = arena.allocate(8);
  expect(q != nullptr && q != p, "expected the arena to keep working");
}

} // namespace

auto main() -> int {
  try {
    test_aligned_allocation_pads_and_does_not_overlap();
    test_aligned_allocation_larger_than_a_block();
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
