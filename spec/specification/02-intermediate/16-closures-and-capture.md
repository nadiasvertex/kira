# 16. Closures and Capture

**Status:** Partial

Capture-by-borrow vs. capture-by-move, the `move` keyword, the `crew`-scoped borrowing rule, closure types (`fn(A) -> B`) and monomorphization, and the three ways to vary returned behavior.

## Capture

A lambda captures the variables it uses from the surrounding scope. How it captures depends on whether the closure escapes:

- A closure that does **not** escape — passed to `map`, `filter`, or any function that calls it and returns — captures by **borrow**. Nothing is copied; the borrow is bounded by the call, like any other borrow.
- A closure that **does** escape — stored somewhere longer-lived, or sent to another task — captures by **move**, taking ownership of what it uses, because a borrow cannot escape. `move` before the lambda forces move-capture explicitly.

```kira
let factor = 3
let scaled = numbers.map(x => x * factor)   # borrows factor; does not escape
```

## `crew`-Scoped Borrowing

Because a `crew`'s tasks cannot outlive the `crew`, and the `crew` cannot outlive its enclosing scope, a closure spawned into a `crew` may **borrow** local data — no `'static`-style requirement, no forced `move`. Full `crew` semantics are in [`crew`, `par`, `race`](28-crew-par-race.md); the rule stated here is that spawn sites are an exception to "an escaping closure must move."

```kira
async def totals() -> result[summary, app_error]:
    let rows = load_rows()
    crew c:
        let a = c.spawn(sum_column(&rows, 0))   # borrows rows across the crew
        let b = c.spawn(sum_column(&rows, 1))
    let total = a.get()? + b.get()?
    return @ok(total)
```

## Closure Types and Returning Closures

The capture rules above govern a closure's *environment*. Its *type* is `fn(A) -> B`, the type of any callable with that signature, capturing or not. Wherever `fn(A) -> B` appears as a parameter or return type, the compiler monomorphizes it to the concrete callable — calls stay direct, no vtable:

```kira
def apply(f: fn(int32) -> int32, x: int32) -> int32:   # accepts any callable, zero cost
    return f(x)

def adder(n: int32) -> fn(int32) -> int32:
    return x => x + n        # returns one concrete closure, hidden behind fn(int32) -> int32
```

`adder` returns a single closure *shape*, so `fn(int32) -> int32` names it opaquely: the caller calls it and the compiler inlines through it — nothing is boxed.

## Three Ways to Vary Returned Behavior

To return *different* behaviors, prefer expressing the variation as data over an opaque closure — it stays static, is inspectable, and fits the rest of the language:

1. **Chosen at compile time** — a dependent return type resolves to a concrete type from a `static` value. No runtime cost, no erasure.
2. **Chosen at runtime, from a known set** — return a sum type describing the behavior and interpret it:

```kira
type op = @inc | @scale(int32) | @clamp(int32, int32)

def apply_op(o: op, x: int32) -> int32:
    match o:
        @inc           => x + 1
        @scale(k)      => x * k
        @clamp(lo, hi) => min(max(x, lo), hi)
```

A sum type is a static, matchable `variant` — dispatch is a branch on a tag, not a hidden call. Closures have anonymous types, so two different closures cannot be named to place them in a sum; modelling the behavior as data avoids the problem.

3. **Chosen at runtime, from an open set** — heterogeneous callbacks stored together, plugins loaded at run time — is the one case that cannot be monomorphized. `box[fn(A) -> B]` is an owned, type-erased closure for this case only: it owns its captured environment and is called indirectly, and the cost is explicit in the type:

```kira
var handlers: list[box[fn(event) -> unit]] = []
handlers.push(box(e => log(e)))
handlers.push(box(e => metrics.record(e)))
```

`box` generalizes beyond closures to any object-safe trait; see [Trait Objects](24-trait-objects.md).

## Implementation status

- `fn(A) -> B` closure types, lambda expressions, and monomorphized inlining at call sites are implemented and exercised throughout the semantic and codegen stress corpora.
- The `move` keyword parses (`parser.cpp` sets `lambda_expr::is_move` on `kw_move`) and survives AST cloning, but no consumer was found: `src/semantic/move_check.cpp` and `src/hir/captures.cpp` (which computes `free_variables` for closure environments) do not branch on `is_move`. The capture-by-borrow-vs-capture-by-move *distinction* described above — including the requirement that an escaping closure without explicit `move` is rejected or otherwise forced to move — is not enforced by the checker today; `move` currently has no observable semantic effect beyond parsing.
- The three ways to vary returned behavior: the dependent-type and sum-type forms rely on ordinary generics and sum types, both implemented (see [Generics and Inference](19-generics-and-inference.md)). `box[fn(A) -> B]` does **not** work today — no trait-object/vtable dynamic dispatch exists anywhere in the compiler (no lowering for `box` was found in `src/hir/`, `src/bytecode/`, or `src/llvm_codegen/`; `box` is registered only as a bare type constructor in `src/semantic/types.cpp`). See [Trait Objects](24-trait-objects.md) for the equivalent finding on `box[trait]` generally.

## See also

- [Ownership and Borrowing](14-ownership-and-borrowing.md) — the borrow/move distinction closures follow.
- [`crew`, `par`, `race`](28-crew-par-race.md) — full structured-concurrency semantics for spawn-site borrowing.
- [Trait Objects](24-trait-objects.md) — `box[trait]` as the general form of `box[fn(A) -> B]`.
