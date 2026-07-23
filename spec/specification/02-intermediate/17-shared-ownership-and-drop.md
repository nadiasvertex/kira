# 17. Shared Ownership and Drop

**Status:** Implemented

`shared[T]` reference-counted values, and the `drop` trait's rules for destructor execution.

## Shared Ownership

When single ownership is too restrictive — a value held by multiple parts of a program at once — use `shared`:

```kira
let config: shared config_t = shared load_config("app.toml")

# config can now be cloned freely; both copies refer to the same data
let worker_config = config.clone()
```

- `shared` values are *atomically* reference-counted, so a single `shared` value is always safe to hand to another task.
- A `shared` handle gives **read-only** access — it never yields `&mut` — so any number of tasks may hold and read the same `shared` value at once without a data race.
- To *mutate* data behind a `shared`, wrap it in a synchronized cell such as `mutex[T]` (see [Data-Race Freedom](30-data-race-freedom.md)).
- `shared` carries a small runtime cost; prefer single ownership, and prefer scoped borrowing (free) whenever possible.

## Destructors: `drop`

A type that owns a resource — a file handle, a socket, a lock — implements `drop` to release it automatically:

```kira
trait drop:
    def drop(mut self) -> unit

type file = { fd: raw_fd }

impl drop for file:
    def drop(mut self) -> unit:
        close_fd(self.fd)
```

`drop()` runs when a value's single owner goes out of scope, and nothing is added to types that do not implement `drop` (zero cost when unused). There is no Python-style `with`/context-manager protocol to opt into — a type just implements `drop`, and it runs at the end of *any* scope the value's owner is declared in, including a bare [`scope`](../01-core/08-control-flow.md#scope) block written solely to end that lifetime early. Rules:

- A binding that was **moved from** never drops — ownership already transferred, nothing left to release.
- Within one scope, values drop in **reverse declaration order**.
- A struct or sum type made of fields that implement `drop` gets an implicit field-wise drop for free: the type's own `drop()` (if any) runs first, then each field drops in declaration order. A type writes `drop()` only for the resource it directly owns, never to manually recurse into its fields.
- `shared[T]` drops the pointee when the atomic reference count reaches zero, not when any single handle goes out of scope.
- There is no direct `x.drop()` call — calling it explicitly and then letting scope exit call it again would double-drop. To release a value early, use the prelude function `drop(x)`, which moves `x` in and drops it.
- A panic **unwinds through drops**: every live destructor on the panicking path still runs, the same as on an ordinary early return.

## See also

- [Ownership and Borrowing](14-ownership-and-borrowing.md) — ownership and moves that `drop` runs against.
- [Control Flow](../01-core/08-control-flow.md#scope) — the `scope` block, for ending a value's lifetime (and its drop) before the enclosing scope would.
- [Data-Race Freedom](30-data-race-freedom.md) — `mutex[T]`/`rwlock[T]`/`atomic[T]` for mutating behind `shared`.
