#include "src/runtime/scheduler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <pthread.h>
#include <unistd.h>

#include "src/runtime/allocator.h"
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
  queued,    ///< In its pool's run queue.
  running,   ///< A worker is inside its step function.
  waiting,   ///< Returned `pending`; a wake will queue it again.
  unwinding, ///< Failed with crews open; waiting for their tasks to stop.
  finished,  ///< Its result is in the value slot, or it failed.
};

/// The scheduler's bookkeeping for one task, pointed to by the task's
/// `record` slot. It is freed with the task, once the task has finished, the
/// program has dropped its handle (`release`), and nothing else names it.
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
  /// The program has dropped its handle to the task.
  bool released = false;
  /// Runtime structures other than the scheduler's own lists that name the
  /// task: a crew it was spawned into, or a reactor wait list (`hold`).
  size_t holds = 0;
  /// The tasks whose `waiters` name this one. A task that finishes leaves
  /// those lists, so none of them names a freed task.
  std::vector<uint64_t *> waiting_on;
  /// The memory word whose waiter list names this task (`word_waiter`).
  const uint64_t *word = nullptr;
};

struct pool_state {
  std::deque<uint64_t *> queue;
  std::condition_variable work;
  /// Notified when a poll stops running, for a worker waiting in `resuming`.
  std::condition_variable slot;
  /// Workers blocked on `work`, or started and on their way there.
  size_t idle = 0;
  /// Workers inside a step function, not counting one blocked in a wait. This
  /// is the number the pool's size limits.
  size_t running = 0;
  /// Workers whose blocking wait has ended, waiting to run again. They go
  /// ahead of the queue, since a poll is already underway on each.
  size_t resuming = 0;
};

struct scheduler_state {
  std::mutex mutex;
  pool_state pools[2];
  /// Notified whenever any task finishes.
  std::condition_variable finished;
  /// Tasks taken over and not yet finished.
  std::unordered_set<uint64_t *> unfinished;
  /// The tasks waiting on each memory word (`word_waiter`).
  std::unordered_map<const uint64_t *, std::vector<uint64_t *>> word_waiters;
};

/// The task this worker is polling, which owns any crew its body opens.
thread_local uint64_t *current_task = nullptr;

/// True on a thread running `worker_loop`, including in a plain function a
/// task called.
thread_local bool is_worker = false;

/// The pool of a thread running `worker_loop`.
thread_local pool worker_pool = pool::io;

/// This poll's panic has been printed, or came from a task that printed it.
thread_local bool panic_reported = false;

/// How long `shutdown` lets tasks stop. A task that never reaches a
/// cancellation check must not keep the process alive.
constexpr auto k_shutdown_grace = std::chrono::seconds(1);

constexpr int k_panic_exit_code = 101; // `bytecode::k_panic_exit_code`

/// A worker's stack, the size of a main thread's by default, so a task can
/// recurse as deep as the entry function. A thread's default is much smaller
/// on macOS. Pages are only committed when touched.
constexpr size_t k_worker_stack_bytes = size_t{8} * 1024 * 1024;

/// Leaked on purpose: worker threads block on this state until the process
/// ends, so it must outlive every static destructor.
auto state() -> scheduler_state & {
  static auto *const instance = new scheduler_state;
  return *instance;
}

auto record_of(const uint64_t *task) -> task_record * {
  return reinterpret_cast<task_record *>(task[co::k_record_slot]);
}

/// Atomic, because a task's dropper reads the status outside the lock to
/// decide whether there is anything to cancel.
auto status_of(const uint64_t *task) -> uint64_t {
  return std::atomic_ref(
             const_cast<uint64_t *>(task)[co::k_status_slot]) // NOLINT
      .load(std::memory_order_acquire);
}

