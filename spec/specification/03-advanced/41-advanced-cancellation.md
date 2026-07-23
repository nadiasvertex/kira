# 41. Advanced Cancellation

**Status:** Planned

Cooperative cancellation mechanics for tasks: the per-task cancellation token, explicit checking, `await yield` as a suspension-and-check point, and cancelling a `crew` from outside.

See [Concurrency Execution Model](../intermediate/26-concurrency-execution-model.md) for the basic cancellation-scope description (a `crew`'s tasks are cancelled together when the scope exits abnormally). This chapter covers the mechanics beneath that model.

## Design

Every task carries a cancellation token inherited from its parent `crew`. Cancellation is cooperative: nothing preempts a running task, so a task must check for cancellation itself, either explicitly or at a suspension point.

```kira
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

```kira
let handle = crew c:
    c.spawn(long_work())

handle.cancel()
await handle    # waits for clean shutdown
```

`handle.cancel()` requests cancellation of every task spawned into the crew; it does not itself suspend. `await handle` suspends until the crew's tasks have observed cancellation and shut down — cancellation is requested, not instantaneous, so a task that never checks its token or reaches an `await yield` runs to completion regardless.

## Implementation status

Not implemented. This depends on the concurrency runtime described in `spec/concurrency-implementation-plan.md`: today `crew` bodies type-check only against an unknown placeholder type, `async`/`await` surface syntax parses but `await` is typed as a pass-through with no scheduling semantics, and neither the bytecode VM nor the AOT runtime has a scheduler, threads, or task/handle machinery to cancel. There is no cancellation token type, no `cancel.is_requested()`, and no `handle.cancel()` anywhere in `src/`. Cancellation is planned as part of the concurrency plan's Phase 5 (`crew`, `par`, `race`, error handling, external cancellation), which itself depends on the earlier phases (task typing, `send`/`share`, crew borrow rules, the HIR/scheduler core) landing first.

## See also

- [Concurrency Execution Model](../intermediate/26-concurrency-execution-model.md) — the basic `crew`/cancellation-scope model this chapter extends; avoid duplicating its description here.
- [Advanced Concurrency: Execution Graphs](40-execution-graphs.md) — another advanced-layer feature blocked on the same runtime.
