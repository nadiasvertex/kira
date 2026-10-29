#include "src/runtime/scheduler.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "src/runtime/coroutine.h"

namespace cinder::runtime::scheduler {

namespace co = cinder::runtime::coroutine;

namespace {

/// Where a spawned task is in its life. Only the scheduler moves a task
/// between these, and only with the lock held.
enum class task_state : uint8_t {
  queued,   ///< In its pool's run queue.
  running,  ///< A worker is inside its step function.
  waiting,  ///< Returned `pending`; a wake will queue it again.
  finished, ///< Its result is in the value slot, or it failed.
};

/// The scheduler's bookkeeping for one task, pointed to by the task's
/// `record` slot. Records are never freed: the task objects they belong to
/// are not freed either (see `drop_plan::cancels_generator`), and a waiter
/// list may still name a finished task.
struct task_record {
  task_state state = task_state::queued;
  /// A wake arrived while the task was running. When the step function then
  /// returns `pending`, the task is queued again at once instead of waiting.
  bool notified = false;
  /// A cancel arrived while the task was running. It is applied once the
  /// step function returns.
  bool cancel_pending = false;
  pool where = pool::io;
  runner how;
  /// Tasks to wake when this one finishes.
  std::vector<uint64_t *> waiters;
  /// Set when the step function threw, e.g. on a panic.
  std::exception_ptr failure;
};

struct pool_state {
  std::deque<uint64_t *> queue;
  std::condition_variable work;
  std::vector<std::thread> workers;
  /// Workers blocked on `work`, i.e. ready to take a queued task.
  size_t idle = 0;
};

struct scheduler_state {
  std::mutex mutex;
  pool_state pools[2];
  /// Notified whenever any task finishes.
  std::condition_variable finished;
  /// Tasks taken over and not yet finished.
  size_t unfinished = 0;
};

/// Leaked on purpose: worker threads block on this state until the process
/// ends, so it must outlive every static destructor.
auto state() -> scheduler_state & {
  static auto *const instance = new scheduler_state;
  return *instance;
}

auto record_of(const uint64_t *task) -> task_record * {
  return reinterpret_cast<task_record *>(task[co::k_record_slot]);
}

auto status_of(const uint64_t *task) -> uint64_t {
  return task[co::k_status_slot];
}

void set_status(uint64_t *task, co::status s) {
  task[co::k_status_slot] = co::raw(s);
}

auto pool_index(pool p) -> size_t { return static_cast<size_t>(p); }

auto worker_count(pool p) -> size_t {
  // `io` is a single-threaded driver unless `CINDER_IO_WORKERS` asks for
  // more, which is how tests run `io` tasks in parallel.
  const char *const variable =
      p == pool::io ? "CINDER_IO_WORKERS" : "CINDER_CPU_WORKERS";
  if (p == pool::io && std::getenv(variable) == nullptr) {
    return 1;
  }
  if (const char *requested = std::getenv(variable); requested != nullptr) {
    const auto parsed = std::strtoul(requested, nullptr, 10);
    if (parsed > 0) {
      return static_cast<size_t>(parsed);
    }
  }
  return std::max<size_t>(1, std::thread::hardware_concurrency());
}

void push_locked(scheduler_state &s, uint64_t *task, task_record &rec);

/// Moves a waiting task back to its run queue, or remembers the wake for a
/// task that is mid-poll.
void wake_locked(scheduler_state &s, uint64_t *task) {
  auto *rec = record_of(task);
  if (rec == nullptr) {
    return;
  }
  switch (rec->state) {
  case task_state::waiting:
    push_locked(s, task, *rec);
    return;
  case task_state::running:
    rec->notified = true;
    return;
  case task_state::queued:
  case task_state::finished:
    return;
  }
}

void finish_locked(scheduler_state &s, uint64_t *task, task_record &rec) {
  rec.state = task_state::finished;
  set_status(task, co::status::done);
  auto waiters = std::move(rec.waiters);
  rec.waiters.clear();
  for (auto *waiter : waiters) {
    wake_locked(s, waiter);
  }
  --s.unfinished;
  s.finished.notify_all();
}

void worker_loop(pool where) {
  auto &s = state();
  auto &mine = s.pools[pool_index(where)];
  auto lock = std::unique_lock(s.mutex);
  while (true) {
    ++mine.idle;
    mine.work.wait(lock, [&] { return !mine.queue.empty(); });
    --mine.idle;
    auto *task = mine.queue.front();
    mine.queue.pop_front();
    auto &rec = *record_of(task);
    rec.state = task_state::running;
    rec.notified = false;

    lock.unlock();
    auto status = uint64_t{0};
    auto failure = std::exception_ptr{};
    try {
      status = rec.how.run(rec.how.context, task);
    } catch (...) {
      failure = std::current_exception();
    }
    lock.lock();

    if (failure != nullptr) {
      rec.failure = failure;
      finish_locked(s, task, rec);
    } else if (status == co::raw(co::status::pending)) {
      if (rec.cancel_pending) {
        rec.cancel_pending = false;
        set_status(task, co::status::cancel_requested);
        push_locked(s, task, rec);
      } else if (rec.notified) {
        push_locked(s, task, rec);
      } else {
        rec.state = task_state::waiting;
      }
    } else {
      // `suspended` is a task handing back its one result; `done` is a body
      // that returned or unwound after a cancel. Either way it is finished.
      finish_locked(s, task, rec);
    }
  }
}

void push_locked(scheduler_state &s, uint64_t *task, task_record &rec) {
  auto &target = s.pools[pool_index(rec.where)];
  rec.state = task_state::queued;
  rec.notified = false;
  target.queue.push_back(task);
  // Workers start on first use, one for each queued task that no idle worker
  // can take, up to the pool's size. A busy worker does not count: a task
  // queued behind a long one would otherwise wait for it.
  const auto wanted = worker_count(rec.where);
  while (target.workers.size() < wanted &&
         target.queue.size() > target.idle) {
    target.workers.emplace_back(worker_loop, rec.where);
  }
  target.work.notify_one();
}

/// Takes over a task nobody has started. Lock held.
auto adopt_locked(scheduler_state &s, uint64_t *task, pool where, runner how)
    -> task_record & {
  auto *rec = new task_record;
  rec->where = where;
  rec->how = how;
  std::atomic_ref(task[co::k_record_slot])
      .store(reinterpret_cast<uint64_t>(rec), std::memory_order_release);
  ++s.unfinished;
  push_locked(s, task, *rec);
  return *rec;
}

auto run_native(void * /*context*/, uint64_t *task) -> uint64_t {
  using step_function = uint64_t (*)(void *, uint64_t, void *);
  const auto step =
      reinterpret_cast<step_function>(task[co::k_step_function_slot]);
  return step(reinterpret_cast<void *>(task[co::k_state_ptr_slot]),
              task[co::k_resume_index_slot], task);
}

} // namespace

auto native_runner() -> runner { return runner{.run = run_native}; }

void spawn(uint64_t *task, pool where, runner how) {
  auto &s = state();
  const auto lock = std::scoped_lock(s.mutex);
  if (record_of(task) != nullptr || status_of(task) == co::raw(co::status::done)) {
    return;
  }
  adopt_locked(s, task, where, how);
}

void block_on(uint64_t *task, pool where, runner how) {
  auto &s = state();
  auto lock = std::unique_lock(s.mutex);
  if (status_of(task) != co::raw(co::status::done) &&
      record_of(task) == nullptr) {
    adopt_locked(s, task, where, how);
  }
  s.finished.wait(lock, [&] {
    return status_of(task) == co::raw(co::status::done);
  });
  const auto *rec = record_of(task);
  const auto failure = rec != nullptr ? rec->failure : std::exception_ptr{};
  lock.unlock();
  if (failure != nullptr) {
    std::rethrow_exception(failure);
  }
}

auto await_poll(uint64_t *self, uint64_t *awaited) -> bool {
  auto &s = state();
  auto lock = std::unique_lock(s.mutex);
  auto *awaited_rec = record_of(awaited);
  if (status_of(awaited) == co::raw(co::status::done)) {
    const auto failure =
        awaited_rec != nullptr ? awaited_rec->failure : std::exception_ptr{};
    lock.unlock();
    if (failure != nullptr) {
      std::rethrow_exception(failure);
    }
    return true;
  }
  if (status_of(self) == co::raw(co::status::cancel_requested)) {
    return true;
  }
  if (awaited_rec == nullptr) {
    const auto *self_rec = record_of(self);
    awaited_rec = &adopt_locked(
        s, awaited, self_rec != nullptr ? self_rec->where : pool::io,
        self_rec != nullptr ? self_rec->how : native_runner());
  }
  if (std::ranges::find(awaited_rec->waiters, self) ==
      awaited_rec->waiters.end()) {
    awaited_rec->waiters.push_back(self);
  }
  return false;
}

namespace {

/// The failure of the first finished task in `tasks`, if any finished.
/// Lock held. Sets `any_finished`.
auto first_finished_failure(uint64_t *const *tasks, size_t count,
                            bool &any_finished) -> std::exception_ptr {
  for (size_t i = 0; i < count; ++i) {
    if (status_of(tasks[i]) == co::raw(co::status::done)) { // NOLINT
      any_finished = true;
      const auto *rec = record_of(tasks[i]); // NOLINT
      return rec != nullptr ? rec->failure : std::exception_ptr{};
    }
  }
  any_finished = false;
  return {};
}

} // namespace

auto await_any_poll(uint64_t *self, uint64_t *const *tasks, size_t count)
    -> bool {
  auto &s = state();
  auto lock = std::unique_lock(s.mutex);
  auto any_finished = false;
  const auto failure = first_finished_failure(tasks, count, any_finished);
  if (any_finished) {
    lock.unlock();
    if (failure != nullptr) {
      std::rethrow_exception(failure);
    }
    return true;
  }
  if (status_of(self) == co::raw(co::status::cancel_requested)) {
    return true;
  }
  const auto *self_rec = record_of(self);
  for (size_t i = 0; i < count; ++i) {
    auto *awaited = tasks[i]; // NOLINT
    auto *rec = record_of(awaited);
    if (rec == nullptr) {
      rec = &adopt_locked(s, awaited,
                          self_rec != nullptr ? self_rec->where : pool::io,
                          self_rec != nullptr ? self_rec->how
                                              : native_runner());
    }
    if (std::ranges::find(rec->waiters, self) == rec->waiters.end()) {
      rec->waiters.push_back(self);
    }
  }
  return false;
}

void block_on_any(uint64_t *const *tasks, size_t count, pool where,
                  runner how) {
  auto &s = state();
  auto lock = std::unique_lock(s.mutex);
  for (size_t i = 0; i < count; ++i) {
    auto *task = tasks[i]; // NOLINT
    if (status_of(task) != co::raw(co::status::done) &&
        record_of(task) == nullptr) {
      adopt_locked(s, task, where, how);
    }
  }
  auto any_finished = false;
  auto failure = std::exception_ptr{};
  s.finished.wait(lock, [&] {
    failure = first_finished_failure(tasks, count, any_finished);
    return any_finished;
  });
  lock.unlock();
  if (failure != nullptr) {
    std::rethrow_exception(failure);
  }
}

auto cancel(uint64_t *task) -> bool {
  // Dropping a plain generator asks here too. A coroutine the scheduler never
  // took over has no record, which spares it the global lock.
  if (std::atomic_ref(task[co::k_record_slot])
          .load(std::memory_order_acquire) == 0) {
    return false;
  }
  auto &s = state();
  const auto lock = std::scoped_lock(s.mutex);
  auto *rec = record_of(task);
  if (rec == nullptr) {
    return false;
  }
  switch (rec->state) {
  case task_state::finished:
    return true;
  case task_state::running:
    rec->cancel_pending = true;
    return true;
  case task_state::queued:
    set_status(task, co::status::cancel_requested);
    return true;
  case task_state::waiting:
    set_status(task, co::status::cancel_requested);
    push_locked(s, task, *rec);
    return true;
  }
  return true;
}

auto is_finished(const uint64_t *task) -> bool {
  auto &s = state();
  const auto lock = std::scoped_lock(s.mutex);
  return status_of(task) == co::raw(co::status::done);
}

void wait_idle() {
  auto &s = state();
  auto lock = std::unique_lock(s.mutex);
  s.finished.wait(lock, [&] { return s.unfinished == 0; });
}

} // namespace cinder::runtime::scheduler

