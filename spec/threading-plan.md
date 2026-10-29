# Threading and Task Scheduling Plan

This plan adds a shared task runtime to the bytecode VM and the AOT backend so that `async`/`await`, `crew`, `par`, `race`, and `on` behave the same on both. The runtime is a C++ library in `src/runtime/` with a C ABI that the LLVM backend calls directly and the VM calls through intrinsics. It provides a worker thread pool, a task scheduler, a `pending` coroutine status with wakers, and a thread-safe allocator. The VM must stop assuming one thread: its `vm` object is split into shared state (module, globals) and per-task state (frames, registers). The work is ordered in six steps, each of which leaves all tests passing. The largest risks are VM re-entrancy and allocator thread safety.

## Current state

Nothing in the compiler or runtime uses threads. The VM is one `vm` object whose `run` executes a single explicit frame stack. `globals_` is a plain vector. The allocator (`cinder_heap_alloc`/`free`) has no locking. Panics use a thread-local hosting flag. A task today is a coroutine with a `suspended`/`done`/`cancel_requested` status, and `await` resumes it to completion on the calling thread.

## Design

**Runtime library.** `src/runtime/scheduler.{h,cpp}` owns a pool of worker threads and a run queue. A pool is created by `on(pool)` or by the process default. The `cpu` context maps to a fixed-size pool sized to the hardware. The `io` context maps to the single-threaded event loop in step 1 and gains a thread-backed driver later. Both backends call the same entry points: `spawn(task)`, `poll(task)`, `wake(task)`, `block_on(task)`, and `cancel(task)`.

**Coroutine protocol.** `status` gains `pending`. A step function returns `pending` when it awaits a task that has not finished. It registers the awaiting task as a waiter on the awaited task before returning. When the awaited task finishes, the scheduler moves its waiters to the run queue. The resumer, not the step function, owns the waker. This keeps the generated code the same size as it is now.

**VM structure.** `vm` splits into a shared part (module, globals, scheduler handle) and an `execution` object holding frames and registers. Each task polled on a worker gets its own `execution`. `op_coroutine_resume` on a task calls `poll` and, on `pending`, unwinds the current `execution` back to the scheduler instead of recursing. Globals are guarded by the module's own rules from chapter 26 (`share` and `send` checking), not by locks in the VM.

**Allocator.** `cinder_heap_alloc`, `realloc`, and `free` become thread-safe. The first version uses a mutex. A per-thread cache is a later optimization.

**Panics.** A panic inside a worker cancels its crew and is reported to the awaiting task as the task's failure, following chapter 26. The thread-local hosting flag is set per worker.

## Steps

1. **Scheduler with one thread.** Add the scheduler, `pending`, and wakers, with a single worker. `await` on an unfinished task returns `pending` and the awaiting task re-queues. Tests: a task that awaits another that has not started, on both backends.
2. **VM split.** Separate shared state from `execution`. No behavior change; all tests pass.
3. **Thread-safe allocator.** Add the mutex and a stress test that allocates from several threads.
4. **Worker pool.** Raise the worker count. Tests use a `# expect:` value that depends on every task having run, and a run under ThreadSanitizer.
5. **`crew`, `par`, `race`, `on` lowering.** Lower each to scheduler calls. `race` cancels the losers through the existing cancel path.
6. **`send`/`share` and crew-scoped borrows.** Checker and ownership-checker work from chapter 26, required before sharing data across workers is sound.

The `task` library type in `src/std/` is added in step 1, so the scheduler has a typed surface to call.

## Risks

- The VM split touches most of `vm.cpp`. It is done before any threads exist so a regression is not confused with a race.
- Differential testing between the VM and AOT cannot detect a race that both hit the same way. Step 4 uses `# expect:` values and ThreadSanitizer for that reason.
- Results that do not fit one value slot are untested today. Step 1 adds a multiword result test before tasks cross threads.
- The `io` context has no real event loop yet. Until it does, `io` tasks run on the single-threaded driver, and the spec status for chapter 26 says so.

## Open questions

- Worker count for the default `cpu` pool: the hardware thread count, or a configurable value set at startup.
- Whether `async def main` runs on the calling thread or on a worker. The plan assumes the calling thread.
