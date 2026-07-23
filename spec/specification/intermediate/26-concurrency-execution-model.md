# 26. Concurrency: The Execution Model

**Status:** Planned

Executors and contexts (`io`, `cpu`/`pool`), tasks (`task[T, E, C]`), and cancellation scopes.

The primitives covered across this chapter and [`async`/`await` and Generators](27-async-await-and-generators.md), [`crew`, `par`, `race`](28-crew-par-race.md), [Channels and Sync Primitives](29-channels-and-sync-primitives.md), and [Data-Race Freedom](30-data-race-freedom.md) all run on one model, defined here.

## Executors and Contexts

An *executor* is a concrete resource that runs tasks: the runtime's I/O reactor, a thread pool, a GPU queue. A *context* is the typed capability to run on a kind of executor — what a task requires in order to run, and the `C` in `task[T, E, C]`. Two contexts are built in:

- `io` — the default context for I/O-bound, suspending work, backed by the runtime's reactor.
- `cpu` — for CPU-bound work, backed by a thread pool; the built-in pool executor is `pool`.

A program can add contexts (`gpu`, `dma`, a real-time loop) by providing an executor for them, and the same primitives work unchanged.

A task may be awaited only from a context that satisfies its requirement, so running CPU-heavy work on the I/O reactor — or I/O on a real-time loop — is a type error rather than a latency bug found in production. `on` bridges contexts:

```kira
async def handle_request(req: http_request) -> http_response:   # a task[..., io]
    let result = await on(pool):        # switch to the cpu context for this block
        expensive_computation(req.body)
    # back on io here
    return http_response.ok(result)
```

Because `pool` — like any multi-threaded executor — may run a task on a different thread, moving work onto it requires the data to satisfy `send`/`share` (see [Data-Race Freedom](30-data-race-freedom.md)). A single-threaded context imposes no such requirement. The context in a task's type and the concurrency-safety concepts are the same mechanism seen from two sides.

## Tasks

An `async def` produces a `task[T, E, C]`: a value describing a computation that yields `T` on success or `E` on failure and requires context `C` to run. A task is inert — nothing happens until it is awaited (which runs it to completion on the current context and produces its `result[T, E]`) or spawned into a `crew` (which runs it concurrently). When a task cannot fail, `E` is `never`, and awaiting yields `T` directly instead of a `result`.

## Cancellation

Every task runs inside a *cancellation scope*. A `crew` establishes one, and each task it spawns inherits a cancellation token. Cancellation is *cooperative*: a task observes it only at suspension points (`await`) and at explicit checks, so it is never interrupted at an arbitrary instruction and its invariants stay intact. A task's own token is available as `cancel`:

```kira
if cancel.is_requested(): return @err(@cancelled)
await yield        # a suspension point that only yields and checks cancellation
```

Cancellation is requested when a sibling in the same `crew` fails (the crew cancels the rest, unless `on_error: collect`), when a `crew` handle is cancelled from outside (`handle.cancel()`), or when the enclosing scope is torn down. A cancelled task unwinds normally — because ownership frees values at scope exit, its locks, handles, and buffers are released as it returns. Cancellation is an *outcome*, not an error: `cancelled` is its own type, never silently confused with a real failure. Because tasks cannot outlive their `crew`, cancellation always completes before the scope returns.

## Implementation status

Design only, beyond parsing. Concretely, as of this writing:

- Surface syntax parses completely: `async`, `await` (including `await yield`), `crew`, `par`, `race`, `on`, and the `async` function modifier all lex and parse (`src/parser/ast.h`, `parser.cpp:3424`–`4512`).
- A bare `task[T, E, C]` type constructor is registered in `src/semantic/types.cpp` (1–3 type arguments) but has no checking semantics — no default `E = never`, no default `C = io`, no `io`/`cpu` builtin context types, no context-mismatch diagnostic.
- `check.cpp` types `await expr` as a pass-through of its operand's type (`infer_await`, `check.cpp:11321`, comment: "awaiting a non-task is validated once tasks are typed") and types a `crew` body as `k_unknown_type` (`check.cpp:11612`–`11617`). There is no context (`C`) tracking, no `send`/`share` checking, no crew-scoped borrow rules.
- `src/hir/nodes.h` lists concurrency forms as unsupported; lowering rejects them.
- Neither runtime backend (the bytecode VM in `src/bytecode/vm.cpp`, the AOT runtime in `src/llvm_codegen/aot_runtime.cpp` + `src/runtime/`) has a scheduler, event loop, threads, or synchronization primitives.
- `src/std/` has no `task`, `channel`, `watch`, `shared`, `mutex`, `rwlock`, or `atomic`.

The coroutine transform this chapter's `task` semantics are meant to build on (state machine with a `resume_index` and heap state block) does already exist end-to-end for `generator def` — see [`async`/`await` and Generators](27-async-await-and-generators.md#implementation-status).

## See also

- [`async`/`await` and Generators](27-async-await-and-generators.md) — the coroutine machinery this model is meant to extend.
- [Data-Race Freedom](30-data-race-freedom.md) — `send`/`share`, referenced above.
