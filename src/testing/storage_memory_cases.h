#pragma once

// Cases for raw memory holding aggregates inline, run by every backend that
// compiles the shared storage form (`bytecode_storage_test/raw_memory.cn`).
// The expected values are worked out by hand from the fixture; the drop counts
// follow chapter 38's raw-memory rules (a read gives the reader the value, a
// write never drops the old one).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "src/runtime/allocator.h"
#include "src/testing/storage_cases.h"
#include "src/testing/test_assert.h"

namespace cinder::testing::storage_cases {

inline auto raw_slots_hold_aggregates_at_their_stride(case_backend &backend)
    -> void {
  auto p = backend.open("bytecode_storage_test", "raw_memory.cn");
  expect_equal(p->scalar("slot_size_of_pair", {}), 16, "size_of[pair]");
  // Five identical pairs: 5 * (1 + 10).
  expect_equal(p->scalar("sum_pairs", std::vector<int64_t>{5}), 55,
               "sum_pairs");
  // Four different pairs: each read comes from its own slot.
  expect_equal(p->scalar("distinct_pairs", {}),
               3 * 1000000 + 300 * 1000 + 100 + 100, "distinct_pairs");
  // `s[1].to.b + s[2].from.a + s[0].to.b` = 8 + 9 + 4.
  expect_equal(p->scalar("nested", {}), 21, "nested");
  // Only the middle pair was written through the reference.
  expect_equal(p->scalar("through_reference", {}), 1 * 10000 + 50 * 100 + 3,
               "through_reference");
  // A reference bound with `let` holds the address of a local.
  expect_equal(p->scalar("write_through_reference", std::vector<int64_t>{10}),
               15, "write_through_reference");
  // Slot 1 starts at byte 16: `a` is the fifth int32 and `b` the fourth int64.
  expect_equal(p->scalar("pair_stride", {}), 77 * 100 + 7, "pair_stride");
  // 320-byte slots: the third head and the middle slot's last cell.
  expect_equal(p->scalar("wide_slots", {}), 3 * 100 + 7, "wide_slots");
  // Heads are 40 words apart; word 79 is the last cell of slot 1.
  expect_equal(p->scalar("wide_stride", {}), 2 * 1000 + 3 * 10 + 7,
               "wide_stride");
}

inline auto raw_slots_cost_one_allocation_for_the_buffer(case_backend &backend)
    -> void {
  auto p = backend.open("bytecode_storage_test", "raw_memory.cn");
  const auto allocations = runtime::heap_allocation_count();
  const auto live = runtime::heap_live_block_count();
  expect_equal(p->scalar("sum_pairs", std::vector<int64_t>{200}), 200 * 11,
               "sum_pairs");
  // The old representation boxed every pair: this is the buffer, only.
  expect_equal(
      static_cast<int64_t>(runtime::heap_allocation_count() - allocations), 1,
      "allocations for 200 pairs");
  expect_equal(runtime::heap_live_block_count() - live, 0,
               "blocks left behind");
}

inline auto
raw_reads_and_writes_follow_the_ownership_rules(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "raw_memory.cn");
  // Four values written, none dropped; four read out, four dropped.
  expect_equal(p->scalar("read_and_drop", std::vector<int64_t>{4}), 4,
               "read_and_drop");
  // The overwritten value is never dropped; the one read out is.
  expect_equal(p->scalar("overwrite_without_drop", {}), 1,
               "overwrite_without_drop");
  // Swapping moves values: no drops, then one for each value read out.
  expect_equal(p->scalar("swap_slots", {}), 0 * 1000 + 2 * 100 + 1 * 10 + 2,
               "swap_slots");
  // Replacing drops the old value once; the new one is dropped when read out.
  expect_equal(p->scalar("replace_slot", {}), 1 * 100 + 2 * 10 + 2,
               "replace_slot");
}

/// Runs the raw-memory cases.
inline auto memory_cases(case_backend &backend) -> void {
  raw_slots_hold_aggregates_at_their_stride(backend);
  raw_slots_cost_one_allocation_for_the_buffer(backend);
  raw_reads_and_writes_follow_the_ownership_rules(backend);
}

} // namespace cinder::testing::storage_cases
