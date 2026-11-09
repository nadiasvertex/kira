#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace cinder::runtime::scheduler {

// ==========================================================================
//  The task scheduler, shared by the bytecode VM and the LLVM tiers.
//
//  A *task* is a coroutine object (`coroutine.h`) produced by an `async def`.
//  It is inert until the scheduler takes it over, either because a program
//  spawns it or because another task or the program's entry point awaits it.
//  From then on a worker thread owns each poll of it: it calls the task's
//  step function, and acts on the status that comes back.
//
//  Workers belong to a *pool*, one per execution context (`26-concurrency-
//  execution-model.md`). The `io` pool has a single worker, so `io` tasks
//  interleave but never run in parallel. The `cpu` pool has one worker per
//  hardware thread (`CINDER_CPU_WORKERS` overrides this) and runs tasks in
//  parallel. Pools start their threads on first use and live for the rest of
//  the process. A pool's size limits how many polls run at once, not how many
//  threads it has: a worker blocked in a plain function does not count.
//
//  All scheduling state sits behind one mutex. A step function runs outside
//  it, so the lock is held only for queue and bookkeeping operations.
// ==========================================================================

/// Which pool a task runs on. The values are the `pool` operand generated
/// code passes to `cinder_task_spawn`.
enum class pool : uint8_t {
  io = 0,
  cpu = 1,
};

/// How a task's step function is run. The VM and the native tiers differ
/// here, and a process may hold tasks from both, so each task remembers the
/// runner it was spawned with.
///
/// `run` returns the step function's status word (`coroutine::raw`). It may
/// throw: the scheduler stores the exception as the task's failure and
/// rethrows it in whoever awaits the task.
struct runner {
  auto (*run)(void *context, uint64_t *task) -> uint64_t = nullptr;
  void *context = nullptr;
};

/// The runner for compiled code, where the task's step function slot holds a
/// native function pointer.
[[nodiscard]] auto native_runner() -> runner;

/// Takes over `task` and queues it on `where`, if the scheduler has not
/// already. Spawning a task twice does nothing the second time.
void spawn(uint64_t *task, pool where, runner how);

/// Runs `task` to completion for a caller that is not itself a task: spawns
/// it if needed and blocks the calling thread until it finishes. Rethrows
/// the failure of a task that panicked. On a worker, which is how a plain
/// function a task called waits, another worker takes this one's place until
/// the wait ends. The other blocking calls below do the same.
void block_on(uint64_t *task, pool where, runner how);

/// Called by the step function of `self` at an `await` of `awaited`. Returns
/// true when `awaited` has finished and its value can be read (rethrowing its
/// failure if it panicked). Returns false when `awaited` has not finished:
/// `self` is now registered as its waiter, `awaited` is queued if nothing had
/// started it, and the step function must return `status::pending`. If `self`
/// was cancelled, `awaited` is cancelled too and the wait lasts until it has
/// stopped; `true` then leads the body to its cancellation check.
[[nodiscard]] auto await_poll(uint64_t *self, uint64_t *awaited) -> bool;

/// `await_poll` for a set of tasks (`race`): true as soon as any of the
/// `count` tasks in `tasks` has finished. Otherwise `self` is registered as a
/// waiter on every unfinished task, unstarted ones are queued, and the step
/// function must return `status::pending`. If one of them panicked or `self`
/// was cancelled, the others are cancelled and it is true once they have all
/// stopped, rethrowing the failure if there is one.
[[nodiscard]] auto await_any_poll(uint64_t *self, uint64_t *const *tasks,
                                  size_t count) -> bool;

/// `await_poll` for a set of tasks that must all finish (`par`): true once
/// every task in `tasks` has finished. Until then `self` waits on every
/// unfinished task and the step function must return `status::pending`. If
/// one of them panicked or `self` was cancelled, the others are cancelled;
/// once they have stopped the failure, if any, is rethrown.
[[nodiscard]] auto await_all_poll(uint64_t *self, uint64_t *const *tasks,
                                  size_t count) -> bool;

