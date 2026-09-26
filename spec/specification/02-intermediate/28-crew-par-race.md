# 28. `crew`, `par`, `race`

**Status:** Planned

`crew` structured concurrency, `par`, `race`, `on(target)`, and error handling in concurrent code (`on_error: collect`).

## `crew` — Structured Concurrency

`crew` runs multiple async tasks together. A `crew` block waits for all its tasks before continuing. If any task fails, the rest are cancelled and the error propagates.

```cinder
async def fetch_dashboard(user_id: int32) -> result[dashboard, app_error]:
    crew c:
        let user_task   = c.spawn(fetch_user(user_id))
        let orders_task = c.spawn(fetch_orders(user_id))
        let prefs_task  = c.spawn(fetch_preferences(user_id))

    let user   = user_task.get()?
    let orders = orders_task.get()?
    let prefs  = prefs_task.get()?
    return @ok(build_dashboard(user, orders, prefs))
```

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
crew c(on_error: collect):
    for item in items:
        c.spawn(process(item))

for err in c.errors():
    log("failed: {err.message()}")
```

## Implementation status

Parse-only. `crew_stmt`/`crew_expr` (with `on_error:`-style options), `par_expr`, `race_expr`, and `on_expr` all lex and parse. `check.cpp` types a `crew` body's contents against `k_unknown_type` with no borrow-scoping, no cancellation-scope construction, and no `spawn`/`get`/`errors` method semantics. No lowering exists — `src/hir/nodes.h` lists these forms as unsupported — and neither runtime backend has a scheduler capable of running tasks concurrently at all. None of the code in this chapter compiles past type-checking today.

## See also

- [The Execution Model](26-concurrency-execution-model.md) — contexts, tasks, cancellation tokens.
- [Closures and Capture](16-closures-and-capture.md) — the crew-scoped borrowing exception to move-on-escape.
- [Data-Race Freedom](30-data-race-freedom.md) — scoped sharing across a crew's tasks.
