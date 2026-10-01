# 17. Shared Ownership and Drop

**Status:** Partial

`shared[T]` reference-counted values, and the `drop` trait's rules for destructor execution.

## Shared Ownership

When single ownership is too restrictive — a value held by multiple parts of a program at once — use `shared`:

```cinder
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

```cinder
trait drop:
    def drop(mut self) -> unit

type file = { fd: raw_fd }

impl drop for file:
    def drop(mut self) -> unit:
        close_fd(self.fd)
```

Every value is dropped exactly once, by whoever owns it when it stops being needed, unless it was moved away first. Nothing is added to types that need no drop (zero cost when unused). There is no Python-style `with`/context-manager protocol to opt into — a type just implements `drop`, and it runs at the end of *any* scope the value's owner is declared in, including a bare [`scope`](../01-core/08-control-flow.md#scope) block written solely to end that lifetime early.

### What needs a drop

A type needs a drop when it implements `drop`, or when any of its fields, variant payloads, or elements needs one. Dropping a value of such a type runs its *drop glue*: the type's own `drop()` first, if it has one, then the drop glue of each field in declaration order (for a sum, of the payload of the variant it holds). A type writes `drop()` only for the resource it directly owns, never to recurse into its fields by hand. A `copy` type never needs a drop (see [Ownership and Borrowing](14-ownership-and-borrowing.md#copy-and-move)).

### Owners

Each of these owns a value, and drops it when its lifetime ends while it still holds it:

- **A binding.** A `let`/`var` name, a by-value parameter, a pattern binding, and a `for` loop variable each own their value. A binding drops at the end of the scope it is declared in; a `for` loop variable drops at the end of each iteration.
- **A field.** A struct or sum value owns its fields and payloads, and drops them as part of its own drop glue. After a partial move, a binding drops only the fields it still owns.
- **An element.** A collection owns its elements and drops each one that it still holds when it is itself dropped. A library collection does this with `drop_in_place` (see [The `machine` Layer](../03-advanced/38-machine-layer.md)).
- **A temporary.** A value that an expression produces and nothing takes ownership of, such as a call result that is discarded, used as a method receiver, or read for one field, drops at the end of the statement that produced it. The exception is a `let` whose initializer borrows a temporary directly (`let r = &make()`): that temporary lives until the end of `r`'s scope.

### When drops run

- **Moves.** A value that was moved never drops at its old owner. If it was moved on some paths to a scope's end and not on others, it drops on exactly the paths where it is still owned. The compiler keeps a hidden flag at run time when it cannot tell the paths apart from the program's structure.
- **Assignment.** Assigning to a binding, field, or element that still holds a value drops the old value, after the new value has been computed.
- **Temporaries.** A value that no binding holds is a temporary: a call result that is discarded, borrowed (`make().len()`, `&make()`), or read a field from (`make().id`). It drops at the end of the statement that made it. A field moved out of a temporary is not dropped with it; a type with its own `drop` does not allow that move. The condition of an `if` or `while`, a `match` guard, the right operand of `and`/`or`, a returned value, and the value a block ends with each count as a statement of their own, so their temporaries drop as soon as they are evaluated. A borrow of a temporary cannot outlive its statement; to keep the value, bind it with `let` first. A value moved into a call or a binding is not a temporary.
- **Order.** Within one scope, bindings drop in reverse declaration order. Temporaries in one statement drop in reverse order of creation. A collection drops its elements in index order.
- **Early exits.** `return`, `break`, `continue`, and `?` drop everything owned by each scope they leave, innermost scope first.
- `shared[T]` drops the pointee when the atomic reference count reaches zero, not when any single handle goes out of scope.
- There is no direct `x.drop()` call — calling it explicitly and then letting scope exit call it again would double-drop. To release a value early, use the prelude function `drop(x)`, which moves `x` in and drops it.
- A panic **unwinds through drops**: every live destructor on the panicking path still runs, the same as on an ordinary early return.

```cinder
def demo(flag: bool) -> unit:
    var a = file.open("a")
    let b = file.open("b")
    if flag:
        consume(b)               # b moves on this path only
    a = file.open("c")           # "a" is closed here, after "c" is opened
    file.open("d").size()        # "d" is closed at the end of this statement
                                 # scope end: b is closed only if flag was false, then "c"
