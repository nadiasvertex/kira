# Concurrency Implementation Plan

Implements the complete concurrency design from `kira-reference.md` (§Async and
Concurrency, §Data-Race Freedom, §Advanced Cancellation, §Channels, §Advanced
Concurrency: Execution Graphs). Written 2026-07-12 against the state of the
tree at that date.

Should be implemented by Opus.

## Where the compiler stands today

Already done — do not redo:

- **Surface syntax is complete.** All keywords (`async`, `await`, `yield`,
  `par`, `race`, `on`, `crew`) are lexed; the parser builds `await_expr`
  (including the `await yield` form), `async_expr`, `crew_stmt`/`crew_expr`
  (with `on_error:`-style options), `par_expr`, `race_expr`, `on_expr`, and
  the `async` function modifier (composing with `generator`, `machine`,
  `pure`). See `src/parser/ast.h`, `parser.cpp:3424`–`4512`.
- **The coroutine transform exists.** `generator def` is implemented
  end-to-end: `hir_yield` + `hir::live_across_yield` liveness analysis, VM
  `op_make_generator`/`op_yield`/resume-index machinery
  (`src/bytecode/opcodes.h`), and LLVM lowering. The spec mandates that
  `async` is *the same* state-machine transform with a second suspension
  protocol — so async lowering is an extension of this machinery, not new
  machinery.
- **A `task` builtin type constructor** is registered in
  `src/semantic/types.cpp` (1–3 args) but has no checking semantics.
- **Existential returns (`some Trait`)** work, which async generators
  (streams) will need.

Not done — the actual work:

- `check.cpp` types `await` as a pass-through and `crew` bodies against
  `k_unknown_type`; there is no context (`C`) tracking, no `send`/`share`,
  no crew-scoped borrow rules.
- `src/hir/nodes.h:74` explicitly lists all concurrency forms as unsupported;
  lowering rejects them.
- Neither runtime (VM in `src/bytecode/vm.cpp`, AOT runtime in
  `src/llvm_codegen/aot_runtime.cpp` + `src/runtime/`) has a scheduler,
  event loop, threads, channels, or synchronization primitives.
- `src/std/` has no `task`, `channel`, `watch`, `shared`, `mutex`, `rwlock`,
  or `atomic`.

## Strategy

Three decisions that shape everything below:

1. **Single-threaded first.** Phases 1–6 build the complete surface —
   typing, lowering, `crew`/`par`/`race`, cancellation, channels — on a
   single-threaded cooperative scheduler (the `io` context). This exercises
   every language-level guarantee without touching threads. The `cpu`
   context, `pool`, and `on` become real in Phase 7. This ordering also means
   `send`/`share` can be *checked* early (Phase 2) but only becomes
   load-bearing when threads arrive, so any concept-inference bugs surface
   before they can cause data races.
2. **VM first, LLVM second, per phase.** The VM is the fast-iteration
   backend; each phase lands VM + tests, then the matching AOT work. The
   codegen stress corpus keeps the two honest.
3. **Async reuses the generator transform.** One suspension mechanism
   (`resume_index` + heap state block), two protocols. A task object is a
   generator object plus: status, result slot, cancellation token, and a
   waker (who to resume when it completes). An `async generator` is the same
   object where `op_yield` and await-suspension coexist — the spec's "no
   third lowering" requirement falls out.

Every phase ends with: regression tests in the affected layer's hand-rolled
test binary (`expect(...)` style), new `.kira` cases in
`src/testdata/semantic_stress/` or the codegen stress corpus, and a
warning-free build. Diagnostics follow the compiler-is-a-teacher standard:
every new error explains expected/found/why/fix.

---

## Phase 1 — Typing `task[T, E, C]`, `async`, `await`

All in `src/semantic/`.

- Give `task[T, E, C]` real semantics in `types.cpp`: default `E = never`,
  default `C = io`. Add builtin context types `io` and `cpu`, and the
  `cancelled` type.
- `check.cpp`:
  - An `async def f(...) -> result[T, E]` has *call type* `task[T, E, C]`;
    the body is checked against the declared return type. Infer `C` (default
    `io`; `on` blocks and executor-typed calls refine it in Phase 7).
  - `infer_await` (`check.cpp:5179`): operand must be a `task`; result is
    `result[T, E]`, or bare `T` when `E = never`. `await` is legal only
    inside an async context (async fn, `async:` block, `par`/`race`/`on`
    bodies) — track an `async_context` on the checker the same way generator
    bodies are tracked today. `await yield` types as `unit` and requires an
    async context.
  - `async:` block expressions type as `task[T, E, C]` of their body.
  - `async generator def` → `some iterator`-style stream typing (reuse
    existential machinery; introduce `stream[T, E]` as the awaited-iterator
    trait in `std/iter.kira`).
  - Bind `cancel` (a `cancel_token`) implicitly in async bodies, like the
    synthetic crew binding at `check.cpp:124`.
