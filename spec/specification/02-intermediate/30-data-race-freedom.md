# 30. Data-Race Freedom

**Status:** Partial

`send`/`share` concepts, and the two ways to share data across tasks: scoped sharing vs. `shared` + `mutex[T]`/`rwlock[T]`/`atomic[T]`.

## `send` and `share`

 Cinder's memory-safety rule — many readers or one writer, never both — is also its data-race rule; the compiler is meant to extend that one invariant across tasks. Two concepts, both satisfied automatically (no `impl` written for them, invisible until violated), decide what may cross a task boundary:

- **`send`** — a type whose ownership may move to another task.
- **`share`** — a type that several tasks may read through `&` at the same time.

A type satisfies them structurally from its parts: a struct of `send` fields is `send`, and so on. The concurrency primitives require them where it matters — `c.spawn`, `par`, `on`, and `channel.send` move `send` values and borrow `share` values — and when a type qualifies for neither, the compiler is meant to name the concept and explain why (a raw pointer, for instance, is neither).

## Two Ways to Share Data

The same invariant, enforced at two different times.

**Scoped sharing — free, checked at compile time.** Because a `crew`'s tasks cannot outlive it, spawned tasks may borrow data from the enclosing scope. The ordinary borrow rules apply across the crew: any number of tasks may hold `&data`, but the compiler will not let two tasks hold `&mut data`. Concurrent mutable aliasing is a compile error, no lock involved.

```cinder
let table = build_table()
crew c:
    c.spawn(lookup(&table, "a"))   # many readers — fine
    c.spawn(lookup(&table, "b"))
```

**Unscoped sharing — `shared`, with synchronized mutation.** When data must outlive any single scope, a `shared[T]` handle keeps it alive and gives read-only access, so many tasks can read at once. To *mutate* shared data, wrap it in a synchronized cell that grants exclusive access one holder at a time:

- `mutex[T]` — lock to obtain a temporary `&mut T`; other tasks wait.
- `rwlock[T]` — many readers or one writer, decided at runtime.
- `atomic[T]` — lock-free operations on primitive values.

```cinder
let counter: shared mutex[int32] = shared mutex(0)

crew c:
    for _ in 0..n:
        c.spawn(async:
            let guard = counter.lock()   # exclusive access, released when guard drops
            *guard += 1
        )
```

A `mutex` enforces "one writer at a time" at runtime, exactly as the borrow checker enforces it at compile time — the two guarantees are one guarantee. Reach for `shared mutex[T]` only when data genuinely escapes structure; scoped sharing costs nothing and should be the default.

## Implementation status

Partial. Everything above works on both backends except writing through a guard with `*`.

- `send` and `share` are marker traits in `std.traits`. The checker derives them structurally (`concurrency_offender`, `src/semantic/check_bounds.cpp`): a raw pointer is neither, a `&T` needs `T: share`, a `shared[T]` needs `T: share`, and a struct, sum, tuple or array is the conjunction of its parts. Standard-library types vouch for their own fields. A user type built on a raw pointer says `impl send for the_type:` and `impl share for the_type:`; the type's arguments are still checked. When a type fails, the error names the field that caused it and suggests the `impl`, or a `mutex` when the type is `send` but not `share`.
- The check runs on the arguments of a call handed to `c.spawn`, `par` or `race`, on the outer locals an `async:` block or an `on(...)` body reads, on the task given to `on(ctx, task)`, and on function values and tasks that cross to another task. The checker follows a function value or task back to where it was made and checks what it carries; one whose origin it cannot see is rejected. The check is the same for every context, `io` included, because any context may run a task on another thread.
- Crew-scoped borrows are checked by the ownership checker (`lower_crew_body`, `src/semantic/ownership_cfg.cpp`). Each `crew` has a holder local that keeps every borrow made by a task spawned into it live until the crew body ends, so the usual rules apply across tasks: any number of `&x`, or one `&mut x`, and no use or move of `x` while a task holds it. A `task` value keeps the borrows of its arguments until it is dropped or spawned. A non-copy local consumed by an `async:` or `on` body moves into the task.
- `std.sync` (`src/std/sync.cn`) has `mutex[T]`, `rwlock[T]` and `atomic[T]`. They are `send` and `share` whenever `T: send`, because one task at a time reaches the value. `rwlock[T]` is `share` only when `T: share`, since its readers share the value.
  - `mutex.lock()` blocks until the lock is free and returns a `mutex_guard[T]`; `try_lock()` returns `none` instead of waiting. The lock is released when the guard drops.
  - `rwlock.read()` returns a `read_guard[T]`, and `rwlock.write()` returns a `write_guard[T]` once no reader or writer holds the lock.
  - `atomic[T]` needs `T: integer` and has `load`, `store`, `swap`, `compare_exchange`, `fetch_add` and `fetch_sub`, all sequentially consistent.
- Deviation: the language has no mutable deref trait, so `*guard += 1` from the example above does not compile. A guard reads through `*guard` or `get()`, and writes through `get_mut()` or `set(value)`.
- `shared[T]` is an atomically reference-counted, read-only handle in `std.shared` (see [Shared Ownership and Drop](17-shared-ownership-and-drop.md)).

## See also

- [Shared Ownership and Drop](17-shared-ownership-and-drop.md) — `shared[T]`, the read-only reference-counted handle.
- [The Execution Model](26-concurrency-execution-model.md) — contexts, and where `send`/`share` are meant to be required.
- [`crew`, `par`, `race`](28-crew-par-race.md) — the structured-concurrency scope that makes scoped sharing sound.