void set_status(uint64_t *task, co::status s) {
  std::atomic_ref(task[co::k_status_slot])
      .store(co::raw(s), std::memory_order_release);
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

/// `worker_count`, read once: it runs under the lock on every push.
auto pool_size(pool p) -> size_t {
  static const size_t sizes[] = {worker_count(pool::io),
                                 worker_count(pool::cpu)};
  return sizes[pool_index(p)];
}

auto is_done(const uint64_t *task) -> bool {
  return status_of(task) == co::raw(co::status::done);
}

auto is_cancelled(const uint64_t *task) -> bool {
  return status_of(task) == co::raw(co::status::cancel_requested);
}

void push_locked(scheduler_state &s, uint64_t *task, task_record &rec);
void dispatch_locked(pool_state &target, pool where);

/// Frees a coroutine object, its state block, and its record.
void free_coroutine(uint64_t *task) {
  cinder_heap_free(reinterpret_cast<void *>(task[co::k_state_ptr_slot]),
                   task[co::k_state_bytes_slot]);
  const auto owned = std::unique_ptr<task_record>(record_of(task));
  cinder_heap_free(task, co::k_slot_count * sizeof(uint64_t));
}

/// Frees `task` if it has finished and nothing names it any more. Lock held.
void free_if_unused_locked(uint64_t *task, const task_record &rec) {
  if (rec.state == task_state::finished && rec.released && rec.holds == 0) {
    free_coroutine(task);
  }
}

/// Lets go of a `hold` on `task`. Lock held.
void unhold_locked(uint64_t *task) {
  auto *rec = record_of(task);
  --rec->holds;
  free_if_unused_locked(task, *rec);
}

/// Makes `waiter` one of the tasks `awaited` wakes when it finishes. Lock
/// held.
void add_waiter_locked(uint64_t *awaited, task_record &rec, uint64_t *waiter) {
  if (std::ranges::find(rec.waiters, waiter) != rec.waiters.end()) {
    return;
  }
  rec.waiters.push_back(waiter);
  if (auto *waiter_rec = record_of(waiter); waiter_rec != nullptr) {
    waiter_rec->waiting_on.push_back(awaited);
  }
}

/// Frees a crew and lets go of its tasks. Lock held.
void free_crew_locked(crew *group) {
  const auto owned = std::unique_ptr<crew>(group);
  for (auto *member : group->tasks) {
    if (record_of(member) != nullptr) {
      unhold_locked(member);
    }
  }
}
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

/// Marks `task` finished and wakes its waiters. It may free `task`, so the
/// caller must not touch it afterwards.
void finish_locked(scheduler_state &s, uint64_t *task, task_record &rec) {
  rec.state = task_state::finished;
  set_status(task, co::status::done);
  // Leave every list that still names this task: a cancelled task, or one
  // that a `race` woke, can be a waiter of tasks that have not finished.
  for (auto *awaited : rec.waiting_on) {
    std::erase(record_of(awaited)->waiters, task);
  }
  rec.waiting_on.clear();
  if (rec.word != nullptr) {
    if (const auto found = s.word_waiters.find(rec.word);
        found != s.word_waiters.end()) {
      std::erase(found->second, task);
      if (found->second.empty()) {
        s.word_waiters.erase(found);
      }
    }
    rec.word = nullptr;
  }
  auto waiters = std::move(rec.waiters);
  rec.waiters.clear();
  for (auto *waiter : waiters) {
    if (auto *waiter_rec = record_of(waiter); waiter_rec != nullptr) {
      std::erase(waiter_rec->waiting_on, task);
    }
    wake_locked(s, waiter);
  }
  s.unfinished.erase(task);
  s.finished.notify_all();
  free_if_unused_locked(task, rec);
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
    free_crew_locked(group);
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
      add_waiter_locked(member, *member_rec, task);
    }
  }
  cancel_locked(s, task, rec);
  finish_unwinding_locked(s, task, rec);
}

