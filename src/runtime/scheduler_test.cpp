#include "src/runtime/scheduler.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#include "src/runtime/coroutine.h"
#include "src/testing/test_assert.h"

namespace {

using cinder::testing::expect;
namespace co = cinder::runtime::coroutine;
namespace sched = cinder::runtime::scheduler;

/// A task for these tests: the six coroutine slots, with the `state_ptr`
/// slot pointing at a `body` the test runner calls in place of a compiled
/// step function.
struct fake_task {
  std::array<uint64_t, co::k_slot_count> slots{};
  /// Runs one poll; returns the step function's status word.
  uint64_t (*body)(fake_task &) = nullptr;
  uint64_t input = 0;
  fake_task *other = nullptr;
  std::vector<fake_task *> many;
  /// Which of the awaits in `body` is next.
  size_t cursor = 0;

  auto raw() -> uint64_t * { return slots.data(); }
};

auto run_fake(void * /*context*/, uint64_t *task) -> uint64_t {
  auto *self = reinterpret_cast<fake_task *>(task); // slots is the first member
  return self->body(*self);
}

auto fake_runner() -> sched::runner { return sched::runner{.run = run_fake}; }

auto result_of(fake_task &t) -> uint64_t { return t.slots[co::k_value_slot]; }

auto yield_result(fake_task &t, uint64_t value) -> uint64_t {
  t.slots[co::k_value_slot] = value;
  return co::raw(co::status::suspended);
}

// A task that produces `input * 2`.
auto double_body(fake_task &t) -> uint64_t {
  return yield_result(t, t.input * 2);
}

// Awaits `other`, which nobody has spawned, and adds one to its result.
auto add_one_body(fake_task &t) -> uint64_t {
  if (!sched::await_poll(t.raw(), t.other->raw())) {
    return co::raw(co::status::pending);
  }
  return yield_result(t, result_of(*t.other) + 1);
}

auto test_block_on_runs_a_task() -> void {
  auto task = fake_task{.body = double_body, .input = 21};
  sched::block_on(task.raw(), sched::pool::io, fake_runner());
  expect(result_of(task) == 42, "block_on should run the task to its result");
  expect(sched::is_finished(task.raw()), "the task should be finished");
}

auto test_await_of_an_unstarted_task_goes_pending_then_resumes() -> void {
  auto inner = fake_task{.body = double_body, .input = 5};
  auto outer = fake_task{.body = add_one_body, .other = &inner};
  sched::block_on(outer.raw(), sched::pool::io, fake_runner());
  expect(result_of(outer) == 11,
         "outer should see the inner task's result plus one");
  expect(sched::is_finished(inner.raw()),
         "awaiting should have started the inner task");
}

auto throwing_body(fake_task & /*t*/) -> uint64_t {
  throw std::runtime_error("boom");
}

auto test_a_failing_task_rethrows_in_the_awaiter() -> void {
  auto task = fake_task{.body = throwing_body};
  auto caught = false;
  try {
    sched::block_on(task.raw(), sched::pool::io, fake_runner());
  } catch (const std::runtime_error &error) {
    caught = std::string_view(error.what()) == "boom";
  }
  expect(caught, "block_on should rethrow the task's failure");

  // A task awaiting the failed one fails the same way.
  auto outer = fake_task{.body = add_one_body, .other = &task};
  caught = false;
  try {
    sched::block_on(outer.raw(), sched::pool::io, fake_runner());
  } catch (const std::runtime_error &) {
    caught = true;
  }
  expect(caught, "a task awaiting a failed task should fail too");
}

// Waits on `other` forever; once cancelled it proceeds and finishes.
auto wait_forever_body(fake_task &t) -> uint64_t {
  if (!sched::await_poll(t.raw(), t.other->raw())) {
    return co::raw(co::status::pending);
  }
  const auto cancelled =
      t.slots[co::k_status_slot] == co::raw(co::status::cancel_requested);
  return yield_result(t, cancelled ? 99 : 0);
}

auto test_cancel_unblocks_a_waiting_task() -> void {
  // `stuck` returns pending without registering a wake, so it never
  // finishes and `waiter` stays waiting on it until cancelled.
  auto stuck = fake_task{.body = [](fake_task &t) -> uint64_t {
    return t.slots[co::k_status_slot] == co::raw(co::status::cancel_requested)
               ? co::raw(co::status::done)
               : co::raw(co::status::pending);
  }};
  auto waiter = fake_task{.body = wait_forever_body, .other = &stuck};
  sched::spawn(waiter.raw(), sched::pool::io, fake_runner());
  // Let the worker park `waiter` on `stuck` before cancelling it.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  expect(sched::cancel(waiter.raw()), "the scheduler owns the waiter");
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!sched::is_finished(waiter.raw()) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  expect(sched::is_finished(waiter.raw()),
         "cancelling a waiting task should let it unwind");
  expect(result_of(waiter) == 99, "the task should have seen the cancel");
  // Let `stuck` unwind too, so it does not keep the scheduler from idling.
  expect(sched::cancel(stuck.raw()), "the scheduler owns the stuck task");
  sched::wait_idle();
}

// ---- A task queued behind a busy worker ----------------------------------

std::atomic<int> g_started{0};

// Waits until a second task has started, which only happens if the pool runs
// the two at once. Gives up after a deadline so a regression fails the test
// instead of hanging it.
auto rendezvous_body(fake_task &t) -> uint64_t {
  g_started.fetch_add(1);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (g_started.load() < 2 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return yield_result(t, g_started.load() >= 2 ? 1 : 0);
}

auto test_a_second_task_does_not_wait_behind_a_busy_worker() -> void {
  auto first = fake_task{.body = rendezvous_body};
  auto second = fake_task{.body = rendezvous_body};
  sched::spawn(first.raw(), sched::pool::cpu, fake_runner());
  // Let a worker pick `first` up, so it is busy when `second` arrives.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  sched::spawn(second.raw(), sched::pool::cpu, fake_runner());
  sched::wait_idle();
  expect(result_of(first) == 1 && result_of(second) == 1,
         "two cpu tasks should run at the same time");
}

// ---- Many tasks on the cpu pool ------------------------------------------

std::atomic<uint64_t> g_ran{0};
std::mutex g_threads_mutex;
std::set<std::thread::id> g_threads;

auto count_body(fake_task &t) -> uint64_t {
  std::this_thread::sleep_for(std::chrono::microseconds(200));
  g_ran.fetch_add(t.input, std::memory_order_relaxed);
  {
    const auto lock = std::scoped_lock(g_threads_mutex);
    g_threads.insert(std::this_thread::get_id());
  }
  return yield_result(t, t.input);
}

// Awaits every task in `many` in order, summing their results.
auto sum_body(fake_task &t) -> uint64_t {
  while (t.cursor < t.many.size()) {
    if (!sched::await_poll(t.raw(), t.many[t.cursor]->raw())) {
      return co::raw(co::status::pending);
    }
    t.input += result_of(*t.many[t.cursor]);
    ++t.cursor;
  }
  return yield_result(t, t.input);
}

auto test_cpu_pool_runs_every_task_in_parallel() -> void {
  constexpr size_t count = 400;
  auto tasks = std::vector<fake_task>(count);
  auto parent = fake_task{.body = sum_body};
  uint64_t expected = 0;
  for (size_t i = 0; i < count; ++i) {
    tasks[i].body = count_body;
    tasks[i].input = i + 1;
    expected += i + 1;
    parent.many.push_back(&tasks[i]);
    sched::spawn(tasks[i].raw(), sched::pool::cpu, fake_runner());
  }
  sched::spawn(parent.raw(), sched::pool::cpu, fake_runner());
  sched::wait_idle();
  expect(g_ran.load() == expected, "every task should have run exactly once");
  expect(result_of(parent) == expected,
         "the parent should have summed every result");
  expect(sched::is_finished(parent.raw()), "the parent should be finished");
  expect(g_threads.size() > 1,
         "the cpu pool should have used more than one thread");
}

} // namespace

auto main() -> int {
  // Read when the cpu pool first starts, so set before any test spawns.
  setenv("CINDER_CPU_WORKERS", "4", 1);
  test_block_on_runs_a_task();
  test_await_of_an_unstarted_task_goes_pending_then_resumes();
  test_a_failing_task_rethrows_in_the_awaiter();
  test_cancel_unblocks_a_waiting_task();
  test_a_second_task_does_not_wait_behind_a_busy_worker();
  test_cpu_pool_runs_every_task_in_parallel();
  return 0;
}
