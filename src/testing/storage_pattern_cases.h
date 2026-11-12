#pragma once

// Cases for pattern tests in the storage form, run by every backend that
// compiles it (`bytecode_storage_test/patterns.cn`). The expected values are
// worked out by hand from the fixture.

#include <cstdint>
#include <vector>

#include "src/testing/storage_cases.h"
#include "src/testing/test_assert.h"

namespace cinder::testing::storage_cases {

inline auto match_selects_by_variant(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "patterns.cn");
  const auto run = [&](const char *name, std::vector<int64_t> args) {
    return p->scalar(name, args);
  };
  expect_equal(run("area", {0, 2, 0}), 12, "area of a circle");
  expect_equal(run("area", {1, 3, 5}), 15, "area of a rect");
  expect_equal(run("area", {2, 0, 0}), 7, "area of the empty shape");
  expect_equal(run("is_circle", {0}), 1, "is_circle(circle)");
  expect_equal(run("is_circle", {1}), 0, "is_circle(rect)");
  expect_equal(run("is_circle", {2}), 0, "is_circle(empty)");
  expect_equal(run("either", {0}), 5, "either(circle)");
  expect_equal(run("either", {1}), 6, "either(rect)");
  expect_equal(run("either", {2}), 5, "either(empty)");
  expect_equal(run("subject_call", {0}), 4, "subject_call(circle)");
  expect_equal(run("subject_call", {1}), 9, "subject_call(rect)");
  expect_equal(run("subject_call", {2}), 0, "subject_call(empty)");
}

inline auto match_statements_fall_through(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "patterns.cn");
  expect_equal(p->scalar("partial", std::vector<int64_t>{0, 5, 0}), 105,
               "partial(circle)");
  expect_equal(p->scalar("partial", std::vector<int64_t>{1, 3, 4}), 107,
               "partial(rect)");
  expect_equal(p->scalar("partial", std::vector<int64_t>{2, 0, 0}), 101,
               "partial(empty)");
}

inline auto match_tests_literals_and_parts(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "patterns.cn");
  const auto run = [&](const char *name, std::vector<int64_t> args) {
    return p->scalar(name, args);
  };
  expect_equal(run("classify", {1, 0}), 10, "classify(some 0)");
  expect_equal(run("classify", {1, 1}), 20, "classify(some 1)");
  expect_equal(run("classify", {1, 5}), 35, "classify(some 5)");
  expect_equal(run("classify", {0, 0}), 40, "classify(none)");
  expect_equal(run("tuple_pick", {0, 7}), 7, "tuple_pick(0, 7)");
  expect_equal(run("tuple_pick", {3, 0}), 300, "tuple_pick(3, 0)");
  expect_equal(run("tuple_pick", {2, 5}), 7, "tuple_pick(2, 5)");
  expect_equal(run("struct_pick", {1, 9}), 9, "struct_pick(1, 9)");
  expect_equal(run("struct_pick", {4, 9}), 40, "struct_pick(4, 9)");
  expect_equal(run("unbox", {1, 3, 4}), 3004, "unbox(one)");
  expect_equal(run("unbox", {0, 3, 4}), -1, "unbox(none_here)");
}

inline auto loops_and_else_use_patterns(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "patterns.cn");
  const auto run = [&](const char *name, std::vector<int64_t> args) {
    return p->scalar(name, args);
  };
  expect_equal(run("drain", {4}), 10, "drain(4)");
  expect_equal(run("drain", {0}), 0, "drain(0)");
  // 4 and 3 are added, 2 is skipped, and 1 stops the loop with nothing left.
  expect_equal(run("drain_some", {4}), 70, "drain_some(4)");
  expect_equal(run("first_or", {1, 5}), 1005, "first_or(some)");
  expect_equal(run("first_or", {0, 5}), -5, "first_or(none)");
}

/// Sums that own droppable payloads. The `machine` entry points report a
/// counter that each drop appends its id to: `drops * 100 + result`, so the
/// digits before the last two say which drops ran and in what order.
inline auto sums_that_own_drops_are_dropped_once(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "patterns.cn");
  const auto run = [&](const char *name, std::vector<int64_t> args) {
    return p->scalar(name, args);
  };
  expect_equal(run("cell_scope", {1, 7}), 71, "cell_scope(full)");
  expect_equal(run("cell_scope", {0, 7}), 1, "cell_scope(vacant)");
  expect_equal(run("cell_take", {1, 7}), 707, "cell_take(full)");
  expect_equal(run("cell_take", {0, 7}), 0, "cell_take(vacant)");
  expect_equal(run("cell_peek", {1, 7}), 701, "cell_peek(full)");
  expect_equal(run("cell_peek", {0, 7}), 0, "cell_peek(vacant)");
  // The payload that is not bound drops first, then the bound one.
  expect_equal(run("duo_first", {2, 3, 4}), 4303, "duo_first(both)");
  expect_equal(run("duo_first", {1, 3, 4}), 303, "duo_first(left)");
  expect_equal(run("duo_first", {0, 3, 4}), 0, "duo_first(nothing)");
  // Bound payloads drop in reverse order of binding; the wildcard arm drops
  // what the other variant holds.
  expect_equal(run("duo_both", {2, 3, 4}), 4307, "duo_both(both)");
  expect_equal(run("duo_both", {1, 3, 4}), 300, "duo_both(left)");
  expect_equal(run("duo_both", {0, 3, 4}), 0, "duo_both(nothing)");
  expect_equal(run("drain_tracked", {3}), 32106, "drain_tracked(3)");
  expect_equal(run("drain_tracked", {0}), 0, "drain_tracked(0)");
  // Item 3 drops at the end of its pass, and item 2 drops on the way out.
  expect_equal(run("drain_until", {3, 2}), 3203, "drain_until(3, 2)");
  expect_equal(run("first_tracked", {4}), 404, "first_tracked(some)");
  expect_equal(run("first_tracked", {0}), -5, "first_tracked(none)");
  expect_equal(run("cell_replace", {}), 1200, "cell_replace");
  expect_equal(run("cell_pass", {1, 7}), 709, "cell_pass(full)");
  expect_equal(run("cell_pass", {0, 7}), 9, "cell_pass(vacant)");
}

/// Runs the pattern cases.
inline auto pattern_cases(case_backend &backend) -> void {
  match_selects_by_variant(backend);
  match_statements_fall_through(backend);
  match_tests_literals_and_parts(backend);
  loops_and_else_use_patterns(backend);
  sums_that_own_drops_are_dropped_once(backend);
}

} // namespace cinder::testing::storage_cases
