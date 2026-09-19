# 17. Shared Ownership and Drop

**Status:** Partial

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

## Implementation status

- **`drop` runs at scope exit for the common case.** `checker::resolve_drop_plans` (`src/semantic/check.cpp`) resolves each droppable type's own `impl drop` plus its droppable fields; `hir::compute_drop_schedule` (`src/hir/drop_schedule.{h,cpp}`) and `hir::lowerer` (`src/hir/lower.cpp`) synthesize the calls. Verified end-to-end, both backends, exact output: reverse-declaration-order drop, a moved-from binding never dropping, implicit field-wise drop for a struct with no own `impl drop`, and drop firing correctly on early `return`/`break`/`continue` (`src/testdata/std_test/scope_exit_drop.kira`). Direct `x.drop()` is rejected, as this chapter requires (`src/testdata/semantic_check_test/reject_direct_drop_call.kira`).
  - Not yet covered: a block or function whose tail is a real (non-`unit`) value skips scope-exit drop for that scope entirely, rather than corrupting the returned value (a documented leak, never a double-drop); a value-returning `return expr` does not drop a local `expr` only *borrows* from (a directly-returned local, like `return h`, is unaffected); match-arm/`for`-loop/destructuring-pattern bindings are not tracked, only a plain `let`/`var`/simple parameter; and a sum type's field-wise drop (payload recursion) is not implemented — only its own `impl drop` runs.
  - Unwinding through drops on panic is moot: this compiler has no stack unwinding at all (`panic` traps/aborts).
  - The prelude `drop(x)` free function still does not exist. A call to it is accepted by the checker anyway and then fails in lowering with "no concrete checked type is available for this node" — a compiler-gap diagnostic for what is really an undefined name. (The `drop` *trait* is correctly prelude-reachable: `impl drop for T` needs no `use`.)
  - `src/semantic/move_check.h` remains the move-tracking substrate these rules build on; `hir::compute_drop_schedule` is a second, HIR-oriented walker alongside it, not a replacement (see that file's doc comment for why).
- **`shared[T]` is a name with nothing behind it.** `shared` is registered as a builtin generic (`src/semantic/types.cpp:51`) and that is the entire implementation. The `shared` expression form yields a plain reference — `let a: shared t = shared t{ id: 1 }` fails with ``expected `shared[t]`, found `&t` `` — and `shared[T]` has no methods, so `.clone()` does not resolve. No atomic reference count exists anywhere in the source tree, so neither the read-only-access guarantee nor drop-at-zero is enforced or implemented.
- **The `scope` block this chapter relies on does not parse.** `scope_stmt` is in `spec/kira-grammar.ebnf`, but there is no `kw_scope` token and no parser support, so the mechanism named above for ending a lifetime early is unavailable.

Tracked as items 6–8 in [todo.md](../../todo.md).

## See also

- [Ownership and Borrowing](14-ownership-and-borrowing.md) — ownership and moves that `drop` runs against.
- [Control Flow](../01-core/08-control-flow.md#scope) — the `scope` block, for ending a value's lifetime (and its drop) before the enclosing scope would.
- [Data-Race Freedom](30-data-race-freedom.md) — `mutex[T]`/`rwlock[T]`/`atomic[T]` for mutating behind `shared`.
