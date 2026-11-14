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
inline auto sums_that_own_drops_are_dropped_once(case_backend &backend)
    -> void {
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

/// Literal and nested tests on sums that own drops. The payload a failed test
/// leaves with the sum is dropped once, where the sum ends.
inline auto
tests_beyond_the_variant_on_sums_that_own_drops(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "patterns.cn");
  const auto run = [&](const char *name, std::vector<int64_t> args) {
    return p->scalar(name, args);
  };
  // The literal holds: the first arm binds the payload and drops it.
  expect_equal(run("stamped_pick", {1, 7, 0}), 7007, "stamped_pick(0)");
  // The literal fails: the second arm leaves the payload with the sum.
  expect_equal(run("stamped_pick", {1, 7, 3}), 7103, "stamped_pick(3)");
  expect_equal(run("stamped_pick", {0, 7, 0}), 5, "stamped_pick(empty)");
  expect_equal(run("nested_take", {2, 7}), 707, "nested_take(full)");
  expect_equal(run("nested_take", {1, 7}), 1, "nested_take(vacant)");
  expect_equal(run("nested_take", {0, 7}), 2, "nested_take(none)");
  expect_equal(run("nested_peek", {2, 7}), 703, "nested_peek(full)");
  expect_equal(run("nested_peek", {1, 7}), 1, "nested_peek(vacant)");
  expect_equal(run("nested_peek", {0, 7}), 2, "nested_peek(none)");
  // Arms that all leave, and a `let else` that leaves.
  expect_equal(run("nested_leave", {2, 7}), 707, "nested_leave(full)");
  expect_equal(run("nested_leave", {1, 7}), 1, "nested_leave(vacant)");
  expect_equal(run("nested_leave", {0, 7}), 2, "nested_leave(none)");
  expect_equal(run("nested_else", {2, 7}), 707, "nested_else(full)");
  expect_equal(run("nested_else", {1, 7}), -7, "nested_else(vacant)");
  expect_equal(run("nested_else", {0, 7}), -7, "nested_else(none)");
}

/// Arms with guards: the guard reads the pattern's bindings, runs once and only
/// when the pattern holds, and a failure of either goes on to the next arm.
inline auto match_arms_with_guards(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "patterns.cn");
  const auto run = [&](const char *name, std::vector<int64_t> args) {
    return p->scalar(name, args);
  };
  expect_equal(run("guard_pick", {1, 2, 9}), 7, "guard_pick(rect 2 9)");
  expect_equal(run("guard_pick", {1, 5, 5}), 7000, "guard_pick(rect 5 5)");
  expect_equal(run("guard_pick", {1, 9, 5}), 0, "guard_pick(rect 9 5)");
  expect_equal(run("guard_pick", {0, 200, 0}), 200, "guard_pick(circle 200)");
  expect_equal(run("guard_pick", {0, 5, 0}), 1005, "guard_pick(circle 5)");
  expect_equal(run("guard_pick", {2, 0, 0}), 0, "guard_pick(empty)");
  expect_equal(run("guard_stmt", {0, 20, 0}), 120, "guard_stmt(circle 20)");
  expect_equal(run("guard_stmt", {0, 4, 0}), 101, "guard_stmt(circle 4)");
  expect_equal(run("guard_stmt", {2, 0, 0}), 102, "guard_stmt(empty)");
  expect_equal(run("guard_pair", {3, 10}), 7, "guard_pair(3 10)");
  expect_equal(run("guard_pair", {10, 3}), 13, "guard_pair(10 3)");
  expect_equal(run("guard_leave", {1, 1, 2}), 10, "guard_leave(held)");
  expect_equal(run("guard_leave", {0, 1, 2}), 20, "guard_leave(guard fails)");
  expect_equal(run("guard_leave", {1, 1, 3}), 20,
               "guard_leave(second literal)");
  expect_equal(run("guard_leave", {1, 2, 2}), 30, "guard_leave(first literal)");
  // The trail names the guards that ran: 1, 12, 12 (neither held), 3.
  expect_equal(run("guard_order", {0, 20, 0}), 1020, "guard_order(circle 20)");
  expect_equal(run("guard_order", {0, 8, 0}), 12108, "guard_order(circle 8)");
  expect_equal(run("guard_order", {0, 3, 0}), 12000, "guard_order(circle 3)");
  expect_equal(run("guard_order", {1, 5, 2}), 3003, "guard_order(rect 5 2)");
  expect_equal(run("guard_order", {1, 2, 5}), 3000, "guard_order(rect 2 5)");
  expect_equal(run("guard_order", {2, 0, 0}), 0, "guard_order(empty)");
  // On a sum that owns a drop, the payload is dropped once whether the guard
  // holds or fails.
  expect_equal(run("guard_owned", {1, 7, 9}), 7009, "guard_owned(held)");
  expect_equal(run("guard_owned", {1, 7, 3}), 7001, "guard_owned(failed)");
  expect_equal(run("guard_owned", {0, 7, 3}), 5, "guard_owned(empty)");
}

/// Runs the pattern cases.
inline auto pattern_cases(case_backend &backend) -> void {
  match_selects_by_variant(backend);
  match_statements_fall_through(backend);
  match_tests_literals_and_parts(backend);
  loops_and_else_use_patterns(backend);
  sums_that_own_drops_are_dropped_once(backend);
  tests_beyond_the_variant_on_sums_that_own_drops(backend);
  match_arms_with_guards(backend);
}

} // namespace cinder::testing::storage_cases
