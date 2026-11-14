#pragma once

// Cases for calls between modules in the storage form, run by every backend
// that compiles it (`bytecode_storage_test/modules.cn`, which names
// `modules_tools.cn` as a companion). The expected values are worked out by
// hand from the fixture.

#include <cstdint>
#include <vector>

#include "src/testing/storage_cases.h"
#include "src/testing/test_assert.h"

namespace cinder::testing::storage_cases {

/// A call into another module builds its result in the caller's storage,
/// hands aggregate arguments over by address, and resolves against that
/// module's functions even when this module has a function of the same name.
inline auto calls_between_modules(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "modules.cn");
  const auto run = [&](const char *name, std::vector<int64_t> args) {
    return p->scalar(name, args);
  };
  // tools.make(4) = {4, 8}; total = 12; sample's make(4) = 5.
  expect_equal(run("pair_total", {4}), 125, "pair_total(4)");
  const auto forwarded = p->into("forward", std::vector<int64_t>{7});
  expect_equal(forwarded.at<int32_t>(0), 7, "forward(7).a");
  expect_equal(forwarded.at<int32_t>(4), 14, "forward(7).b");
  // consume({5, 10}) = 5.
  expect_equal(run("handed", {5}), 5, "handed(5)");
  // widen(3) = {3, 4, 5}; weigh = 300 + 40 + 5.
  expect_equal(run("weighed", {3}), 345, "weighed(3)");
  // twice(3): make(3) = {3, 6}; make(6) = {6, 12}; 600 + 12.
  expect_equal(run("chained", {3}), 612, "chained(3)");
  // The drop runs inside `discard`, before the counter is read.
  expect_equal(run("dropped_there", {7}), 707, "dropped_there(7)");
  // Two locals drop in reverse order at the end of the branch: 8 then 7.
  expect_equal(run("dropped_here", {7}), 87, "dropped_here(7)");
  expect_equal(run("dropped_here", {0}), 0, "dropped_here(0)");
}

/// Runs the module cases.
inline auto module_cases(case_backend &backend) -> void {
  calls_between_modules(backend);
}

} // namespace cinder::testing::storage_cases
