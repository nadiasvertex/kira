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

Partial. A narrow form of `send`/`share` is checked; crew-scoped borrow rules and the synchronized cells are not built.

- `send` and `share` are marker traits in `std.traits`. The checker derives them structurally (`concurrency_offender`, `src/semantic/check_bounds.cpp`): a raw pointer is neither, a `&T` needs `T: share`, a `shared[T]` needs `T: share`, and a struct, sum, tuple or array is the conjunction of its parts. Standard-library types vouch for their own fields. A user type built on a raw pointer says `impl send for the_type` and `impl share for the_type`; the type's arguments are still checked.
- The check runs on the arguments of a call handed to `c.spawn`, `par` or `race`, and on the outer locals an `async:` block or an `on(pool)` body reads. `on(io)` is exempt because it stays on one thread.
- Not checked: the captures of a closure value passed as a task, `&mut` aliasing across spawned tasks, `channel.send`, and a function-typed value's captures. A task type does not record its arguments, so only the direct call form is seen.
- `shared[T]` is implemented: an atomically reference-counted, read-only handle in `std.shared` (see [Shared Ownership and Drop](17-shared-ownership-and-drop.md)). `mutex[T]`, `rwlock[T]`, and `atomic[T]` do not exist in `src/std/`; `std.mem` has the atomic operations.

## See also

- [Shared Ownership and Drop](17-shared-ownership-and-drop.md) — `shared[T]`, the read-only reference-counted handle.
- [The Execution Model](26-concurrency-execution-model.md) — contexts, and where `send`/`share` are meant to be required.
- [`crew`, `par`, `race`](28-crew-par-race.md) — the structured-concurrency scope that makes scoped sharing sound.
