# Threading and Task Scheduling Plan

This plan adds a shared task runtime to the bytecode VM and the AOT backend so that `async`/`await`, `crew`, `par`, `race`, and `on` behave the same on both. Every step is done on both backends: a scheduler with `io` and `cpu` worker pools, a `pending` coroutine status with wakers, a thread-safe allocator, a VM that polls tasks from several threads, lowering for `crew`, `par`, `race`, `spawn`, `async:` blocks and `on`, the `send`/`share` and crew-scoped borrow checks, `std.sync`, and an event loop behind the `io` context. `std.io` has asynchronous file and pipe I/O and `std.time` has sleeping, both waiting in the event loop. No planned work is left. The gaps found along the way are listed in `spec/todo.md`.

## Current state

The runtime is in `src/runtime/scheduler.{h,cpp}` and `src/runtime/reactor.{h,cpp}`. Both backends call it through a C ABI: the LLVM backend calls it directly and the VM calls it through intrinsics.

- **Pools.** The `io` pool has one worker unless `CINDER_IO_WORKERS` raises it, so `io` tasks interleave but do not run in parallel by default. The stress tests raise it. The `cpu` pool has one worker per hardware thread, which `CINDER_CPU_WORKERS` can override. `on(pool)` maps to the `cpu` pool.
- **Waiting.** `await` on an unfinished task returns `pending` and the awaiting task is re-queued when the awaited one finishes. Outside a task, `block_on`, `block_on_all`, and `block_on_any` block the calling thread. A worker that blocks in a plain function is replaced in its pool by another worker, so a blocking wait inside a task does not deadlock.
- **Entry point.** `async def main` runs on the calling thread and blocks on its tasks. Tasks still unfinished when it returns are cancelled and stopped before the process exits.
- **VM.** The VM does not have a separate `execution` object. `vm::poll_task` gives each poll its own call frames, so several workers can poll tasks of one `vm` at once. The module is read-only, and `globals_` is the only state the polls share.
- **Allocator.** `cinder_heap_alloc`, `realloc`, and `free` are thread-safe because they sit directly on the system allocator. A test in `allocator_test.cpp` allocates, grows, and frees from several threads at once.
- **Stacks.** Compiled tasks get an 8 MiB stack.
- **Panics.** A panic in a task is recorded as the task's failure and propagates to the awaiter. In `par` and `crew`, a panic cancels the sibling tasks, and the panic propagates once they have stopped.
- **Event loop.** One reactor thread waits on kqueue (macOS, BSD) or epoll (Linux) and on a timer heap. A task waiting for a descriptor or a deadline is a small waiter task that the reactor wakes through the scheduler, so it holds no worker. `std.io`'s `async_file`, `async_pipe` and `async_reader`/`async_writer`, and `std.time`'s `sleep`, are written in Cinder over it.
- **ThreadSanitizer.** `.bazelrc` has a `tsan` config for the scheduler and reactor tests.

The user-facing behavior is described in the specification: chapter 26 for the `io` context, chapter 27 for `async`/`await`, chapter 28 for `crew`/`par`/`race`/`on`, chapter 30 for `send`/`share`, chapter 54 for asynchronous I/O, and the `std.time` chapter for sleeping. Chapter 69 describes the scheduler and the event loop.

## Steps

