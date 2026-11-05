# Threading and Task Scheduling Plan

This plan adds a shared task runtime to the bytecode VM and the AOT backend so that `async`/`await`, `crew`, `par`, `race`, and `on` behave the same on both. Steps 1 through 5 are done on both backends: there is a scheduler with `io` and `cpu` worker pools, a `pending` coroutine status with wakers, a thread-safe allocator, a VM that polls tasks from several threads, and lowering for `crew`, `par`, `race`, `spawn`, and `on`. Step 6, `send`/`share` and crew-scoped borrows, is partly done. What remains is the rest of the step 6 checking, the synchronized cells and channels, a real `io` event loop, and a few lowering gaps.

## Current state

The runtime is in `src/runtime/scheduler.{h,cpp}`. Both backends call it through a C ABI: the LLVM backend calls it directly and the VM calls it through intrinsics.

- **Pools.** The `io` pool has one worker unless `CINDER_IO_WORKERS` raises it, so `io` tasks interleave but do not run in parallel by default. The stress tests raise it. The `cpu` pool has one worker per hardware thread, which `CINDER_CPU_WORKERS` can override. `on(pool)` maps to the `cpu` pool.
- **Waiting.** `await` on an unfinished task returns `pending` and the awaiting task is re-queued when the awaited one finishes. Outside a task, `block_on`, `block_on_all`, and `block_on_any` block the calling thread. A worker that blocks in a plain function is replaced in its pool by another worker, so a blocking wait inside a task does not deadlock.
- **Entry point.** `async def main` runs on the calling thread and blocks on its tasks. Tasks still unfinished when it returns are cancelled and stopped before the process exits.
- **VM.** The VM does not have a separate `execution` object. `vm::poll_task` gives each poll its own call frames, so several workers can poll tasks of one `vm` at once. The module is read-only, and `globals_` is the only state the polls share.
- **Allocator.** `cinder_heap_alloc`, `realloc`, and `free` are thread-safe because they sit directly on the system allocator. A test in `allocator_test.cpp` allocates, grows, and frees from several threads at once.
- **Stacks.** Compiled tasks get an 8 MiB stack.
- **Panics.** A panic in a task is recorded as the task's failure and propagates to the awaiter. In `par` and `crew`, a panic cancels the sibling tasks, and the panic propagates once they have stopped.
- **ThreadSanitizer.** `.bazelrc` has a `tsan` config for the scheduler tests.

The user-facing behavior is described in the specification: chapter 27 for `async`/`await`, chapter 28 for `crew`/`par`/`race`/`on`, and chapter 30 for `send`/`share`.

## Steps

1. **Scheduler with one thread.** Done. This step added the scheduler, the `pending` status, wakers, and the `task` type.
2. **VM split.** Done, in a smaller form than planned. Each poll gets its own frames inside `vm::poll_task` instead of a separate `execution` object.
3. **Thread-safe allocator.** Done. The system allocator provides thread safety, so no extra mutex was needed. The multi-threaded stress test was added.
4. **Worker pool.** Done. The `cpu` pool is sized to the hardware, and the `io` pool can be raised for tests.
5. **`crew`, `par`, `race`, `on` lowering.** Done on both backends, including `task.spawn()` and `crew c(on_error: collect)` with `c.errors()`. `race` cancels its losers without waiting for them. Every way of leaving a crew's body joins the crew, including `return`, `?`, `break`, `continue`, and cancellation.
6. **`send`/`share` and crew-scoped borrows.** Partly done.
   - Done: `send` and `share` are marker traits that the checker derives structurally. The check covers the arguments of calls handed to `c.spawn`, `par`, and `race`, and the outer locals read by an `async:` block or an `on(pool)` body. A task that carries a raw pointer is rejected. The ownership checker keeps a crew's spawned borrows live until the crew body ends, including for a task bound to a name before `c.spawn(t)`. As a result, two tasks cannot both hold `&mut` to the same value.
   - Not done: the captures of a closure passed as a task, `channel.send`, the captures of a function-typed value, and ownership of non-copy captures in an `on(ctx)` body.

## Remaining work

The remaining work is listed below, in no particular order.

- Finish the step 6 checks listed above.
- Add `mutex[T]`, `rwlock[T]`, and `atomic[T]` to `src/std/` (chapter 30), and channels (chapter 29). `std.mem` already has the atomic operations.
- Give the `io` context a real event loop. Until then `io` tasks run on the single-threaded driver.
- Allow `await` inside an `on(ctx)` body, and add its sender form.
- Lower a crew exit that sits where a task cannot suspend, such as inside an `if` or `match` that produces a value. This is refused today.
- Support `c.errors()` inside a generic function.

## Risks

- Differential testing between the VM and AOT cannot detect a race that both tiers hit the same way. Concurrency tests should use `# expect:` values and the `tsan` config.
- Until the step 6 checks are finished, a closure or function value passed as a task can carry data that is not safe to share to another worker, and the compiler will not reject it.
- Globals are shared across workers without locks on both tiers. This relies on statics being immutable: the ownership checker rejects any write, `&mut`, `mut self` call, or move through a static (chapter 31). Chapter 26 has no rules of its own for globals.

## Resolved questions

- The default `cpu` pool size is the hardware thread count, and `CINDER_CPU_WORKERS` can override it.
- `async def main` runs on the calling thread.