void worker_loop(pool where) {
  is_worker = true;
  worker_pool = where;
  auto &s = state();
  auto &mine = s.pools[pool_index(where)];
  auto lock = std::unique_lock(s.mutex);
  while (true) {
    mine.work.wait(lock, [&] {
      return !mine.queue.empty() && mine.resuming == 0 &&
             mine.running < pool_size(where);
    });
    --mine.idle;
    ++mine.running;
    auto *task = mine.queue.front();
    mine.queue.pop_front();
    if (!mine.queue.empty()) {
      // Several places can free up at one notify; pass the wake along.
      dispatch_locked(mine, where);
    }
    auto &rec = *record_of(task);
    rec.state = task_state::running;
    rec.notified = false;

    lock.unlock();
    auto status = uint64_t{0};
    auto failure = std::exception_ptr{};
    current_task = task;
    panic_reported = false;
    try {
      status = rec.how.run(rec.how.context, task);
    } catch (...) {
      failure = std::current_exception();
    }
    current_task = nullptr;
    lock.lock();
    --mine.running;
    ++mine.idle;
    dispatch_locked(mine, where);

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
  dispatch_locked(target, rec.where);
}

/// Starts a thread running `worker_loop`. Returns false if the system has no
/// thread to give. Workers are never joined.
auto start_worker(pool where) -> bool {
  pthread_attr_t attributes{};
  if (pthread_attr_init(&attributes) != 0) {
    return false;
  }
  (void)pthread_attr_setstacksize(&attributes, k_worker_stack_bytes);
  (void)pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
  pthread_t thread{};
  const auto failed = pthread_create(
      &thread, &attributes,
      [](void *argument) -> void * {
        worker_loop(static_cast<pool>(reinterpret_cast<uintptr_t>(argument)));
        return nullptr;
      },
      reinterpret_cast<void *>(static_cast<uintptr_t>(where)));
  (void)pthread_attr_destroy(&attributes);
  return failed == 0;
}

/// Gives a free place in the pool to whoever should take it: a worker back
/// from a blocking wait, else a queued task. Workers start on first use, one
/// for each task that could run now and no idle worker can take. A busy
/// worker does not count, and neither does a blocked one, so a task queued
/// behind either does not wait for it.
void dispatch_locked(pool_state &target, pool where) {
  if (target.resuming > 0) {
    target.slot.notify_one();
    return;
  }
  const auto size = pool_size(where);
  const auto free = size - std::min(size, target.running);
  while (std::min(target.queue.size(), free) > target.idle) {
    static const auto stop_at_exit = std::atexit([] { (void)shutdown(); });
    (void)stop_at_exit;
    if (!start_worker(where)) {
      // A worker that polls or waits will reach the queue later. With none,
      // nothing will, and a program left waiting would hang.
      if (target.running == 0 && target.idle == 0) {
        report_panic("could not start a thread to run a task");
        std::_Exit(k_panic_exit_code);
      }
      break;
    }
    ++target.idle;
  }
  target.work.notify_one();
}

/// Blocks until `done` holds. On a worker, a plain function a task called is
/// waiting: it cannot suspend, so its place in the pool goes to another
/// worker, which runs what it waits for, and it takes a place back before
/// continuing. A pool therefore never runs more polls than its size. Lock
/// held.
template <typename predicate>
void blocking_wait(scheduler_state &s, std::unique_lock<std::mutex> &lock,
                   predicate done) {
  if (!is_worker || done()) {
    s.finished.wait(lock, done);
    return;
  }
  auto &mine = s.pools[pool_index(worker_pool)];
  --mine.running;
  dispatch_locked(mine, worker_pool);
  s.finished.wait(lock, done);
  ++mine.resuming;
  mine.slot.wait(lock, [&] { return mine.running < pool_size(worker_pool); });
  --mine.resuming;
  ++mine.running;
  dispatch_locked(mine, worker_pool);
}

/// Takes over a task nobody has started. Lock held.
auto adopt_locked(scheduler_state &s, uint64_t *task, pool where, runner how)
    -> task_record & {
  auto *rec = new task_record;
  rec->where = where;
  rec->how = how;
  std::atomic_ref(task[co::k_record_slot])
      .store(reinterpret_cast<uint64_t>(rec), std::memory_order_release);
  s.unfinished.insert(task);
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

namespace {

auto load_word(const uint64_t *word) -> uint64_t {
  return std::atomic_ref(*const_cast<uint64_t *>(word)).load(); // NOLINT
}

/// The step function of a `word_waiter` task, whose value slots hold the
/// word and the value it waits to see change.
auto word_waiter_step(void * /*state*/, uint64_t /*resume_index*/, void *object)
    -> uint64_t {
  auto *task = static_cast<uint64_t *>(object);
  const auto *word = reinterpret_cast<const uint64_t *>( // NOLINT
      task[co::k_value_slot]);
  const auto expected = task[co::k_value_slot + 1];
  auto &s = state();
  const auto lock = std::scoped_lock(s.mutex);
  if (is_cancelled(task) || load_word(word) != expected) {
    return co::raw(co::status::done);
  }
  auto &rec = *record_of(task);
  if (rec.word == nullptr) {
    s.word_waiters[word].push_back(task);
    rec.word = word;
  }
  return co::raw(co::status::pending);
}

} // namespace

void wait_word(const uint64_t *word, uint64_t expected) {
  auto &s = state();
  auto lock = std::unique_lock(s.mutex);
  blocking_wait(s, lock, [&] { return load_word(word) != expected; });
}

auto word_waiter(const uint64_t *word, uint64_t expected) -> uint64_t * {
  auto *task = static_cast<uint64_t *>(
      cinder_heap_alloc(co::k_slot_count * sizeof(uint64_t)));
  using step_function = uint64_t (*)(void *, uint64_t, void *);
  const step_function step = word_waiter_step;
  task[co::k_step_function_slot] = reinterpret_cast<uint64_t>(step); // NOLINT
  task[co::k_state_ptr_slot] = 0;
  task[co::k_resume_index_slot] = 0;
  task[co::k_status_slot] = co::raw(co::status::suspended);
  task[co::k_record_slot] = 0;
  task[co::k_value_slot] = reinterpret_cast<uint64_t>(word); // NOLINT
  task[co::k_value_slot + 1] = expected;
  spawn(task, pool::io, native_runner());
  return task;
}

void wake_word(const uint64_t *word) {
  auto &s = state();
  const auto lock = std::scoped_lock(s.mutex);
  if (const auto found = s.word_waiters.find(word);
      found != s.word_waiters.end()) {
    const auto waiters = std::move(found->second);
    s.word_waiters.erase(found);
    for (auto *waiter : waiters) {
      record_of(waiter)->word = nullptr;
      wake_locked(s, waiter);
    }
  }
  // A thread in `wait_word` waits on `finished`.
  s.finished.notify_all();
}

void wake(uint64_t *task) {
  auto &s = state();
  const auto lock = std::scoped_lock(s.mutex);
  wake_locked(s, task);
}

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
  blocking_wait(s, lock, [&] { return is_done(task); });
  const auto *rec = record_of(task);
  const auto failure = rec != nullptr ? rec->failure : std::exception_ptr{};
  lock.unlock();
  if (failure != nullptr) {
    panic_reported = true;
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
    add_waiter_locked(awaited, *rec, self);
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
    panic_reported = true;
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
  blocking_wait(s, lock, [&] { return std::ranges::all_of(tasks, is_done); });
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
  blocking_wait(s, lock, [&] {
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
  adopt_all_locked(s, std::span(&task, 1), where, how);
  if (auto *rec = record_of(task); rec != nullptr) {
    rec->crew_owned = true;
    // The crew reads its tasks back after the block's handles are gone.
    ++rec->holds;
  }
  group->tasks.push_back(task);
}

auto crew_join_poll(uint64_t *self, crew *group) -> bool {
  return await_all_poll(self, group->tasks.data(), group->tasks.size());
}

void crew_join(crew *group, pool where, runner how) {
  block_on_all(group->tasks.data(), group->tasks.size(), where, how);
}

void crew_free(crew *group) {
  auto &s = state();
  const auto lock = std::scoped_lock(s.mutex);
  if (group->owner != nullptr) {
    std::erase(record_of(group->owner)->open_crews, group);
  }
  free_crew_locked(group);
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
  blocking_wait(s, lock, [&] { return std::ranges::any_of(all, is_done); });
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

void release(uint64_t *task) {
  if (std::atomic_ref(task[co::k_record_slot])
          .load(std::memory_order_acquire) == 0) {
    // Nothing but the program ever held it: a generator, or a task nobody
    // started. Its dropper has already resumed it to the end.
    free_coroutine(task);
    return;
  }
  auto &s = state();
  const auto lock = std::scoped_lock(s.mutex);
  auto *rec = record_of(task);
  rec->released = true;
  free_if_unused_locked(task, *rec);
}

void hold(uint64_t *task) {
  auto &s = state();
  const auto lock = std::scoped_lock(s.mutex);
  ++record_of(task)->holds;
}

void unhold(uint64_t *task) {
  auto &s = state();
  const auto lock = std::scoped_lock(s.mutex);
  unhold_locked(task);
}

auto is_finished(const uint64_t *task) -> bool {
  auto &s = state();
  const auto lock = std::scoped_lock(s.mutex);
  return is_done(task);
}

void wait_idle() {
  auto &s = state();
  auto lock = std::unique_lock(s.mutex);
  s.finished.wait(lock, [&] { return s.unfinished.empty(); });
}

auto shutdown() -> bool {
  if (is_worker) {
    return false;
  }
  auto &s = state();
  auto lock = std::unique_lock(s.mutex);
  // A copy: a cancel can finish a task, which edits the set.
  const auto tasks = std::vector(s.unfinished.begin(), s.unfinished.end());
  for (auto *task : tasks) {
    cancel_locked(s, task, *record_of(task));
  }
  return s.finished.wait_for(lock, k_shutdown_grace,
                             [&] { return s.unfinished.empty(); });
}

auto on_worker() -> bool { return is_worker; }

void report_panic(std::string_view message) {
  if (std::exchange(panic_reported, true)) {
    return;
  }
  // One write, so panics on two workers do not interleave.
  const auto line = std::string("panic: ").append(message).append("\n");
  (void)::write(2, line.data(), line.size());
}

void exit_after_panic() {
  (void)shutdown();
  std::_Exit(k_panic_exit_code);
}

void raise_native_panic(std::string_view message) {
  report_panic(message);
  if (!is_worker) {
    exit_after_panic();
  }
  throw task_panic{};
}

} // namespace cinder::runtime::scheduler

namespace {

/// Runs a blocking wait for generated code. A panic that reaches a thread
/// that is not a worker has no awaiter left, so it ends the process; the
/// other tasks are stopped while the entry function's frame is still alive.
template <typename wait_function> void blocking(wait_function wait) {
  namespace sched = cinder::runtime::scheduler;
  try {
    wait();
  } catch (const sched::task_panic &) {
    if (sched::on_worker()) {
      throw;
    }
    sched::exit_after_panic();
  }
}

} // namespace

extern "C" {
auto cinder_rt_task_block_on(uint64_t *task, uint64_t pool) -> uint64_t * {
  namespace sched = cinder::runtime::scheduler;
  blocking([&] {
    sched::block_on(task, static_cast<sched::pool>(pool),
                    sched::native_runner());
  });
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

auto cinder_rt_task_release(uint64_t *task) -> uint64_t * {
  cinder::runtime::scheduler::release(task);
  return nullptr;
}

auto cinder_rt_task_is_finished(uint64_t *task) -> uint32_t {
  return cinder::runtime::scheduler::is_finished(task) ? 1U : 0U;
}

auto cinder_rt_crew_new() -> uint64_t * {
  return reinterpret_cast<uint64_t *>(
      cinder::runtime::scheduler::crew_new()); // NOLINT
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

auto cinder_rt_atomic_load(uint64_t *word) -> uint64_t {
  return std::atomic_ref(*word).load();
}

auto cinder_rt_atomic_store(uint64_t *word, uint64_t value) -> uint64_t * {
  std::atomic_ref(*word).store(value);
  return nullptr;
}

auto cinder_rt_atomic_compare_exchange(uint64_t *word, uint64_t expected,
                                       uint64_t desired) -> uint64_t {
  (void)std::atomic_ref(*word).compare_exchange_strong(expected, desired);
  return expected;
}

auto cinder_rt_word_wait(uint64_t *word, uint64_t expected) -> uint64_t * {
  cinder::runtime::scheduler::wait_word(word, expected);
  return nullptr;
}

auto cinder_rt_word_waiter(uint64_t *word, uint64_t expected) -> uint64_t * {
  return cinder::runtime::scheduler::word_waiter(word, expected);
}

auto cinder_rt_word_wake(uint64_t *word) -> uint64_t * {
  cinder::runtime::scheduler::wake_word(word);
  return nullptr;
}

auto cinder_rt_crew_join_poll(uint64_t *self, uint64_t *group) -> uint32_t {
  namespace sched = cinder::runtime::scheduler;
  return sched::crew_join_poll(self,
                               reinterpret_cast<sched::crew *>(group)) // NOLINT
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
  blocking([&] {
    sched::crew_join(reinterpret_cast<sched::crew *>(group), // NOLINT
                     sched::pool::io, sched::native_runner());
  });
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
  blocking([&] {
    sched::block_on_all(tasks, static_cast<size_t>(count), sched::pool::io,
                        sched::native_runner());
  });
  return nullptr;
}

auto cinder_rt_task_block_on_any(uint64_t *const *tasks, uint64_t count)
    -> uint64_t * {
  namespace sched = cinder::runtime::scheduler;
  blocking([&] {
    sched::block_on_any(tasks, static_cast<size_t>(count), sched::pool::io,
                        sched::native_runner());
  });
  return nullptr;
}
}
