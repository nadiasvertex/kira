# 41. Advanced Cancellation

**Status:** Planned

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

Not implemented. The task runtime exists and cancels tasks internally: `race` cancels its losers, a panicking `par` branch or crew task cancels its siblings, dropping an unfinished task cancels it, and cancelling the task that runs a `crew` block cancels the crew's tasks ([`crew`, `par`, `race`](../02-intermediate/28-crew-par-race.md#implementation-status)). None of this chapter's user-facing surface exists: there is no cancellation token type, `cancel.is_requested()` does not resolve (`cancel` is only a reserved prelude name), a `crew` block yields no handle with `cancel()`, and `await yield` parses and type-checks but is not lowered on either backend (`spec/todo.md` item 15).

## See also

- [Concurrency Execution Model](../02-intermediate/26-concurrency-execution-model.md) — the basic `crew`/cancellation-scope model this chapter extends; avoid duplicating its description here.
- [Advanced Concurrency: Execution Graphs](40-execution-graphs.md) — another advanced-layer feature blocked on the same runtime.
