# 17. Shared Ownership and Drop

**Status:** Implemented

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
- `clone()` on a handle makes another handle to the same value; it does not copy the value. The value drops when the last handle drops.
- Handles that form a cycle keep each other alive and are never dropped. There is no weak handle.
- `shared[T]` is an ordinary standard library type (`std.shared`). The `shared e` expression form needs no import; naming the type (`shared T` or `shared[T]`) needs `use std.shared.shared`.

### Representation

A handle holds one pointer to a heap block that records the number of live handles and the value:

```cinder
type shared_block[T] = { count: usize, value: T }
pub type shared[T] = { inner: *mut shared_block[T] }
```

- `shared[T].new(v)` allocates a block, moves `v` into it, and sets the count to 1. `shared e` is the same call written as an expression, and the bare form `shared.new(e)` solves `T` from its argument.
- `clone()` atomically increments the count and returns a second handle to the same block. `drop` atomically decrements it; the handle that takes the count to zero runs the value's drop glue and frees the block.
- `strong_count()` reports the number of live handles.
- A handle is one pointer wide. Its `shared_block` is held by reference like any struct, so code in `std.shared` writes the whole block into its slot instead of assigning the fields of an unwritten slot.
- Reading through a handle uses [`deref`](18-traits.md#deref). `clone` is found on the handle before the target's own `clone`, so `h.clone()` always adds a handle and never copies the value. A target's own method of the same name is reached with `(*h).clone()`.
- `shared` is a keyword that is also accepted as a module path segment and a type declaration name. The type name is not in the prelude and needs `use std.shared.shared`; the `shared e` expression form needs no import.

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
- **A matched value.** A `match`, `if let`, `while let` or destructuring `let` moves its subject only when some pattern binds a non-`copy` part of it by value; the bindings then own the parts they bind, and a `_` drops the part it skips. When the patterns bind only copies, a place keeps its value and its owner drops it. A pattern whose bindings overlap (an `as` alias, a `|`, or an array pattern) cannot split a value between owners: an alias of the whole pattern (`p as whole`) owns the value, any other overlapping binding must be `copy`, and an arm that binds only copies owns and drops a moved subject itself.
- **A loop.** `for x in xs` consumes `xs`: the iterator the loop makes of it (`into_iter`), or the iterator or generator it is handed, belongs to the loop, which drops it when the loop ends, so it drops the elements the loop never reached. The loop variable owns each element it is given. A loop over a borrowed collection (`for x in r` where `r: &list[T]`) cannot consume it, so it is allowed only when the elements are `copy`; to borrow the elements, iterate `r.iter()`.
- **A generator.** A `generator def` owns its parameters and the locals its body binds. A generator dropped before it finishes is resumed once to return from where it is suspended, which drops what it still owns; one that never ran drops its parameters.
- **A field.** A struct or sum value owns its fields and payloads, and drops them as part of its own drop glue. After a partial move, a binding drops only the fields it still owns.
- **An element.** A collection owns its elements and drops each one that it still holds when it is itself dropped. A library collection does this with `drop_in_place` (see [The `machine` Layer](../03-advanced/38-machine-layer.md)).
- **A temporary.** A value that an expression produces and nothing takes ownership of, such as a call result that is discarded, used as a method receiver, or read for one field, drops at the end of the statement that produced it. The exception is a `let` whose initializer borrows a temporary directly (`let r = &make()`): that temporary lives until the end of `r`'s scope.

### When drops run

- **Moves.** A value that was moved never drops at its old owner. If it was moved on some paths to a scope's end and not on others, it drops on exactly the paths where it is still owned. The compiler keeps a hidden flag at run time when it cannot tell the paths apart from the program's structure.
- **Assignment.** Assigning to a binding, field, or element that still holds a value drops the old value, after the new value has been computed.
- **Temporaries.** A value that no binding holds is a temporary: a call result that is discarded, borrowed (`make().len()`, `&make()`), or read a field from (`make().id`). It drops at the end of the statement that made it. A field moved out of a temporary is not dropped with it; a type with its own `drop` does not allow that move. The condition of an `if` or `while`, a `match` guard, the right operand of `and`/`or`, a returned value, and the value a block ends with each count as a statement of their own, so their temporaries drop as soon as they are evaluated. A borrow of a temporary cannot outlive its statement; to keep the value, bind it with `let` first. A value moved into a call or a binding is not a temporary.
- **Order.** Within one scope, bindings drop in reverse declaration order. Temporaries in one statement drop in reverse order of creation. A collection drops its elements in index order.
- **Early exits.** `return`, `break`, `continue`, and `?` drop everything owned by each scope they leave, innermost scope first, after the temporaries of the statements they leave. A `while let` subject's temporaries last one iteration.
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
  - Pattern bindings drop in `match`, `if let`, `while let`, destructuring `let`/`let ... else`, destructuring parameters and `for` heads, including struct shorthand fields, a `match` on a field of a local, an `as` alias of the whole pattern, and an arm that owns a subject its `|` pattern binds nothing of (`src/testdata/std_test/pattern_drops.cn`). A non-`copy` binding in an overlapping pattern is rejected (`src/testdata/semantic_ownership_check_test/reject_overlapping_owned_binding.cn`).
  - `for` loops own their iterator and drop it after the loop and on every jump out of it, and the loop variable, or each binding of a destructuring head, owns its element; comprehension variables do too (`src/testdata/std_test/for_loop_drops.cn`). A consuming loop over a borrowed collection of non-`copy` elements is rejected (`src/testdata/semantic_ownership_check_test/reject_consuming_loop_over_borrow.cn`).
  - Generators drop their locals as any function does, and a generator dropped before it finishes is cancelled (`hir_generator_cancel`): resumed with its `finished` slot set to `2`, its body returns from its start or the `yield` it is suspended at (`src/testdata/std_test/generator_drops.cn`).
  - A partly moved binding drops the fields it still owns, one by one (`pending_drop::moved_paths`, `src/hir/drop_schedule.h`).
  - Assigning over a value drops the old one first, for a local, a field of one, and a field or `*r` reached through a reference (`semantic::ownership::drop_facts_of`, `src/semantic/ownership_check.cpp`).
  - Run-time drop flags: a `let`/`var` binding or single-name parameter, or a field of one, moved on some paths only is dropped under a flag that the move clears and an assignment sets again (`drop_schedule::binding_flags`/`move_clears`, `src/testdata/std_test/drop_flags.cn`). Pattern bindings and `for` loop variables get flags the same way.
  - Temporaries drop at the end of their statement (`hir::make_references_explicit`, `src/hir/reference_check.cpp`), and the ownership checker ends them at the same points, so a borrow that outlives one is rejected (`src/testdata/std_test/temporary_drops.cn`, `src/testdata/semantic_ownership_check_test/reject_temporary_outlived.cn`). A `return`, `break`, `continue` or `?` that leaves a statement early drops the temporaries it has made so far under run-time live flags, and a `while let` subject's temporaries drop every iteration (`src/testdata/std_test/early_exit_drops.cn`). The failure path of `?` drops the function's locals too.
  - `list` drops its elements (`std.mem.drop_in_place`/`drop_range`), as do `clear`, `set` and `v[i] = x`; `list_into_iter` drops the elements it has not yielded (`src/testdata/std_test/list_element_drops.cn`).
  - `needs_drop[T]()` answers from the same drop plans scope exit reads, and `if` branches whose condition lowers to a literal `false` are not lowered (`src/testdata/std_test/needs_drop.cn`). It is a prelude query like `size_of`, usable in ordinary `if` conditions but not in `static if` or compile-time functions, since drop plans exist only after checking ends.
  - Unwinding through drops on panic is moot: this compiler has no stack unwinding at all (`panic` traps/aborts).
  - The prelude `drop(x)` function exists (`src/std/traits.cn`) and drops `x` when it returns (`src/testdata/std_test/swap_replace_drop.cn`).
  - The ownership checker (`src/semantic/ownership_check.h`) rejects any use after a move; `hir::compute_drop_schedule` is a separate, HIR-oriented walker that decides where drops go, and keeps its own record of which bindings were moved (see that file's doc comment).
- **`shared[T]` is implemented** in `src/std/shared.cn` over `std.mem`'s allocator and atomic primitives, with `clone`, `deref` and `drop` impls. Both backends run it (`src/testdata/std_test/shared_*.cn`, `accept_shared_expr.cn`; layout checked by `src/testdata/codegen_stress/134_shared_layout.cn`). Writing through a handle is rejected (`src/testdata/semantic_check_test/reject_mutate_through_shared.cn`), and so is a borrow through a handle that outlives it or a move out of the shared value (`src/testdata/semantic_ownership_check_test/reject_shared_borrow_outlived.cn`). A cycle of handles leaks; no test builds one, because a cycle needs interior mutability, which does not exist yet.
- **`scope` blocks work** (`src/testdata/std_test/scope_block.cn`), and so do drops for `where` bindings (`src/testdata/std_test/where_inline.cn`). The block's locals drop in reverse order at its `DEDENT`; a `where` binding drops when its `let` finishes, after the initializer is evaluated.

## See also

- [Ownership and Borrowing](14-ownership-and-borrowing.md) — ownership and moves that `drop` runs against.
- [Control Flow](../01-core/08-control-flow.md#scope) — the `scope` block, for ending a value's lifetime (and its drop) before the enclosing scope would.
- [Data-Race Freedom](30-data-race-freedom.md) — `mutex[T]`/`rwlock[T]`/`atomic[T]` for mutating behind `shared`.