1. **Scheduler with one thread.** Done. This step added the scheduler, the `pending` status, wakers, and the `task` type.
2. **VM split.** Done, in a smaller form than planned. Each poll gets its own frames inside `vm::poll_task` instead of a separate `execution` object.
3. **Thread-safe allocator.** Done. The system allocator provides thread safety, so no extra mutex was needed. The multi-threaded stress test was added.
4. **Worker pool.** Done. The `cpu` pool is sized to the hardware, and the `io` pool can be raised for tests.
5. **`crew`, `par`, `race`, `on` lowering.** Done on both backends, including `task.spawn()` and `crew c(on_error: collect)` with `c.errors()`. `race` cancels its losers without waiting for them. Every way of leaving a crew's body joins the crew, including `return`, `?`, `break`, `continue`, and cancellation.
6. **`send`/`share` and crew-scoped borrows.** Done.
   - `send` and `share` are marker traits that the checker derives structurally. The check covers the arguments of calls handed to `c.spawn`, `par`, and `race`, the outer locals read by an `async:` block or a non-`io` `on` body, and function values and tasks that cross to another task, which the checker follows back to where they were made. A task that carries a raw pointer is rejected.
   - The ownership checker keeps a crew's spawned borrows live until the crew body ends, including for a task bound to a name before `c.spawn(t)`, so two tasks cannot both hold `&mut` to the same value. A non-copy local that an `async:` or `on` body consumes moves into the task.
   - The `std.sync` types are `send` and `share` whenever `T: send`, because only one task reaches the value inside at a time. `rwlock[T]` is the exception: its readers share the value, so it is `share` only when `T: share`.
7. **Synchronized cells and channels.** Done. `std.sync` is written in Cinder over atomic access to a `usize` and three runtime calls that wait on a word, wake its waiters, or make a task that finishes when the word changes. `mutex.lock`, `rwlock.read` and `rwlock.write` block the thread; `channel.send`, `channel.recv` and `watch.changed` suspend the task.
8. **Lowering gaps.** Done. An `on(ctx)` body may `await`, and `on(ctx, task)` runs an existing task on `ctx`. A `crew` exit, `await` or other suspension inside an `if` or `match` that produces a value, or inside a loop condition, is lowered. `c.errors()` works inside a generic function, and generic `async def` calls are typed as tasks.

9. **Event loop behind `io`.** Done on both backends. The reactor registers one-shot interest per descriptor and direction, re-arms epoll for the direction that still has waiters, and finishes a waiter at once for a regular file, a directory, or a closed descriptor. Timers sit in a heap, and a wake-up pipe ends the wait early when an earlier timer arrives. Two coroutine bugs it exposed are fixed on both tiers: a `match`, `while let` or `let ... else` subject that is live across an `await` was never stored into its suspended-state slot, and a borrowed temporary passed to an awaited call (`await f(&xs[a..b])`) pointed into the frame that suspended. A data race on a task's status word, between a dropped crew handle and the worker finishing it, is fixed too.

## Remaining work

None from this plan. Follow-ups found during the work are in `spec/todo.md`: items 13 and 15 (an unawaited `on` wrapper leaks its captures; `await yield` is not lowered), 16 (a method that moves out of `self` leaves two owners; `option`/`result` `unwrap` was the standard library's case and has been replaced by `or else`), 17 (borrowed temporaries in coroutines are never freed), and 19 (the corpus cannot run under the `tsan` config past one unrelated file).

## Risks

- Differential testing between the VM and AOT cannot detect a race that both tiers hit the same way. Concurrency tests should use `# expect:` values and the `tsan` config.
- `std.sync` is new and has been run only by the corpus tests and a drop test. Its lock and channel algorithms are simple, but they have not been stress-tested under ThreadSanitizer at high worker counts.
- The event loop has been tested on macOS (kqueue) only. The epoll path, including re-arming a descriptor with waiters in both directions, compiles on Linux but has not been run.
- A cancelled waiter stays in the reactor's lists until its descriptor's next event or its deadline. This is harmless because task objects are never freed. If tasks are ever freed, cancellation has to remove the waiter from the reactor first.
- The checks that follow function values and tasks back to their origin reject any value whose origin they cannot see. This is sound but may refuse correct programs that pass task values through data structures.
- Globals are shared across workers without locks on both tiers. This relies on statics being immutable: the ownership checker rejects any write, `&mut`, `mut self` call, or move through a static (chapter 31). Chapter 26 has no rules of its own for globals.

## Resolved questions

- The default `cpu` pool size is the hardware thread count, and `CINDER_CPU_WORKERS` can override it.
- `async def main` runs on the calling thread.