```

## Implementation status

- **`drop` runs at scope exit for the common case.** `checker::resolve_drop_plans` (`src/semantic/check.cpp`) resolves each droppable type's own `impl drop` plus its droppable fields; `hir::compute_drop_schedule` (`src/hir/drop_schedule.{h,cpp}`) and `hir::lowerer` (`src/hir/lower.cpp`) synthesize the calls. Verified end-to-end, both backends, exact output: reverse-declaration-order drop, a moved-from binding never dropping, implicit field-wise drop for a struct with no own `impl drop`, and drop firing correctly on early `return`/`break`/`continue` (`src/testdata/std_test/scope_exit_drop.cn`). Direct `x.drop()` is rejected, as this chapter requires (`src/testdata/semantic_check_test/reject_direct_drop_call.cn`).
  - Not yet covered: a block or function whose tail is a real (non-`unit`) value skips scope-exit drop for that scope entirely, rather than corrupting the returned value (a documented leak, never a double-drop); a value-returning `return expr` does not drop a local `expr` only *borrows* from (a directly-returned local, like `return h`, is unaffected); a destructuring `for` head over owned yields is not tracked, an iterable that is not a plain local is never dropped, and neither are bindings in patterns with an alias, `|`, or an array pattern. `for x in xs` moves a whole local `xs` and drops it after the loop (also on `break` and `return`); the loop variable borrows. A single-name loop variable over an iterator or generator that yields values owns each one and drops it at the end of its iteration. Bindings in `match`, `if let`, `while let`, destructuring `let`/`let ... else`, and destructuring parameters do drop.
  - A partly moved binding drops the fields it still owns, one by one (`pending_drop::moved_paths`, `src/hir/drop_schedule.h`).
  - Assigning over a value drops the old one first, for a local, a field of one, and a field or `*r` reached through a reference (`semantic::ownership::drop_facts_of`, `src/semantic/ownership_check.cpp`).
  - Run-time drop flags: a `let`/`var` binding or single-name parameter, or a field of one, moved on some paths only is dropped under a flag that the move clears and an assignment sets again (`drop_schedule::binding_flags`/`move_clears`, `src/testdata/std_test/drop_flags.cn`). A pattern binding moved on some paths still leaks on the others.
  - Temporaries drop at the end of their statement (`hir::make_references_explicit`, `src/hir/reference_check.cpp`), and the ownership checker ends them at the same points, so a borrow that outlives one is rejected (`src/testdata/std_test/temporary_drops.cn`, `src/testdata/semantic_ownership_check_test/reject_temporary_outlived.cn`). A temporary is not dropped on a path that leaves its statement early (a `return`, `break`, `continue` or `?` inside it), and a `while let` subject's temporaries drop once, after the loop, rather than every iteration.
  - `list` drops its elements (`std.mem.drop_in_place`/`drop_range`), as do `clear`, `set` and `v[i] = x`; `list_into_iter` drops the elements it has not yielded (`src/testdata/std_test/list_element_drops.cn`).
  - **Not yet implemented** from the rules above: `needs_drop[T]()`. The drop decisions are also split between the ownership checker and the lowerer, which have to agree through shared helper functions; it is tracked as item 6 in [todo.md](../../todo.md).
  - Unwinding through drops on panic is moot: this compiler has no stack unwinding at all (`panic` traps/aborts).
  - The prelude `drop(x)` function exists (`src/std/traits.cn`) and drops `x` when it returns (`src/testdata/std_test/swap_replace_drop.cn`).
  - The ownership checker (`src/semantic/ownership_check.h`) rejects any use after a move; `hir::compute_drop_schedule` is a separate, HIR-oriented walker that decides where drops go, and keeps its own record of which bindings were moved (see that file's doc comment).
- **`shared[T]` is a name with nothing behind it.** `shared` is registered as a builtin generic (`src/semantic/types.cpp:51`) and that is the entire implementation. The `shared` expression form yields a plain reference — `let a: shared t = shared t{ id: 1 }` fails with ``expected `shared[t]`, found `&t` `` — and `shared[T]` has no methods, so `.clone()` does not resolve. No atomic reference count exists anywhere in the source tree, so neither the read-only-access guarantee nor drop-at-zero is enforced or implemented.
- **`scope` blocks work** (`src/testdata/std_test/scope_block.cn`), and so do drops for `where` bindings (`src/testdata/std_test/where_inline.cn`). The block's locals drop in reverse order at its `DEDENT`; a `where` binding drops when its `let` finishes, after the initializer is evaluated.

Tracked as items 6–7 in [todo.md](../../todo.md).

## See also

- [Ownership and Borrowing](14-ownership-and-borrowing.md) — ownership and moves that `drop` runs against.
- [Control Flow](../01-core/08-control-flow.md#scope) — the `scope` block, for ending a value's lifetime (and its drop) before the enclosing scope would.
- [Data-Race Freedom](30-data-race-freedom.md) — `mutex[T]`/`rwlock[T]`/`atomic[T]` for mutating behind `shared`.
