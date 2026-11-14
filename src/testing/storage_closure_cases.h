#pragma once

// Cases for closures in the storage form, run by every backend that compiles
// it (`bytecode_storage_test/closures.cn`). The expected values are worked out
// by hand from the fixture; the drop counts follow chapter 16 (a closure
// drops what was moved into it, in reverse capture order, when it is dropped).

#include <cstdint>
#include <vector>

#include "src/runtime/allocator.h"
#include "src/testing/storage_cases.h"
#include "src/testing/test_assert.h"

namespace cinder::testing::storage_cases {

inline auto closures_read_their_captures(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "closures.cn");
  expect_equal(p->scalar("add_captured", std::vector<int64_t>{37}), 42,
               "add_captured");
  // 1 + 3 + 40, the pair read out of the environment bytes.
  expect_equal(p->scalar("captured_pair", {}), 44, "captured_pair");
  // 2 * 3 + 4 * 100 + 5, with the captures at offsets 0, 8 and 16.
  expect_equal(p->scalar("mixed_alignment", std::vector<int64_t>{3, 4, 5}), 411,
               "mixed_alignment");
  // first 5; the two calls through the callee return 20 and 42; total 42.
  expect_equal(p->scalar("by_reference", {}), 5 * 1000 + 62 * 10 + 42,
               "by_reference");
}

inline auto closures_drop_what_they_own(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "closures.cn");
  // The closure returns 11 and drops its one captured value at the end.
  expect_equal(p->scalar("owning_scope_end", {}), 11 * 100 + 1,
               "owning_scope_end");
  // The callee owns the closure it was passed and drops it.
  expect_equal(p->scalar("owning_passed", {}), 22 * 100 + 2, "owning_passed");
  // Reverse capture order: the second capture (5) drops, then the first (4).
  expect_equal(p->scalar("several", {}), 10 * 1000 + 54, "several");
  // Nothing dropped when the closure was returned; it drops where it ends.
  expect_equal(p->scalar("returned", {}), 8 * 100 + 7, "returned");
  // A value captured by copy is not dropped by the closure.
  expect_equal(p->scalar("read_only", {}), 7 * 100 + 0, "read_only");
}

inline auto closures_cost_one_block_each(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "closures.cn");
  for (const int64_t count : {int64_t{10}, int64_t{100}}) {
    const auto allocations = runtime::heap_allocation_count();
    const auto live = runtime::heap_live_block_count();
    // Each pass is `2 + i`, summed.
    expect_equal(p->scalar("count_closures", std::vector<int64_t>{count}),
                 2 * count + count * (count - 1) / 2, "count_closures");
    // The old representation boxed the pair, the environment and the closure
    // value: this is the environment, only.
    expect_equal(
        static_cast<int64_t>(runtime::heap_allocation_count() - allocations),
        count, "allocations for the closures");
    expect_equal(runtime::heap_live_block_count() - live, 0,
                 "blocks left behind");
  }
}

/// Runs the closure cases.
inline auto closure_cases(case_backend &backend) -> void {
  closures_read_their_captures(backend);
  closures_drop_what_they_own(backend);
  closures_cost_one_block_each(backend);
}

} // namespace cinder::testing::storage_cases
