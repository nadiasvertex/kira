# 69. The Task Scheduler

**Status:** Partial

The shared runtime that runs tasks on worker threads for both backends. It lives in `src/runtime/scheduler.{h,cpp}`, behind a C++ interface the VM uses and a C ABI that compiled code calls.

## Pools and workers

A task runs in a pool, one per execution context ([The Execution Model](../02-intermediate/26-concurrency-execution-model.md)). The `io` pool has one worker thread, so `io` tasks interleave but do not run in parallel. The `cpu` pool has one worker per hardware thread. `CINDER_IO_WORKERS` and `CINDER_CPU_WORKERS` override the counts; the test harness sets the first to run `io` tasks in parallel. A pool starts its threads when it first has work and keeps them for the life of the process. All scheduling state is behind one mutex, and a step function runs outside it.

Every `async def` is an `io` task today. `await on(cpu): ...` starts a task in the `cpu` pool.

## Task lifecycle

A task is a coroutine object ([HIR and the Bytecode VM](66-hir-and-bytecode-vm.md)). It is inert until the scheduler takes it over, which happens when a program calls `spawn`, when another task awaits it, or when the entry function blocks on it. Taking a task over allocates a record for it (never freed, like the task itself) and queues it.

A worker pops a task and calls its step function through the runner the task was spawned with: the VM's runner polls the task on the VM, and the native runner calls the compiled function pointer. The status it returns decides what happens next:

- `suspended` or `done`: the task produced its result or finished. The scheduler marks it done and moves its waiters to the run queue.
- `pending`: the task awaited something unfinished and registered itself as a waiter. If a wake arrived while the step function was still running, the task is queued again at once; otherwise it waits.
- an exception: the scheduler stores it as the task's failure and finishes the task.

## Awaiting

`await_poll(self, awaited)` is called by a task's step function at each `await`. It returns true if `awaited` is done (rethrowing its failure if it panicked). Otherwise it queues `awaited` if nothing had started it, adds `self` to its waiters, and returns false, and the step function returns `pending`. On wake the step function resumes at the same `await` and polls again, so a spurious wake is harmless. `await_any_poll` does the same for a set of tasks and is true when any one is done; it backs `race`. `await_all_poll` is true when every task is done; if one has panicked it cancels the others and rethrows. It backs `par`, with `block_on_all` for callers that are not tasks. `block_on` serves callers that are not tasks, such as the entry `async def main`: it queues the task and blocks the calling thread until it is done.

A panic inside a task does not end the process. It is rethrown in the task or function that awaits it, so it propagates up the chain of awaits to the entry function.

## Cancellation

`cancel(task)` marks a task the scheduler owns so that it unwinds. A queued task is flagged, a waiting task is flagged and queued, and a running task is flagged and handled when its step function returns. The flag is the same `cancel_requested` status a dropped generator sees, so the body runs its drops and returns at its next cancellation check. An `await` treats a cancelled task as ready, so the check after it runs. A task the scheduler does not own is resumed by its dropper directly, as before.

## Allocator

`cinder_heap_alloc` and its companions are safe to call from any thread. The system allocator inherits that from the C library; the arena allocator takes a lock around each bump.

## Testing

`scheduler_test` drives the scheduler with fake step functions, including 400 tasks on a four-worker pool. `allocator_test` runs eight threads against the allocator, with the system and the arena allocator. Corpus files `150`–`153` and `155` (`on(cpu)`) run awaiting tasks, multiword results, `par`, `race` and a shared atomic counter on both backends, and the harness runs them with four `io` workers. `bazelisk test --config=tsan` builds under ThreadSanitizer.

## Not built

`crew` is not lowered, and `on` bodies cannot `await`. `par` waits for its branches in order and does not cancel the rest when one fails. `send` and `share` are not checked, so nothing yet stops a program from handing non-shareable data to a task that runs on another thread. There is no event loop behind `io`.
