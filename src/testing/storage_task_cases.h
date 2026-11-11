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
  // The entry point awaits without being a task: 12 + 42.
  expect_equal(run("main", {}), 54, "main()");
}

/// Runs the task cases.
inline auto task_cases(case_backend &backend) -> void {
  tasks_hand_back_their_result(backend);
  tasks_resume_after_each_await(backend);
  tasks_move_results_of_any_size(backend);
  tasks_run_when_spawned_or_dropped(backend);
}

} // namespace cinder::testing::storage_cases
