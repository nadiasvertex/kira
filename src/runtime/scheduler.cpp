#include "src/runtime/scheduler.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "src/runtime/coroutine.h"

namespace cinder::runtime::scheduler {

namespace co = cinder::runtime::coroutine;

struct crew {
  std::vector<uint64_t *> tasks;
  /// The task whose body opened this crew; null on a thread that is not a
  /// worker.
  uint64_t *owner = nullptr;
};

namespace {

/// Where a spawned task is in its life. Only the scheduler moves a task
/// between these, and only with the lock held.
enum class task_state : uint8_t {
  queued,   ///< In its pool's run queue.
  running,  ///< A worker is inside its step function.
  waiting,  ///< Returned `pending`; a wake will queue it again.
  unwinding, ///< Failed with crews open; waiting for their tasks to stop.
  finished,  ///< Its result is in the value slot, or it failed.
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
  /// Spawned into a `crew`, which keeps the task alive until the block ends
  /// even when the program drops its handle.
  bool crew_owned = false;
  pool where = pool::io;
  runner how;
  /// Tasks to wake when this one finishes.
  std::vector<uint64_t *> waiters;
  /// Set when the step function threw, e.g. on a panic.
  std::exception_ptr failure;
  /// The crews this task's body has opened and not yet freed. A panic skips
  /// the end of the block, so the scheduler finishes them (`unwind_locked`).
  std::vector<crew *> open_crews;
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

/// The task this worker is polling, which owns any crew its body opens.
thread_local uint64_t *current_task = nullptr;

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

auto is_done(const uint64_t *task) -> bool {
  return status_of(task) == co::raw(co::status::done);
}

auto is_cancelled(const uint64_t *task) -> bool {
  return status_of(task) == co::raw(co::status::cancel_requested);
}

void push_locked(scheduler_state &s, uint64_t *task, task_record &rec);
void finish_unwinding_locked(scheduler_state &s, uint64_t *task,
                             task_record &rec);

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
  case task_state::unwinding:
    finish_unwinding_locked(s, task, *rec);
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

/// Asks a task to unwind, along with the tasks of every crew it has open:
/// they stop together, whichever `await` the body is at. A task already asked
/// is left where it is, so a repeated cancel does not requeue it.
void cancel_locked(scheduler_state &s, uint64_t *task, task_record &rec) {
  for (const auto *group : rec.open_crews) {
    for (auto *member : group->tasks) {
      if (auto *member_rec = record_of(member); member_rec != nullptr) {
        cancel_locked(s, member, *member_rec);
      }
    }
  }
  switch (rec.state) {
  case task_state::running:
    rec.cancel_pending = true;
    return;
  case task_state::queued:
    set_status(task, co::status::cancel_requested);
    return;
  case task_state::waiting:
    if (!is_cancelled(task)) {
      set_status(task, co::status::cancel_requested);
      push_locked(s, task, rec);
    }
    return;
  case task_state::unwinding:
  case task_state::finished:
    return;
  }
}

/// Finishes a failed task once every crew it left open has stopped.
void finish_unwinding_locked(scheduler_state &s, uint64_t *task,
                             task_record &rec) {
  for (const auto *group : rec.open_crews) {
    if (!std::ranges::all_of(group->tasks, is_done)) {
      return;
    }
  }
  for (auto *group : rec.open_crews) {
    // The crew's block will not reach `crew_free`.
    const auto owned = std::unique_ptr<crew>(group);
  }
  rec.open_crews.clear();
  finish_locked(s, task, rec);
}

/// A task's body panicked, so nothing will reach the end of its open `crew`
/// blocks. Their tasks may borrow from the task's frame, so it does not
/// finish until they have stopped.
void unwind_locked(scheduler_state &s, uint64_t *task, task_record &rec) {
  rec.state = task_state::unwinding;
  for (const auto *group : rec.open_crews) {
    for (auto *member : group->tasks) {
      auto *member_rec = record_of(member);
      if (member_rec == nullptr || is_done(member)) {
        continue;
      }
      member_rec->waiters.push_back(task);
    }
  }
  cancel_locked(s, task, rec);
  finish_unwinding_locked(s, task, rec);
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
    current_task = task;
    try {
      status = rec.how.run(rec.how.context, task);
    } catch (...) {
      failure = std::current_exception();
    }
    current_task = nullptr;
    lock.lock();

    if (failure != nullptr) {
      rec.failure = failure;
      unwind_locked(s, task, rec);
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
  while (target.workers.size() < wanted && target.queue.size() > target.idle) {
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
  if (record_of(task) != nullptr || is_done(task)) {
    return;
  }
  adopt_locked(s, task, where, how);
}

void block_on(uint64_t *task, pool where, runner how) {
  auto &s = state();
  auto lock = std::unique_lock(s.mutex);
  if (!is_done(task) && record_of(task) == nullptr) {
    adopt_locked(s, task, where, how);
  }
  s.finished.wait(lock, [&] { return is_done(task); });
  const auto *rec = record_of(task);
  const auto failure = rec != nullptr ? rec->failure : std::exception_ptr{};
  lock.unlock();
  if (failure != nullptr) {
    std::rethrow_exception(failure);
  }
}

namespace {

/// The failure of the first task in `tasks` that finished by panicking, if
/// any. Lock held.
auto first_failure(std::span<uint64_t *const> tasks) -> std::exception_ptr {
  for (const auto *task : tasks) {
    const auto *rec = record_of(task);
    if (is_done(task) && rec != nullptr && rec->failure != nullptr) {
      return rec->failure;
    }
  }
  return {};
}

/// Makes `self` a waiter on every unfinished task in `tasks`, and returns
/// whether there were any. Unstarted tasks are queued. If `stopping`, the
/// tasks are cancelled instead, and an unstarted one is left alone since it
/// has nothing to unwind. Lock held.
auto wait_on_locked(scheduler_state &s, uint64_t *self,
                    std::span<uint64_t *const> tasks, bool stopping) -> bool {
  const auto *self_rec = record_of(self);
  auto waiting = false;
  for (auto *awaited : tasks) {
    if (is_done(awaited)) {
      continue;
    }
    auto *rec = record_of(awaited);
    if (rec == nullptr) {
      if (stopping) {
        continue;
      }
      rec = &adopt_locked(
          s, awaited, self_rec != nullptr ? self_rec->where : pool::io,
          self_rec != nullptr ? self_rec->how : native_runner());
    }
    if (std::ranges::find(rec->waiters, self) == rec->waiters.end()) {
      rec->waiters.push_back(self);
    }
    if (stopping) {
      cancel_locked(s, awaited, *rec);
    }
    waiting = true;
  }
  return waiting;
}

/// Ends a wait that found nothing left to wait for.
auto finish_wait(std::unique_lock<std::mutex> &lock,
                 const std::exception_ptr &failure) -> bool {
  lock.unlock();
  if (failure != nullptr) {
    std::rethrow_exception(failure);
  }
  return true;
}

void adopt_all_locked(scheduler_state &s, std::span<uint64_t *const> tasks,
                      pool where, runner how) {
  for (auto *task : tasks) {
    if (!is_done(task) && record_of(task) == nullptr) {
      adopt_locked(s, task, where, how);
    }
  }
}

/// Cancels `tasks` and blocks until they have all finished. Lock held.
void stop_all_locked(scheduler_state &s, std::unique_lock<std::mutex> &lock,
                     std::span<uint64_t *const> tasks) {
  for (auto *task : tasks) {
    if (auto *rec = record_of(task); rec != nullptr) {
      cancel_locked(s, task, *rec);
    }
  }
  s.finished.wait(lock, [&] { return std::ranges::all_of(tasks, is_done); });
}

} // namespace

auto await_poll(uint64_t *self, uint64_t *awaited) -> bool {
  auto &s = state();
  auto lock = std::unique_lock(s.mutex);
  const auto tasks = std::span<uint64_t *const>(&awaited, 1);
  const auto failure = first_failure(tasks);
  if (!wait_on_locked(s, self, tasks, is_cancelled(self))) {
    return finish_wait(lock, failure);
  }
  return false;
}

auto await_all_poll(uint64_t *self, uint64_t *const *tasks, size_t count)
    -> bool {
  auto &s = state();
  auto lock = std::unique_lock(s.mutex);
  const auto all = std::span(tasks, count);
  const auto failure = first_failure(all);
  if (!wait_on_locked(s, self, all, failure != nullptr || is_cancelled(self))) {
    return finish_wait(lock, failure);
  }
  return false;
}

void block_on_all(uint64_t *const *tasks, size_t count, pool where,
                  runner how) {
  auto &s = state();
  auto lock = std::unique_lock(s.mutex);
  const auto all = std::span(tasks, count);
  adopt_all_locked(s, all, where, how);
  auto failure = std::exception_ptr{};
  s.finished.wait(lock, [&] {
    failure = first_failure(all);
    return failure != nullptr || std::ranges::all_of(all, is_done);
  });
  if (failure != nullptr) {
    stop_all_locked(s, lock, all);
  }
  finish_wait(lock, failure);
}

auto crew_new() -> crew * {
  auto group = std::make_unique<crew>();
  if (current_task != nullptr) {
    auto &s = state();
    const auto lock = std::scoped_lock(s.mutex);
    group->owner = current_task;
    record_of(current_task)->open_crews.push_back(group.get());
  }
  return group.release();
}

void crew_spawn(crew *group, uint64_t *task, pool where, runner how) {
  auto &s = state();
  const auto lock = std::scoped_lock(s.mutex);
  group->tasks.push_back(task);
  adopt_all_locked(s, std::span(&task, 1), where, how);
  if (auto *rec = record_of(task); rec != nullptr) {
    rec->crew_owned = true;
  }
}

auto crew_join_poll(uint64_t *self, crew *group) -> bool {
  return await_all_poll(self, group->tasks.data(), group->tasks.size());
}

void crew_join(crew *group, pool where, runner how) {
  block_on_all(group->tasks.data(), group->tasks.size(), where, how);
}

void crew_free(crew *group) {
  const auto owned = std::unique_ptr<crew>(group);
  if (group->owner != nullptr) {
    auto &s = state();
    const auto lock = std::scoped_lock(s.mutex);
    std::erase(record_of(group->owner)->open_crews, group);
  }
}

auto crew_size(const crew *group) -> size_t { return group->tasks.size(); }

auto crew_task(const crew *group, size_t index) -> uint64_t * {
  return group->tasks[index];
}

auto await_any_poll(uint64_t *self, uint64_t *const *tasks, size_t count)
    -> bool {
  auto &s = state();
  auto lock = std::unique_lock(s.mutex);
  const auto all = std::span(tasks, count);
  const auto failure = first_failure(all);
  const auto cancelled = is_cancelled(self);
  if (failure == nullptr && !cancelled && std::ranges::any_of(all, is_done)) {
    return true;
  }
  if (!wait_on_locked(s, self, all, failure != nullptr || cancelled)) {
    return finish_wait(lock, failure);
  }
  return false;
}

void block_on_any(uint64_t *const *tasks, size_t count, pool where,
                  runner how) {
  auto &s = state();
  auto lock = std::unique_lock(s.mutex);
  const auto all = std::span(tasks, count);
  adopt_all_locked(s, all, where, how);
  s.finished.wait(lock, [&] { return std::ranges::any_of(all, is_done); });
  const auto failure = first_failure(all);
  if (failure != nullptr) {
    stop_all_locked(s, lock, all);
  }
  finish_wait(lock, failure);
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
  cancel_locked(s, task, *rec);
  return true;
}

auto cancel_on_drop(uint64_t *task) -> bool {
  if (std::atomic_ref(task[co::k_record_slot])
          .load(std::memory_order_acquire) == 0) {
    return false;
  }
  {
    auto &s = state();
    const auto lock = std::scoped_lock(s.mutex);
    const auto *rec = record_of(task);
    if (rec != nullptr && rec->crew_owned) {
      return true;
    }
  }
  return cancel(task);
}

auto is_finished(const uint64_t *task) -> bool {
  auto &s = state();
  const auto lock = std::scoped_lock(s.mutex);
  return is_done(task);
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

auto cinder_rt_task_drop(uint64_t *task) -> uint32_t {
  return cinder::runtime::scheduler::cancel_on_drop(task) ? 1U : 0U;
}

auto cinder_rt_task_is_finished(uint64_t *task) -> uint32_t {
  return cinder::runtime::scheduler::is_finished(task) ? 1U : 0U;
}

auto cinder_rt_crew_new() -> uint64_t * {
  return reinterpret_cast<uint64_t *>(cinder::runtime::scheduler::crew_new()); // NOLINT
}

auto cinder_rt_crew_spawn(uint64_t *group, uint64_t *task) -> uint64_t * {
  namespace sched = cinder::runtime::scheduler;
  sched::crew_spawn(reinterpret_cast<sched::crew *>(group), task, // NOLINT
                    sched::pool::io, sched::native_runner());
  return task;
}

auto cinder_rt_crew_len(uint64_t *group) -> uint64_t {
  namespace sched = cinder::runtime::scheduler;
  return sched::crew_size(reinterpret_cast<sched::crew *>(group)); // NOLINT
}

auto cinder_rt_crew_task(uint64_t *group, uint64_t index) -> uint64_t * {
  namespace sched = cinder::runtime::scheduler;
  return sched::crew_task(reinterpret_cast<sched::crew *>(group), // NOLINT
                          index);
}

auto cinder_rt_crew_join_poll(uint64_t *self, uint64_t *group) -> uint32_t {
  namespace sched = cinder::runtime::scheduler;
  return sched::crew_join_poll(self, reinterpret_cast<sched::crew *>(group)) // NOLINT
             ? 1U
             : 0U;
}

auto cinder_rt_crew_free(uint64_t *group) -> uint64_t * {
  namespace sched = cinder::runtime::scheduler;
  sched::crew_free(reinterpret_cast<sched::crew *>(group)); // NOLINT
  return nullptr;
}

auto cinder_rt_crew_join(uint64_t *group) -> uint64_t * {
  namespace sched = cinder::runtime::scheduler;
  sched::crew_join(reinterpret_cast<sched::crew *>(group), // NOLINT
                   sched::pool::io, sched::native_runner());
  return nullptr;
}

auto cinder_rt_task_await_all_poll(uint64_t *self, uint64_t *const *tasks,
                                   uint64_t count) -> uint32_t {
  return cinder::runtime::scheduler::await_all_poll(self, tasks,
                                                    static_cast<size_t>(count))
             ? 1U
             : 0U;
}

auto cinder_rt_task_await_any_poll(uint64_t *self, uint64_t *const *tasks,
                                   uint64_t count) -> uint32_t {
  return cinder::runtime::scheduler::await_any_poll(self, tasks,
                                                    static_cast<size_t>(count))
             ? 1U
             : 0U;
}

auto cinder_rt_task_block_on_all(uint64_t *const *tasks, uint64_t count)
    -> uint64_t * {
  namespace sched = cinder::runtime::scheduler;
  sched::block_on_all(tasks, static_cast<size_t>(count), sched::pool::io,
                      sched::native_runner());
  return nullptr;
}

auto cinder_rt_task_block_on_any(uint64_t *const *tasks, uint64_t count)
    -> uint64_t * {
  namespace sched = cinder::runtime::scheduler;
  sched::block_on_any(tasks, static_cast<size_t>(count), sched::pool::io,
                      sched::native_runner());
  return nullptr;
}
}
