# 41. Advanced Cancellation

**Status:** Partial

Cooperative cancellation mechanics for tasks: the per-task cancellation token, explicit checking, `await yield` as a suspension-and-check point, and cancelling a `crew` from outside.

See [Concurrency Execution Model](../02-intermediate/26-concurrency-execution-model.md) for the basic cancellation-scope description (a `crew`'s tasks are cancelled together when the scope exits abnormally). This chapter covers the mechanics beneath that model.

## Design

Every task carries a cancellation token inherited from its parent `crew`. Cancellation is cooperative: nothing preempts a running task, so a task must check for cancellation itself, either explicitly or at a suspension point.

```cinder
async def long_work() -> result[unit, cancelled]:
    for i in 0..1_000_000:
        if cancel.is_requested(): return @err(@cancelled)
        step(i)
        await yield    # explicit yield + cancellation check
    return @ok(unit)
```

- `cancel.is_requested()` polls the current task's token without suspending.
- `await yield` is a suspension point that does no real waiting — it gives the scheduler a chance to run other work, and checks for cancellation as part of resuming. It is the idiomatic way to make a CPU-bound loop cancellable and cooperative without an actual I/O wait.
- A task that observes cancellation returns an error result (`@err(@cancelled)`) along its ordinary `result`-based error path — cancellation is not a hidden control-flow mechanism, it surfaces through the same channel every other failure does.

Cancelling a `crew` from outside:

```cinder
let handle = crew c:
    c.spawn(long_work())

handle.cancel()
await handle    # waits for clean shutdown
```

`handle.cancel()` requests cancellation of every task spawned into the crew; it does not itself suspend. `await handle` suspends until the crew's tasks have observed cancellation and shut down — cancellation is requested, not instantaneous, so a task that never checks its token or reaches an `await yield` runs to completion regardless.

## Implementation status

Partial. `await yield` is built on both backends; the token, explicit checks and cancelling a crew from outside are not.

- `await yield` suspends the task, puts it at the back of its pool's queue, and checks for cancellation when it resumes ([The Task Scheduler](../05-implementation/69-task-scheduler.md)). In the entry `async def main`, which is not a task, it does nothing.
- A task cancelled at `await yield` unwinds, as it does at any other `await` ([Concurrency Execution Model](../02-intermediate/26-concurrency-execution-model.md#cancellation)): it runs its drops and returns. It does not return `@err(@cancelled)` as the design above describes. The two chapters differ on this point, and there is no `cancelled` type to return.
- Cancellation itself is cooperative and works today: dropping an unfinished task, a failing sibling in a `crew`, or losing a `race` cancels a task, which stops at its next `await`.
- There is no `cancel` token, no `cancel.is_requested()`, and no `handle.cancel()`.

## See also

- [Concurrency Execution Model](../02-intermediate/26-concurrency-execution-model.md) — the basic `crew`/cancellation-scope model this chapter extends; avoid duplicating its description here.
- [Advanced Concurrency: Execution Graphs](40-execution-graphs.md) — another advanced-layer feature blocked on the same runtime.