/// `block_on` for a set of tasks that must all finish: blocks until they
/// have. If one panics the rest are cancelled, and the failure is rethrown
/// once they have stopped.
void block_on_all(uint64_t *const *tasks, size_t count, pool where, runner how);

/// The tasks spawned into one `crew` block. Owned by the block that made it,
/// which frees it with `crew_free` once it has joined. If a task's body
/// panics with a crew open, the scheduler cancels the crew's tasks, frees it,
/// and finishes the panicking task only once they have stopped.
struct crew;

[[nodiscard]] auto crew_new() -> crew *;

/// Starts `task` and records it in `group`, which is only touched by the
/// task or thread that runs the `crew` block.
void crew_spawn(crew *group, uint64_t *task, pool where, runner how);

/// `await_all_poll` over everything spawned into `group`.
[[nodiscard]] auto crew_join_poll(uint64_t *self, crew *group) -> bool;

/// `block_on_all` over everything spawned into `group`.
void crew_join(crew *group, pool where, runner how);

void crew_free(crew *group);

/// How many tasks have been spawned into `group`, and the `index`-th of them.
[[nodiscard]] auto crew_size(const crew *group) -> size_t;
[[nodiscard]] auto crew_task(const crew *group, size_t index) -> uint64_t *;

/// `block_on` for a set of tasks: blocks until any one has finished. If a
/// finished one panicked, the rest are cancelled and the failure is rethrown
/// once they have stopped.
void block_on_any(uint64_t *const *tasks, size_t count, pool where, runner how);

/// Asks a task the scheduler owns to stop at its next cancellation check and
/// unwind. Returns false, having done nothing, for a task the scheduler
/// never took over; the caller then resumes it itself. A task that already
/// finished is left alone and counts as handled.
[[nodiscard]] auto cancel(uint64_t *task) -> bool;

/// `cancel` for a program that dropped its handle to `task`. A task spawned
/// into a `crew` is left running: the block that owns it waits for it.
[[nodiscard]] auto cancel_on_drop(uint64_t *task) -> bool;

/// The program dropped its handle to `task`, having cancelled it first if
/// it had not finished. The task is freed now if the scheduler never took it
/// over. Otherwise it is freed once it has finished and nothing else names
/// it: no crew it was spawned into is still open and no `hold` remains.
void release(uint64_t *task);

/// Keeps a task the scheduler owns from being freed while something outside
/// the scheduler, such as a reactor wait list, still names it. Each `hold`
/// is ended by one `unhold`.
void hold(uint64_t *task);
void unhold(uint64_t *task);

/// Whether `task` has finished (its body returned or was cancelled).
[[nodiscard]] auto is_finished(const uint64_t *task) -> bool;

// --------------------------------------------------------------------------
//  Waiting on a memory word. These are what `std.sync` builds its locks and
//  channels from: a lock or a queue keeps its state in a `usize`, changes it
//  with an atomic operation, and wakes the word's waiters after a change.
//  A wait re-reads the word under the scheduler's lock, and a wake takes
//  that lock, so a change made before a wake is never missed.
// --------------------------------------------------------------------------

/// Blocks the calling thread while the word at `word` holds `expected`. On
/// a worker, another worker takes this one's place, as in `block_on`.
void wait_word(const uint64_t *word, uint64_t expected);

/// A task that finishes once the word at `word` no longer holds `expected`,
/// for a task body to `await`. It is a coroutine object of its own whose
/// step function re-reads the word; the scheduler has already taken it over.
[[nodiscard]] auto word_waiter(const uint64_t *word, uint64_t expected)
    -> uint64_t *;

/// Wakes every wait on `word`: each re-reads it and either returns or waits
/// again.
void wake_word(const uint64_t *word);

/// Queues `task` again if it returned `pending`, or has it polled once more
/// if it is mid-poll. For a task that waits on something outside the
/// scheduler, such as the reactor (`reactor.h`). Does nothing to a task the
/// scheduler does not own or that has finished.
void wake(uint64_t *task);

/// Blocks the calling thread until every task the scheduler has taken over
/// has finished. Used by tests to drain detached work; a program's tasks are
/// normally awaited.
void wait_idle();

