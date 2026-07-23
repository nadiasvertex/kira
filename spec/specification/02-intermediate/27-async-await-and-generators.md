# 27. `async`/`await` and Generators

**Status:** Partial

`async`/`await` basics, `generator`/`yield`, the shared coroutine-lowering explanation, and async+generator composing into a stream.

## `async` and `await`

Marking a function `async` makes it a suspendable computation. Inside an `async` function, `await` pauses execution until another async operation completes, freeing the current thread for other work.

```kira
async def fetch_name(id: int32) -> result[str, network_error]:
    let response = await http.get("https://api.example.com/users/{id}")?
    let body     = await response.text()?
    return @ok(body)
```

`async` functions return `task[T, E, C]` (see [The Execution Model](26-concurrency-execution-model.md)) — nothing runs until something awaits or runs the task.

## Generators: `generator` and `yield`

`async` compiles a function into a suspendable state machine: locals live across `await` points, and the function resumes from wherever it last paused. A `generator` is that exact same transform with a different reason to pause. Where `await` suspends waiting on another computation, `yield` suspends to hand a value back to the caller:

```kira
generator def fibonacci() -> some iterator[uint64]:
    var a: uint64 = 0
    var b: uint64 = 1
    loop:
        yield a
        (a, b) = (b, a + b)
```

`generator def` produces `some iterator[T]` — the same trait an ordinary hand-written `next()` implementation satisfies (see [Traits](18-traits.md)). Nothing about calling a generator differs from calling any other iterator; `for x in fibonacci().take(10)` works whether `fibonacci`'s `next()` was written by hand or generated from a `yield`-based body. `generator` only changes how a body is *written*, never how the result is *used*.

`async` and `generator` are two surface names over one coroutine lowering, so they compose the same way other prefixes do:

```kira
async generator def lines_from(url: str) -> some iterator[str]:
    let conn = await connect(url)?
    loop:
        let chunk = await conn.read_chunk()?
        if chunk.is_empty(): break
        for line in chunk.split("\n"):
            yield line
```

A function that is both `async` and `generator` produces a *stream* — a value that suspends on `await` while producing values with `yield`. There is no third lowering to write or maintain for this case; it falls out of the two suspension protocols sharing one state-machine transform.

`yield` inside a generator suspended mid-body can be holding locals with live destructors, the same as `await` can — a generator abandoned before it runs to completion must still run `drop` on anything it was holding, exactly as an early `return` would. This is the same "every path out of a scope runs drops" rule that applies to functions in general (see [Shared Ownership and Drop](17-shared-ownership-and-drop.md)).

A generator that calls itself recursively does not automatically compose into one flat state machine — each recursive call is its own coroutine, resumed by the one that called it. This is fine for shallow, bounded recursion, but a deeply recursive generator (walking a tree structure and `yield`ing each node, for example) pays a per-level cost on every element it produces. Prefer an explicit stack over recursion in a generator when depth may be large.

## Implementation status

This chapter is split down the middle.

- **`generator`/`yield` is implemented end-to-end**, both backends: `hir_yield` plus `hir::live_across_yield` liveness analysis, VM `op_make_generator`/`op_yield`/resume-index machinery (`src/bytecode/opcodes.h`), and LLVM lowering. This is real, working compiler surface — write and run generator code today.
- **`async`/`await` is not implemented beyond parsing.** Surface syntax parses (`async_expr`, `await_expr`, the `async` function modifier composing with `generator`), but `task[T, E, C]` has no checking semantics, `check.cpp` types `await` as a pass-through of its operand rather than unwrapping a task's result, and no lowering exists for `async` — `src/hir/nodes.h` lists it as unsupported. The plan is to extend the existing generator coroutine transform to cover `async` (a task object is designed as a generator object plus status/result slot/cancellation token/waker), but that extension has not been built. The `async generator` stream form and the `async`+`await` half of every example above are design only.

## See also

- [The Execution Model](26-concurrency-execution-model.md) — `task[T, E, C]`, contexts, cancellation.
- [Shared Ownership and Drop](17-shared-ownership-and-drop.md) — the drop-on-abandonment rule generators follow.
- [Traits](18-traits.md) — the `iterator` trait a generator's result satisfies.
