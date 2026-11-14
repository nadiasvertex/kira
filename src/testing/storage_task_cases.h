#pragma once

// Cases for tasks in the storage form, run by every backend that compiles it
// (`bytecode_storage_test/tasks.cn`). The expected values are worked out by
// hand from the fixture.

#include <cstdint>
#include <vector>

#include "src/testing/storage_cases.h"
#include "src/testing/test_assert.h"

namespace cinder::testing::storage_cases {

inline auto tasks_hand_back_their_result(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "tasks.cn");
  const auto run = [&](const char *name, std::vector<int64_t> args) {
    return p->scalar(name, args);
  };
  expect_equal(run("run_double", {21}), 42, "run_double(21)");
  expect_equal(run("run_double", {0}), 0, "run_double(0)");
  // 0 + 2 + 4 + 6 + 8, then nothing, then a thousand awaits.
  expect_equal(run("run_sum", {5}), 20, "run_sum(5)");
  expect_equal(run("run_sum", {0}), 0, "run_sum(0)");
  expect_equal(run("run_sum", {100}), 9900, "run_sum(100)");
}

inline auto tasks_resume_after_each_await(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "tasks.cn");
  const auto run = [&](const char *name, std::vector<int64_t> args) {
    return p->scalar(name, args);
  };
  // a = 6, b = 12: 6 + 12 * 10.
  expect_equal(run("run_chain", {3}), 126, "run_chain(3)");
  // The borrow of `p` reads the same bytes after the suspension: 2 + 6 * 100
  // + 4 * 10000.
  expect_equal(run("run_borrows", {2}), 40602, "run_borrows(2)");
}

inline auto tasks_move_results_of_any_size(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "tasks.cn");
  const auto run = [&](const char *name, std::vector<int64_t> args) {
    return p->scalar(name, args);
  };
  // (2, 4, 6) read as 2 + 4 * 100 + 6 * 10000.
  expect_equal(run("run_triple", {2}), 60402, "run_triple(2)");
  expect_equal(run("run_triple", {0}), 0, "run_triple(0)");
  // The awaited task's result is `unit`: nothing moves, the caller goes on.
  expect_equal(run("run_unit", {9}), 10, "run_unit(9)");
}

inline auto tasks_run_when_spawned_or_dropped(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "tasks.cn");
  const auto run = [&](const char *name, std::vector<int64_t> args) {
    return p->scalar(name, args);
  };
  // Two spawned tasks: 8 * 100 + 10.
  expect_equal(run("run_spawned", {4}), 810, "run_spawned(4)");
  // A task nobody started is dropped without running; its body would not
  // finish in any reasonable time.
  expect_equal(run("run_unstarted", {}), 7, "run_unstarted()");
  // The entry point awaits without being a task, on single tasks and on
  // groups: 12 + 42 + 10 + 12 * 100 + 14 * 10000.
  expect_equal(run("main", {}), 141264, "main()");
}

inline auto tasks_wait_for_groups(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "tasks.cn");
  const auto run = [&](const char *name, std::vector<int64_t> args) {
    return p->scalar(name, args);
  };
  // double(3) = 6 and chain(3) = 126: 6 + 126 * 1000.
  expect_equal(run("run_together", {3}), 126006, "run_together(3)");
  // (2, 4, 6) as 2 + 40 + 600, double(2) = 4, chain(2) = 84.
  expect_equal(run("run_wide_group", {2}), 844642, "run_wide_group(2)");
  // The slow branch is cancelled while it waits on one of its own tasks.
  expect_equal(run("run_first_wins", {21}), 42, "run_first_wins(21)");
  expect_equal(run("run_first_wins", {0}), 0, "run_first_wins(0)");
  // Sum of 2i + 2(i + 1) for i in 0..4: 2 + 6 + 10 + 14.
  expect_equal(run("run_groups_in_a_loop", {4}), 32, "run_groups_in_a_loop(4)");
  expect_equal(run("run_groups_in_a_loop", {0}), 0, "run_groups_in_a_loop(0)");
  // An `on(cpu)` block is a task of its own: 7 * 5 + 1.
  expect_equal(run("run_on_a_pool", {7}), 36, "run_on_a_pool(7)");
}

