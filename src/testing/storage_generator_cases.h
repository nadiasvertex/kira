#pragma once

// Cases for generators in the storage form, run by every backend that compiles
// it (`bytecode_storage_test/generators.cn`). The expected values are worked
// out by hand from the fixture.

#include <cstdint>
#include <vector>

#include "src/testing/storage_cases.h"
#include "src/testing/test_assert.h"

namespace cinder::testing::storage_cases {

inline auto generators_yield_in_order(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "generators.cn");
  const auto run = [&](const char *name, std::vector<int64_t> args) {
    return p->scalar(name, args);
  };
  expect_equal(run("sum_to", {5}), 10, "sum_to(5)");
  expect_equal(run("sum_to", {100}), 4950, "sum_to(100)");
  expect_equal(run("sum_to", {1}), 0, "sum_to(1)");
  expect_equal(run("sum_to", {0}), 0, "a generator that yields nothing");
  // Each has its own state: 0+1+2 and then none (-1), against 0+100+200+300.
  expect_equal(run("interleave", {}), 602, "interleave()");
  // 0, then @none twice: 0*10000 + (-2 + 10)*100 + (-3 + 10).
  expect_equal(run("after_the_end", {}), 807, "after_the_end()");
  // 0, 1, then @none twice, though a second run of the body would yield 1.
  expect_equal(run("stays_finished", {}), 198, "stays_finished()");
}

inline auto generators_resume_inside_branches_and_loops(case_backend &backend)
    -> void {
  auto p = backend.open("bytecode_storage_test", "generators.cn");
  const auto run = [&](const char *name, std::vector<int64_t> args) {
    return p->scalar(name, args);
  };
  // 0, -1, 2, -3, 4, 99 weighted 1..6.
  expect_equal(run("zigzag_weighted", {5}), 606, "zigzag_weighted(5)");
  expect_equal(run("zigzag_weighted", {1}), 198, "zigzag_weighted(1)");
  expect_equal(run("zigzag_weighted", {0}), 99, "zigzag_weighted(0)");
}

inline auto generators_keep_parameters_and_borrows(case_backend &backend)
    -> void {
  auto p = backend.open("bytecode_storage_test", "generators.cn");
  const auto run = [&](const char *name, std::vector<int64_t> args) {
    return p->scalar(name, args);
  };
  // 7, 200, 30, then @none: 7 + 2000 + 30000 - 100000.
  expect_equal(run("mixed_weighted", {}), -67993, "mixed_weighted()");
  // The borrow of `p` still reads the same bytes after each suspension:
  // 4, 8, then 4 + 8.
  expect_equal(run("borrowed_sum", {4}), 4008012, "borrowed_sum(4)");
}

inline auto generators_yield_items_of_any_size(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "generators.cn");
  const auto run = [&](const char *name, std::vector<int64_t> args) {
    return p->scalar(name, args);
  };
  // Items of 24 bytes: (1, 2, 3) and (2, 4, 6) add 30201 and 60402.
  expect_equal(run("triple_sum", {3}), 90603, "triple_sum(3)");
  expect_equal(run("triple_sum", {0}), 0, "triple_sum(0)");
}

inline auto generators_cross_calls_by_value(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "generators.cn");
  expect_equal(p->scalar("through_calls", std::vector<int64_t>{5}), 10,
               "through_calls(5)");
}

inline auto generators_drop_what_they_own_once(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "generators.cn");
  const auto run = [&](const char *name) {
    return p->scalar(name, std::vector<int64_t>{});
  };
  // Ran to the end: dropped once (3), items 1 and 2, then @none.
  expect_equal(run("guarded_to_end"), 3120, "guarded_to_end()");
  // Dropped while suspended after its first item: dropped once on unwinding.
  expect_equal(run("guarded_cancelled_drops"), 31, "guarded_cancelled_drops()");
  // Never started: the value was never built, so it is never dropped.
  expect_equal(run("guarded_unstarted_drops"), 0, "guarded_unstarted_drops()");
}

/// Runs the generator cases.
inline auto generator_cases(case_backend &backend) -> void {
  generators_yield_in_order(backend);
  generators_resume_inside_branches_and_loops(backend);
  generators_keep_parameters_and_borrows(backend);
  generators_yield_items_of_any_size(backend);
  generators_cross_calls_by_value(backend);
  generators_drop_what_they_own_once(backend);
}

} // namespace cinder::testing::storage_cases
