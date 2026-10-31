# 28. `crew`, `par`, `race`

**Status:** Planned

`crew` structured concurrency, `par`, `race`, `on(target)`, and error handling in concurrent code (`on_error: collect`).

## `crew` — Structured Concurrency

`crew` runs multiple async tasks together. A `crew` block waits for all its tasks before continuing. If any task fails, the rest are cancelled and the error propagates.

```cinder
async def fetch_dashboard(user_id: int32) -> result[dashboard, app_error]:
    let (user_task, orders_task, prefs_task) = crew c:
        let user_task   = c.spawn(fetch_user(user_id))
        let orders_task = c.spawn(fetch_orders(user_id))
        let prefs_task  = c.spawn(fetch_preferences(user_id))
        (user_task, orders_task, prefs_task)

    let user   = user_task.get()?
    let orders = orders_task.get()?
    let prefs  = prefs_task.get()?
    return @ok(build_dashboard(user, orders, prefs))
```

The body of a `crew` is its own scope, and its last expression is the value of the block. The block waits for every task spawned into it, so a task handed out as the block's value has already finished when `get()` reads it. `c.spawn(t)` returns `t`; dropping that handle does not cancel the task.

Tasks in a `crew` cannot outlive the `crew` block — enforced by the compiler, so there are no dangling background tasks. This is what lets a closure spawned into a `crew` borrow local data instead of moving it; see [Closures and Capture](16-closures-and-capture.md#crew-scoped-borrowing).

## `par` — Parallel Composition

`par` runs a fixed set of tasks simultaneously and gives all the results as a tuple:

```cinder
let (user, orders, prefs) = await par:
    fetch_user(id)
    fetch_orders(id)
    fetch_preferences(id)
```

If any task fails, the others are cancelled.

## `race`

`race` runs multiple tasks and returns the first to complete, cancelling the rest:

```cinder
let result = await race:
    fetch_from_primary(key)
    fetch_from_replica(key)
```

## Moving Work to a Different Context: `on(target)`

`on(target)` runs a block on another context or executor and returns the result to the caller's context (see [The Execution Model](26-concurrency-execution-model.md)):

```cinder
let result = await on(pool):
    expensive_computation(req.body)
```

## Error Handling in Concurrent Code

Errors from concurrent tasks follow the same `result` model as sequential code. A `crew` that uses `on_error: collect` gathers all errors rather than stopping on the first:

```cinder
let errors = crew c(on_error: collect):
    for item in items:
        let _ = c.spawn(process(item))
    c.errors()

for err in errors:
    log("failed: {err}")
```

`on_error: collect` is the one crew option. It allows `c.errors()` inside the crew's body. The call waits for every task spawned so far, then gives a `list[E]` of the errors they finished with, in spawn order. A task that finishes with `err` does not stop its siblings, and the `ok` results stay readable through `get()`. Every task spawned into a collecting crew must have the same type, `task[T, E]`, so `errors()` has one element type. A panic still cancels the other tasks and propagates, as in any crew.

## Implementation status

Partial. `par`, `race`, `spawn`, `await on(cpu|pool|io):` and `crew` work on both backends; `crew` options work too (`on_error: collect`).

- `await par: ...` starts every branch task, waits for each in order, and gives the tuple of results. `await race: ...` waits until any branch has finished, gives the first finished one in branch order, and cancels the others. Both work inside a task and in the entry `async def main`. If a `par` branch panics, the runtime cancels the other branches and the panic propagates to the awaiting task once they have stopped. `race` cancels its losers without waiting for them.
- `task.spawn()` starts a task without waiting; a later `await` collects its result.
- All of these run on the `io` pool. With the default single `io` worker they interleave; they run in parallel only when `CINDER_IO_WORKERS` is raised (the stress-test harness does this).
- `await on(ctx): body` runs the body as a task on that context's pool (`cpu`/`pool` → cpu pool). The body is lowered to a closure and run by a synthesized task function, so it cannot itself `await`, and it has no sender form. Values it captures are copied in; ownership of non-copy captures is not yet checked (step 6 of the threading plan).
- `crew c: body` binds `c` (a `crew` handle) for the body, runs it, then waits for every task passed to `c.spawn(t)` and frees the handle. It is a statement or an expression whose value is the body's last expression. In a task the wait suspends the task; in an ordinary function it blocks the thread. If a spawned task panics the others are cancelled, and the panic propagates once they have stopped. Every exit from the body waits the same way: `return`, `?`, and a `break` or `continue` out of the block join the crew and free it, and cancelling the task that runs the block cancels the crew's tasks and waits for them. In a task, such an exit is a suspension, so one placed where a task cannot suspend (inside an `if` or `match` that produces a value, for example) is refused as not lowered yet. `t.get()` is `await t`. Spawned tasks run on the `io` pool. With `on_error: collect`, `c.errors()` waits (it suspends the task, or blocks the thread outside one), then reads each spawned task through `rt_crew_len` and `rt_crew_task` and pushes the `err` payloads onto a list. `c.errors()` is not supported inside a generic function. Spawned-task bodies are ordinary `async def` calls; a closure spawned into a crew does not yet borrow local data (step 6).
- There is no `send`/`share` checking and no crew-scoped borrowing, so nothing yet stops a program from handing data that is not safe to share to a task running on another worker.

## See also

- [The Execution Model](26-concurrency-execution-model.md) — contexts, tasks, cancellation tokens.
- [Closures and Capture](16-closures-and-capture.md) — the crew-scoped borrowing exception to move-on-escape.
- [Data-Race Freedom](30-data-race-freedom.md) — scoped sharing across a crew's tasks.
