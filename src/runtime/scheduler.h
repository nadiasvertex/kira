#pragma once

#include <cstddef>
#include <cstdint>

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
//  the process.
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
/// the failure of a task that panicked. Must not be called from a worker.
void block_on(uint64_t *task, pool where, runner how);

/// Called by the step function of `self` at an `await` of `awaited`. Returns
/// true when `awaited` has finished and its value can be read (rethrowing its
/// failure if it panicked), and also when `self` was cancelled, so that the
/// body can proceed to its cancellation check. Returns false when `awaited`
/// has not finished: `self` is now registered as its waiter, `awaited` is
/// queued if nothing had started it, and the step function must return
/// `status::pending`.
[[nodiscard]] auto await_poll(uint64_t *self, uint64_t *awaited) -> bool;

/// `await_poll` for a set of tasks (`race`): true as soon as any of the
/// `count` tasks in `tasks` has finished (rethrowing its failure if it
/// panicked) or `self` was cancelled. Otherwise `self` is registered as a
/// waiter on every unfinished task, unstarted ones are queued, and the step
/// function must return `status::pending`.
[[nodiscard]] auto await_any_poll(uint64_t *self, uint64_t *const *tasks,
                                  size_t count) -> bool;

/// `await_poll` for a set of tasks that must all finish (`par`): true once
/// every task in `tasks` has finished, or `self` was cancelled. If one of
/// them panicked, the others are cancelled and the failure is rethrown.
/// Otherwise `self` waits on every unfinished task and the step function must
/// return `status::pending`.
[[nodiscard]] auto await_all_poll(uint64_t *self, uint64_t *const *tasks,
                                  size_t count) -> bool;

/// `block_on` for a set of tasks that must all finish: blocks until they
/// have, or until one panics (cancelling the rest and rethrowing).
void block_on_all(uint64_t *const *tasks, size_t count, pool where, runner how);

/// The tasks spawned into one `crew` block. Owned by the block that made it,
/// which frees it with `crew_free` once it has joined.
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

/// `block_on` for a set of tasks: blocks until any one has finished.
void block_on_any(uint64_t *const *tasks, size_t count, pool where, runner how);

/// Asks a task the scheduler owns to stop at its next cancellation check and
/// unwind. Returns false, having done nothing, for a task the scheduler
/// never took over; the caller then resumes it itself. A task that already
/// finished is left alone and counts as handled.
[[nodiscard]] auto cancel(uint64_t *task) -> bool;

/// `cancel` for a program that dropped its handle to `task`. A task spawned
/// into a `crew` is left running: the block that owns it waits for it.
[[nodiscard]] auto cancel_on_drop(uint64_t *task) -> bool;

/// Whether `task` has finished (its body returned or was cancelled).
[[nodiscard]] auto is_finished(const uint64_t *task) -> bool;

/// Blocks the calling thread until every task the scheduler has taken over
/// has finished. Used by tests to drain detached work; a program's tasks are
/// normally awaited.
void wait_idle();

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
}