/// Cancels every unfinished task and waits a short time for them to stop.
/// Returns whether they all did: a task that never reaches a cancellation
/// check is still running if not. A worker cannot wait for itself, so there
/// it does nothing and returns false. Runs at process exit; a host that frees
/// what tasks point at calls it first.
[[nodiscard]] auto shutdown() -> bool;

/// Whether the calling thread is a worker, where a panic unwinds to the
/// scheduler rather than ending the process.
[[nodiscard]] auto on_worker() -> bool;

/// Thrown through compiled frames by a panic on a worker. It carries nothing:
/// the message is printed where the panic was raised.
struct task_panic {};

/// Prints `panic: <message>` on stderr, once per poll of a task, so a panic
/// rethrown in its awaiters is not printed again.
void report_panic(std::string_view message);

/// Ends the process after a panic with nothing left to propagate to: stops
/// the tasks, then exits without running static destructors.
[[noreturn]] void exit_after_panic();

/// The panic of compiled code: reports `message`, then throws `task_panic` on
/// a worker or ends the process elsewhere.
[[noreturn]] void raise_native_panic(std::string_view message);

} // namespace cinder::runtime::scheduler

extern "C" {
/// The C-ABI face of the calls above, for generated code. These are the
/// native symbols of the `rt_task_*` intrinsics (`src/intrinsics.h`), so they
/// use the intrinsic ABI: a coroutine object crosses as a pointer to its
/// slots, `pool` is a `scheduler::pool` value, and a returned pointer stands
/// for Cinder's `unit`. They run tasks with the native runner; the VM calls
/// the C++ functions above with its own.
auto cinder_rt_task_block_on(uint64_t *task, uint64_t pool) -> uint64_t *;
auto cinder_rt_task_await_poll(uint64_t *self, uint64_t *awaited) -> uint32_t;
auto cinder_rt_task_spawn(uint64_t *task, uint64_t pool) -> uint64_t *;
auto cinder_rt_task_cancel(uint64_t *task) -> uint32_t;
auto cinder_rt_task_drop(uint64_t *task) -> uint32_t;
auto cinder_rt_task_release(uint64_t *task) -> uint64_t *;
auto cinder_rt_task_is_finished(uint64_t *task) -> uint32_t;
auto cinder_rt_task_await_any_poll(uint64_t *self, uint64_t *const *tasks,
                                   uint64_t count) -> uint32_t;
auto cinder_rt_task_block_on_any(uint64_t *const *tasks, uint64_t count)
    -> uint64_t *;
auto cinder_rt_task_await_all_poll(uint64_t *self, uint64_t *const *tasks,
                                   uint64_t count) -> uint32_t;
auto cinder_rt_task_block_on_all(uint64_t *const *tasks, uint64_t count)
    -> uint64_t *;
auto cinder_rt_crew_new() -> uint64_t *;
auto cinder_rt_crew_spawn(uint64_t *group, uint64_t *task) -> uint64_t *;
auto cinder_rt_crew_join_poll(uint64_t *self, uint64_t *group) -> uint32_t;
auto cinder_rt_crew_join(uint64_t *group) -> uint64_t *;
auto cinder_rt_crew_free(uint64_t *group) -> uint64_t *;
auto cinder_rt_crew_len(uint64_t *group) -> uint64_t;
auto cinder_rt_crew_task(uint64_t *group, uint64_t index) -> uint64_t *;

/// The word operations of `std.sync`: sequentially consistent atomic access
/// to a `usize`, and `wait_word`/`word_waiter`/`wake_word` above.
auto cinder_rt_atomic_load(uint64_t *word) -> uint64_t;
auto cinder_rt_atomic_store(uint64_t *word, uint64_t value) -> uint64_t *;
auto cinder_rt_atomic_compare_exchange(uint64_t *word, uint64_t expected,
                                       uint64_t desired) -> uint64_t;
auto cinder_rt_word_wait(uint64_t *word, uint64_t expected) -> uint64_t *;
auto cinder_rt_word_waiter(uint64_t *word, uint64_t expected) -> uint64_t *;
auto cinder_rt_word_wake(uint64_t *word) -> uint64_t *;
}
