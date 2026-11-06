# 69. The Task Scheduler

**Status:** Partial

The shared runtime that runs tasks on worker threads for both backends. It lives in `src/runtime/scheduler.{h,cpp}`, behind a C++ interface the VM uses and a C ABI that compiled code calls.

## Pools and workers

A task runs in a pool, one per execution context ([The Execution Model](../02-intermediate/26-concurrency-execution-model.md)). The `io` pool has one worker thread, so `io` tasks interleave but do not run in parallel. The `cpu` pool has one worker per hardware thread. `CINDER_IO_WORKERS` and `CINDER_CPU_WORKERS` override the counts; the test harness sets the first to run `io` tasks in parallel. A pool starts its threads when it first has work and keeps them for the life of the process. A worker has an 8 MiB stack, the usual size of a main thread's, so a task can recurse as deep in compiled code as the entry function can. If no thread can be started and no worker remains to reach the queue, the process prints a panic and exits with status 101. The count limits how many polls run at once, not how many threads exist: a plain function a task called cannot suspend, so a blocking wait there gives the worker's place to another worker, started if none is idle. When the wait ends the worker waits for a place, ahead of the queue, so an `io` pool of one still runs one poll at a time. Each nested blocking wait holds a thread, and the threads stay for the life of the process. All scheduling state is behind one mutex, and a step function runs outside it.

Every `async def` is an `io` task today. `await on(cpu): ...` starts a task in the `cpu` pool.

## Task lifecycle

A task is a coroutine object ([HIR and the Bytecode VM](66-hir-and-bytecode-vm.md)). It is inert until the scheduler takes it over, which happens when a program calls `spawn`, when another task awaits it, or when the entry function blocks on it. Taking a task over allocates a record for it (never freed, like the task itself) and queues it.

A worker pops a task and calls its step function through the runner the task was spawned with: the VM's runner polls the task on the VM, and the native runner calls the compiled function pointer. The status it returns decides what happens next:

- `suspended` or `done`: the task produced its result or finished. The scheduler marks it done and moves its waiters to the run queue.
- `pending`: the task awaited something unfinished and registered itself as a waiter. If a wake arrived while the step function was still running, the task is queued again at once; otherwise it waits.
- an exception: the scheduler stores it as the task's failure and finishes the task.

## Awaiting

`await_poll(self, awaited)` is called by a task's step function at each `await`. It returns true if `awaited` is done (rethrowing its failure if it panicked). Otherwise it queues `awaited` if nothing had started it, adds `self` to its waiters, and returns false, and the step function returns `pending`. On wake the step function resumes at the same `await` and polls again, so a spurious wake is harmless. `await_any_poll` does the same for a set of tasks and is true when any one is done; it backs `race`. `await_all_poll` is true when every task is done; if one has panicked it cancels the others and rethrows once they have stopped. It backs `par`, with `block_on_all` for callers that are not tasks. A `crew` is a runtime object (`crew_new`, `crew_spawn`, `crew_join_poll`/`crew_join`, `crew_free`, and `crew_size`/`crew_task` for reading the tasks back) holding the tasks spawned into one block; joining it is `await_all`/`block_on_all` over them. `crew_spawn` marks each task crew-owned so that dropping the program's handle (`cancel_on_drop`, used by drop glue) does not cancel it. `block_on` serves callers that are not tasks, such as the entry `async def main` or a plain function a task called: it queues the task and blocks the calling thread until it is done. A cancel does not reach a task a plain function is blocked on with `block_on`, so the worker waits for it to finish normally; only the tasks of a crew the task has open are cancelled.

A panic inside a task does not end the process. Both tiers share one path. `report_panic` prints the message once where the panic happens, and on a worker the panic then unwinds to `worker_loop`: the VM throws `panic_error`, and compiled code throws `task_panic` through its frames, which carry unwind tables for this. No landing pads run, so the panicking task's own drops are skipped on both tiers. The failure is rethrown in the task or function that awaits it, so it propagates up the chain of awaits to the entry function. A panic skips the end of any `crew` block the task's body had open, so each task keeps a list of its open crews. A task that fails with crews open enters the `unwinding` state: the scheduler cancels their tasks, finishes the failed task once they have stopped, and frees the crews. Awaiters see the failure only then, so a crew's tasks never outlive the frame they may borrow from. 

A panic on a thread that is not a worker has nowhere to propagate, and that covers one rethrown into the entry function. `exit_after_panic` runs `shutdown` and then `_Exit(101)`, so the entry function's frame outlives its crews and no static destructor runs beside a worker.

## Waiting on a word

`std.sync` builds its locks and channels on three calls that wait on a 64-bit word. `wait_word(word, expected)` blocks the calling thread while the word holds `expected`; on a worker it gives up the worker's place like any other blocking wait. `word_waiter(word, expected)` returns a task that finishes once the word no longer holds `expected`, which a task awaits to suspend instead of block. `wake_word(word)` wakes every wait on the word. A wait re-reads the word under the scheduler's lock and a wake takes the same lock, so a change followed by a wake cannot be missed. The waiters are kept in a table keyed by the word's address. The calls are reached from Cinder as `rt_word_wait`, `rt_word_waiter` and `rt_word_wake`, together with `rt_atomic_load`, `rt_atomic_store` and `rt_atomic_compare_exchange`, which are sequentially consistent.

## Process shutdown

`shutdown` cancels every unfinished task and waits up to one second for them to stop. It reports whether they all did, since a task that never reaches a cancellation check cannot be stopped. It runs at process exit through `atexit`, and `cinder run` calls it before freeing the VM and module; if a task is still running, the driver calls `_Exit` with the program's status instead of freeing what the task reads. A task nobody awaited is therefore cancelled when the entry function returns.

## Cancellation

`cancel(task)` marks a task the scheduler owns so that it unwinds. A queued task is flagged, a waiting task is flagged and queued, and a running task is flagged and handled when its step function returns. The flag is the same `cancel_requested` status a dropped generator sees, so the body runs its drops and returns at its next cancellation check. A cancelled task's `await` cancels what it awaits and stays pending until that has stopped, so the check after it runs with nothing left borrowing from the frame. Cancelling a task also cancels the tasks of every crew it has open, at once, whichever `await` its body is at. A task the scheduler does not own is resumed by its dropper directly, as before.

The losers of a `race` are the exception: they are cancelled but not waited for.

## Allocator

`cinder_heap_alloc` and its companions are safe to call from any thread. The system allocator inherits that from the C library; the arena allocator takes a lock around each bump.

## Testing

`scheduler_test` drives the scheduler with fake step functions, including 400 tasks on a four-worker pool, cancellation reaching awaited tasks and open crews, a panic in a crew body, and `shutdown`. `allocator_test` runs eight threads against the allocator, with the system and the arena allocator. Corpus files `150`–`153`, `155`–`159` and `169`–`173` (`on(cpu)`, `race` cancelling losers, `crew`, `crew` collecting errors, every exit from a `crew` body, `async:` blocks, borrows held across `await`, `std.sync`, suspending value branches, generic tasks) run awaiting tasks, multiword results, `par`, `race` and a shared atomic counter on both backends, and the harness runs them with four `io` workers. `bazelisk test --config=tsan` builds under ThreadSanitizer.

## Not built

There is no event loop behind `io`. A task that waits on the operating system blocks its worker.