- Diagnostics: `await` outside async; awaiting a non-task (name the type,
  suggest removing `await` or making the callee `async`); missing `await`
  on a task used as a plain value (help: "a task is inert until awaited").
- Tests: `check_test.cpp` + semantic stress cases (positive and negative).

## Phase 2 — `send` / `share` concepts

- Add the two auto-derived structural concepts to the concept machinery in
  `check.cpp`/`types.cpp`: derived from a type's parts (struct of `send`
  fields is `send`; `&mut`-containing and raw-pointer types are neither;
  `shared[T]` is `send`+`share` iff `T: share`; `mutex[T]` is `share` iff
  `T: send`). Cache per `type_id`.
- Enforcement points: `c.spawn`, `par`, `on`, `channel.send` — but *gate on
  the target context being multi-threaded*, per spec. Until Phase 7 only
  `cpu`/`pool` targets trigger it, so add the checks now with tests that use
  `on(pool)` type-checking (even though `on` doesn't run yet).
- Diagnostic must name the concept, the offending field/part, and why
  (spec: "the compiler names the concept and explains why").
- Prelude: expose `send`, `share` names (`src/std/prelude.kira` /
  `traits.kira`).

## Phase 3 — Crew borrow rules (scoped sharing)

- Extend `move_check.cpp` (which already sees crew nodes) so a closure/task
  spawned into a `crew` may borrow enclosing locals, with the ordinary
  many-readers-xor-one-writer rule applied *across sibling spawns*: two
  spawns taking `&x` fine; two taking `&mut x` (or `&` + `&mut`) is a
  compile error; borrows may not escape the crew block.
- Escaping closures (stored, channel-sent) require `move` capture — hook the
  existing `hir/captures` escape analysis.
- This is compile-time only; no runtime work.

## Phase 4 — HIR lowering + VM scheduler (the core phase)

HIR (`src/hir/`):

- `hir_function::is_async`; `hir_await` (expr, carries operand + result
  type); `hir_async_block`; generalize `live_across_yield` to
  live-across-*suspend* (yield and await are both suspension points — the
  analysis is identical, only the trigger set grows).
- Drops on abandonment: a task cancelled/dropped mid-suspend must run drops
  for live locals, same rule generators already need; make sure the
  generator drop path is actually implemented and shared (check
  `generators-implemented` notes for the `locals_.emplace` footgun).

VM (`src/bytecode/`, `src/bytecode_compiler/`):

- Task object = generator object + `{status: created|running|suspended|done,
  result_slot, cancel_token, waker}`. New ops: `op_make_task` (mirrors
  `op_make_generator`), `op_await` (u8 dst, u8 task_reg — if done, copy
  result; else register current task as waker and suspend), `op_task_yield`
  (`await yield`: reschedule self at back of ready queue, check
  cancellation).
- Scheduler in `vm.cpp`: a ready deque of task objects + run loop
  (`run_until_complete(main_task)`). Single-threaded; `await` on a
  not-yet-done task suspends the current frame stack exactly as `op_yield`
  suspends a generator. The existing flat-frame-vector design means each
  task owns its own frame vector; the VM switches between them.
- Cancellation token: small shared object `{requested: bool}` checked by
  `op_task_yield` and on resume at `op_await`. A cancelled task resumes with
  a distinguished "unwind" path that runs drops and completes with the
  `cancelled` outcome.
- Tests: `vm_test.cpp` op-level tests; `compile_test.cpp` lowering tests;
  end-to-end `.kira` programs (two tasks ping-ponging via `await yield`,
  async call chains, `E = never` direct-value await).

## Phase 5 — `crew`, `par`, `race`, error handling, external cancellation

- Lower `crew` to: create cancellation scope + task list; `c.spawn(t)`
  enqueues `t` with the crew's token and returns a handle; at block end,
  emit a join loop (await each task; on first failure, request cancellation
  of siblings, then still await them — spec: cancellation completes before
  the scope returns). `on_error: collect` switches join to gather-all;
  `c.errors()` iterates them. Handle `.get()` yields the task's
  `result[T, E]`.
- `par:` desugars to a crew that spawns each branch and returns the tuple of
  results; `race:` to a crew that awaits the first completion, cancels the
  rest, returns the winner. Do these as HIR-level desugarings so the VM and
  LLVM backends both get them for free.
- Crew handles as values (`let handle = crew c: ...`), `handle.cancel()`,
  `await handle` (Advanced Cancellation §).
- Exhaustive tests: sibling-failure cancellation order, collect mode,
  external cancel, cancelled-vs-error distinctness (`cancelled` is its own
  type, not an `E`).

## Phase 6 — Channels, `watch`, and the sync/shared family

- Runtime intrinsics (VM natives now; C ABI symbols for AOT in Phase 8):
  channel create/send/recv/close with a fixed-capacity ring buffer;
  suspension integrates with the Phase 4 scheduler (full → suspend sender
  and park it on the channel's sender wait-list; empty+closed → `@none`).
- `src/std/`: new `sync.kira` (or extend prelude) with `channel[T]`
  (returning `(sender, receiver)` pair), `watch[T]` (`set`, `changed`,
  `get`), typed over `send` per Phase 2.
- `shared[T]` (atomic refcount, read-only deref), `mutex[T]` (lock →
  guard with `drop`-released `&mut`), `rwlock[T]`, `atomic[T]` for
  primitives. On the single-threaded runtime these are trivially correct;
  their contracts (guard types, `drop` release) are what's being built and
  tested now, their atomicity matters in Phase 7.

## Phase 7 — Threads: `cpu` context, `pool`, `on`

The first phase where real parallelism exists.

- VM runtime: a thread pool executor; `on(pool): block` lowers to "package
  block as a task, hand to pool, suspend on `io` until done, resume back on
  the caller's context". The reactor thread and pool threads exchange
  completed tasks via an MPSC queue.
- Make `shared`/`mutex`/`rwlock`/`atomic`/channel internals genuinely
  thread-safe (std::atomic refcounts, real locks, lock-free or mutexed
  channel).
- Checker: `on(pool)` refines the block's context to `cpu`; awaiting a
  `task[.., cpu]` from an `io` fn without `on` is the type error the spec
  promises. `send`/`share` enforcement from Phase 2 flips on for these
  edges. User-defined contexts/executors: a `context` trait + executor
  registration — keep minimal (built-ins + the trait), the extension story
  can grow later.
- Heavy stress tests: N tasks incrementing `shared mutex[int32]`, channel
  fan-in/fan-out across the pool, `race` between pool and io tasks.

## Phase 8 — LLVM/AOT backend parity

- Extend `llvm_codegen/codegen.cpp` to lower async state machines exactly as
  it lowers generators today (same live-across-suspend analysis feeds both).
- Port the Phase 4–7 runtime into the AOT runtime
  (`llvm_codegen/aot_runtime.cpp` + `src/runtime/`): scheduler, task
  objects, cancellation, channels, thread pool — as C ABI functions the
  generated code calls. Consider factoring the VM-native and AOT
  implementations over one shared C++ core in `src/runtime/` to avoid two
  schedulers drifting.
- Gate: the entire concurrency stress corpus passes identically under
  `interpret` and `aot` (extend `codegen_stress_test.cpp`).

## Phase 9 — The I/O reactor

Makes `io` mean something: without it, `await` never actually waits.

- kqueue (macOS) / epoll (Linux) reactor owned by the scheduler; timers
  first (`sleep`), then nonblocking file/socket reads in `src/runtime/io.*`,
  surfaced through `std/io.kira` as async functions.
- This can proceed in parallel with Phase 8 once Phase 7 lands; scope it to
  timers + basic file/pipe IO — an HTTP client is stdlib work, not
  concurrency-design work.

## Phase 10 — Async generators (streams) end-to-end

- Mostly falls out of Phases 1+4 (one transform, two protocols), but verify
  and test explicitly: `async generator def` bodies containing both `await`
  and `yield`, `for ... in` over a stream awaiting each `next()`, drops when
  a stream is abandoned mid-suspend. Budget real time here — "falls out"
  claims always hide a resume-index interaction bug or two.

## Phase 11 (deferred) — Execution graphs

`just | then | on | with_timeout | retry` pipeline values, `connect`/
`start`, compile-time graph inspection. The spec itself flags this as
rarely needed; it depends on compile-time reflection maturity more than on
the scheduler. Recommend deferring until Phases 1–10 are stable and there's
a motivating use case. Not scheduled.

---

## Sequencing and risk

Dependency chain: 1 → (2, 3 in parallel) → 4 → 5 → 6 → 7 → 8 → (9, 10) → 11.

Highest-risk items, worth prototyping early inside their phase:

1. **Task/frame switching in the VM** (Phase 4) — the flat frame-vector
   design meets multi-stack scheduling; get the ownership story right before
   building crew on top. Watch the `type_table` deque invariant and the
   generator `locals_` footgun already on record.
2. **Cancellation-runs-drops** (Phases 4–5) — correctness of unwinding a
   suspended coroutine is the subtlest guarantee in the spec.
3. **Two runtimes drifting** (Phase 8) — mitigate with the shared
   `src/runtime/` core and the dual-backend stress gate.
