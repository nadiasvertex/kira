# 40. Advanced Concurrency: Execution Graphs

**Status:** Planned

An execution graph is a pipeline of work described as a value — combined with `|` from stage combinators, inspected at compile time, and run only once connected to a scheduler.

`async`/`await` plus `crew` (see [Advanced Cancellation](41-advanced-cancellation.md) and the concurrency execution model) covers the vast majority of concurrency needs. Execution graphs are for handing a computation pipeline to an external scheduler, instrumenting across pipeline stages, or running stages on heterogeneous hardware (GPU, DMA engine, DSP).

## Design

Nothing in an execution graph runs until it is connected to a scheduler and started:

```kira
let graph =
    just(input)
    | then(parse)
    | on(pool, then(transform))
    | on(io, then(write_result))
    | with_timeout(seconds(30))
    | retry(max: 3, backoff: exponential)

let handle = graph.connect(my_scheduler).start()
let result = await handle
```

- `just(value)` starts a graph with a value already in hand.
- `then(f)` appends a stage that applies `f` to the previous stage's output.
- `on(context, stage)` runs `stage` under a specific execution context (a thread pool, an I/O context, or other custom context).
- `with_timeout(duration)` bounds a graph's total run time.
- `retry(max:, backoff:)` re-runs a failed stage up to `max` times under the given backoff policy.
- `graph.connect(scheduler)` binds the graph to a scheduler; `.start()` begins execution and returns a handle; `await handle` suspends until the graph completes.

Graphs are inspectable at compile time, since a graph is a value built up from combinators known at compile time:

```kira
static STAGES:   usize = count_stages(graph)
static USES_IO:  bool  = graph.uses_context(io)
```

A custom scheduler receives the graph as a value and may reorder stages, add instrumentation, or dispatch stages to specialized hardware — the extension point for game engines, real-time systems, and GPU pipelines.

## Implementation status

Not implemented. Execution graphs depend on the concurrency runtime — a scheduler, an execution-context abstraction (`pool`, `io`), and task/handle machinery — none of which exists yet: `src/hir/nodes.h` lists concurrency forms as unsupported for lowering, and neither the bytecode VM nor the AOT runtime has a scheduler, event loop, threads, channels, or synchronization primitives. `spec/concurrency-implementation-plan.md` explicitly defers execution graphs to its last phase (Phase 11, "not scheduled"), to be picked up once the ordinary `async`/`crew` runtime (Phases 1–10) is stable and a motivating use case exists. No grammar, AST, or semantic support for `just`/`then`/`on`/`with_timeout`/`retry`/`connect`/`start`/`count_stages`/`uses_context` exists in `src/`.

## See also

- [Advanced Cancellation](41-advanced-cancellation.md) — cancellation mechanics for the ordinary `async`/`crew` model this feature extends.