extern "C" {
auto cinder_rt_task_block_on(uint64_t *task, uint64_t pool) -> uint64_t * {
  namespace sched = cinder::runtime::scheduler;
  sched::block_on(task, static_cast<sched::pool>(pool), sched::native_runner());
  return nullptr;
}

auto cinder_rt_task_await_poll(uint64_t *self, uint64_t *awaited) -> uint32_t {
  return cinder::runtime::scheduler::await_poll(self, awaited) ? 1U : 0U;
}

auto cinder_rt_task_spawn(uint64_t *task, uint64_t pool) -> uint64_t * {
  namespace sched = cinder::runtime::scheduler;
  sched::spawn(task, static_cast<sched::pool>(pool), sched::native_runner());
  return nullptr;
}

auto cinder_rt_task_cancel(uint64_t *task) -> uint32_t {
  return cinder::runtime::scheduler::cancel(task) ? 1U : 0U;
}

auto cinder_rt_task_is_finished(uint64_t *task) -> uint32_t {
  return cinder::runtime::scheduler::is_finished(task) ? 1U : 0U;
}

auto cinder_rt_task_await_any_poll(uint64_t *self, uint64_t *const *tasks,
                                   uint64_t count) -> uint32_t {
  return cinder::runtime::scheduler::await_any_poll(
             self, tasks, static_cast<size_t>(count))
             ? 1U
             : 0U;
}

auto cinder_rt_task_block_on_any(uint64_t *const *tasks, uint64_t count)
    -> uint64_t * {
  namespace sched = cinder::runtime::scheduler;
  sched::block_on_any(tasks, static_cast<size_t>(count), sched::pool::io,
                      sched::native_runner());
  return nullptr;
}
}