inline auto tasks_drop_results_once(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "tasks.cn");
  const auto run = [&](const char *name, std::vector<int64_t> args) {
    return p->scalar(name, args);
  };
  // One drop of id 4 after the read: drops = 4, seen = 4.
  expect_equal(run("noisy_read_by_get", {}), 44, "noisy_read_by_get()");
  // One drop of id 5: 5 * 100 + 50.
  expect_equal(run("noisy_read_by_await", {}), 550, "noisy_read_by_await()");
  // A task that finished and was never read drops its result with the handle:
  // one drop of id 6, and the marker's 2.
  expect_equal(run("noisy_finished_unread", {}), 62, "noisy_finished_unread()");
  // Fields drop in declaration order (3, then 4); so do tuple members.
  expect_equal(run("noisy_holder_unread", {}), 342, "noisy_holder_unread()");
  expect_equal(run("noisy_tuple_unread", {}), 562, "noisy_tuple_unread()");
  // The read result is dropped once, by the reader: 78 and 15.
  expect_equal(run("noisy_read_then_drop", {}), 7815, "noisy_read_then_drop()");
  // A task that never ran has no result.
  expect_equal(run("noisy_unstarted", {}), 1, "noisy_unstarted()");
  // A result that is a sum drops the payload it holds, and `none` owes nothing.
  expect_equal(run("noisy_maybe_unread", {5}), 52, "noisy_maybe_unread(some)");
  expect_equal(run("noisy_maybe_unread", {0}), 2, "noisy_maybe_unread(none)");
  expect_equal(run("noisy_maybe_read", {6}), 606, "noisy_maybe_read(some)");
  expect_equal(run("noisy_maybe_read", {0}), 0, "noisy_maybe_read(none)");
  expect_equal(run("noisy_pick_unread", {5}), 52, "noisy_pick_unread(left)");
  expect_equal(run("noisy_pick_unread", {0}), 92, "noisy_pick_unread(right)");
}

inline auto tasks_join_a_crew(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "tasks.cn");
  const auto run = [&](const char *name, std::vector<int64_t> args) {
    return p->scalar(name, args);
  };
  // double(3) = 6 and chain(3) = 6 + 12 * 10 = 126, read after the block.
  expect_equal(run("run_crew_in_task", {3}), 12606, "run_crew_in_task(3)");
  expect_equal(run("crew_in_function", {3}), 12606, "crew_in_function(3)");
  expect_equal(run("run_crew_in_loop", {5}), 5, "run_crew_in_loop(5)");
  // The block waits for a child that is still running at the end of the body.
  expect_equal(run("crew_waits_in_task", {}), 71, "crew_waits_in_task()");
  expect_equal(run("crew_waits_on_return", {}), 59, "crew_waits_on_return()");
  expect_equal(run("crew_waits_in_function", {}), 3, "crew_waits_in_function()");
  // `c.errors()`: the errors of 3, 6 and 9 in spawn order, from inside a task.
  expect_equal(run("run_errors_in_task", {10}), 30609, "run_errors_in_task(10)");
  expect_equal(run("run_errors_in_task", {2}), 0, "run_errors_in_task(2)");
  // The error 9, then the `ok` 40 read by `get()` twice after `errors()`.
  expect_equal(run("errors_then_get", {}), 94040, "errors_then_get()");
}

/// Runs the task cases.
inline auto task_cases(case_backend &backend) -> void {
  tasks_join_a_crew(backend);
  tasks_drop_results_once(backend);
  tasks_hand_back_their_result(backend);
  tasks_resume_after_each_await(backend);
  tasks_move_results_of_any_size(backend);
  tasks_run_when_spawned_or_dropped(backend);
  tasks_wait_for_groups(backend);
}

} // namespace cinder::testing::storage_cases
