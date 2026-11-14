#pragma once

// Cases for results built in their final storage, run by every backend that
// compiles the storage form (`bytecode_storage_test/rvo.cn`). The expected
// values are worked out by hand from the fixture.

#include <cstdint>
#include <vector>

#include "src/testing/storage_cases.h"
#include "src/testing/test_assert.h"

namespace cinder::testing::storage_cases {

/// Constructors, chained calls, branches, matches, tuples and arrays written
/// straight into the result. The entry point weights each field by a different
/// power of ten, so a value written to the wrong place changes the sum.
inline auto results_are_built_where_they_end_up(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "rvo.cn");
  const auto run = [&](const char *name, std::vector<int64_t> args) {
    return p->scalar(name, args);
  };
  expect_equal(run("direct_forms", {2}), 4243125423, "direct_forms(2)");
  expect_equal(run("direct_forms", {1}), 4232114312, "direct_forms(1)");
  expect_equal(run("direct_forms", {0}), 4221023201, "direct_forms(0)");
  expect_equal(run("option_sum", {3}), 36, "option_sum(some)");
  expect_equal(run("option_sum", {0}), -1, "option_sum(none)");
}

/// `?` whose success type differs from the function's: the failure is rebuilt
/// in the result, whatever position of the value under construction it is in.
inline auto failures_are_rebuilt_in_the_result(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "rvo.cn");
  const auto run = [&](const char *name, std::vector<int64_t> args) {
    return p->scalar(name, args);
  };
  expect_equal(run("wrap_value", {4}), 45, "wrap_value(ok)");
  expect_equal(run("wrap_value", {-3}), 1003, "wrap_value(err 3)");
  expect_equal(run("wrap_value", {0}), 1000, "wrap_value(err 0)");
  expect_equal(run("wrap_inside_value", {5}), 505, "wrap_inside_value(ok)");
  expect_equal(run("wrap_inside_value", {-2}), 2002,
               "wrap_inside_value(err)");
  expect_equal(run("maybe_pair_value", {4}), 48, "maybe_pair_value(some)");
  expect_equal(run("maybe_pair_value", {0}), -3, "maybe_pair_value(none)");
}

/// A failure inside a value under construction drops the parts already built,
/// once and in order. The `machine` entry points report `drops * 1000 +
/// result`, where the digits of `drops` are the ids of the drops that ran.
inline auto a_failed_construction_drops_what_it_built(case_backend &backend)
    -> void {
  auto p = backend.open("bytecode_storage_test", "rvo.cn");
  const auto run = [&](const char *name, std::vector<int64_t> args) {
    return p->scalar(name, args);
  };
  // Built whole, the consumer drops the fields in order.
  expect_equal(run("twin_try", {3}), 73001, "twin_try(ok)");
  // The first field is dropped when the second fails.
  expect_equal(run("twin_try", {0}), 7110, "twin_try(err)");
  expect_equal(run("trio_try", {3}), 329001, "trio_try(ok)");
  expect_equal(run("trio_try", {2}), 219001, "trio_try(ok 2)");
  // The second `?` fails with the first field built.
  expect_equal(run("trio_try", {1}), 1010, "trio_try(second fails)");
  // The first `?` fails with nothing built.
  expect_equal(run("trio_try", {-1}), 9, "trio_try(first fails)");
  // Parts built before a failure drop newest first, and a whole value drops
  // its fields in order.
  expect_equal(run("trio_all_try", {3}), 321001, "trio_all_try(ok)");
  expect_equal(run("trio_all_try", {2}), 12010, "trio_all_try(third fails)");
  expect_equal(run("trio_all_try", {1}), 1010, "trio_all_try(second fails)");
}

/// A returned local is built in the result storage when every return moves it
/// (the test binaries run every case with that on and off, so these values are
/// the same either way). Drops happen once, in the caller, for the returned
/// value, and in the callee for a local no return moved.
inline auto returned_locals_keep_their_meaning(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "rvo.cn");
  const auto run = [&](const char *name, std::vector<int64_t> args) {
    return p->scalar(name, args);
  };
  expect_equal(run("named_value", {3}), 34, "named_value(3)");
  expect_equal(run("named_value", {0}), 1, "named_value(0)");
  expect_equal(run("early_value", {1, 3}), 39, "early_value(early)");
  expect_equal(run("early_value", {0, 3}), 1, "early_value(late)");
  expect_equal(run("loop_value", {4}), 64, "loop_value(4)");
  expect_equal(run("loop_value", {0}), 0, "loop_value(0)");
  expect_equal(run("reassigned_value", {2}), 32, "reassigned_value(2)");
  expect_equal(run("named_tracked_drops", {4}), 5001, "named_tracked_drops(4)");
  expect_equal(run("named_tracked_drops", {0}), 1001, "named_tracked_drops(0)");
  // The local is the result: dropped by the caller alone.
  expect_equal(run("sometimes_tracked_drops", {1, 5}), 5001,
               "sometimes_tracked_drops(returned)");
  // The local is dropped by the callee, and the other value by the caller.
  expect_equal(run("sometimes_tracked_drops", {0, 5}), 56001,
               "sometimes_tracked_drops(not returned)");
}

/// Runs the result-storage cases.
inline auto rvo_cases(case_backend &backend) -> void {
  results_are_built_where_they_end_up(backend);
  failures_are_rebuilt_in_the_result(backend);
  a_failed_construction_drops_what_it_built(backend);
  returned_locals_keep_their_meaning(backend);
}

} // namespace cinder::testing::storage_cases
