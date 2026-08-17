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

### Explicit capture lists

The rule above is the *implicit* capture rule: a lambda with no capture list captures every outer-scope name its body uses. A capture list — `[...]` before the parameters, see [Lambdas](../01-core/05-lambdas.md) — replaces this with an explicit, closed set: only listed names are reachable from the enclosing scope, and each one names its own capture mode (`name` for the default borrow-or-move rule, `&name` to force borrow, `&mut name` for mutable borrow) instead of inheriting it from escape analysis. `move` still applies to the whole lambda, and when combined with a capture list forces move on every bare `name` entry — `&name`/`&mut name` entries are unaffected, since they already state their mode explicitly.

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
- The `move` keyword parses (`parser.cpp` sets `lambda_expr::is_move` on `kw_move`) and survives AST cloning, but no consumer was found: `src/semantic/move_check.cpp` and `src/hir/captures.cpp` do not branch on `is_move`. The capture-by-borrow-vs-capture-by-move *distinction* described above for **implicit** capture — including the requirement that an escaping closure without explicit `move` is rejected or otherwise forced to move — is not enforced by the checker today; `move` has no observable semantic effect beyond parsing, and implicit capture is by value in every lowering. The one part of `move` this chapter describes that is therefore still unimplemented is "`move` forces move on every bare `name` entry": a bare entry is a by-value copy whether `move` is written or not.
- Explicit capture lists (`[name, &name, &mut name]`, and `[]`) are implemented end to end:
  - **Parsing** — `parse_capture_list` (`src/parser/parser.cpp`), reached from a leading `[` in expression position by the `at_capture_list` lookahead, which distinguishes a capture list from an array literal by what follows the `]`. The list is stored as `std::optional<std::vector<lambda_capture>>` on `lambda_expr`, so `[]` (capture nothing) stays distinct from no list at all (capture implicitly).
  - **Restriction** — the checker keeps a stack of capture barriers alongside its lexical scopes (`capture_barrier` in `src/semantic/check.cpp`). A name resolved across a lambda boundary whose list omits it is rejected, with a diagnostic that points at both the reference and the list. The list itself is validated too: duplicate entries, names that do not resolve to an enclosing local, module-level names that never needed capturing, `&mut` of a non-`var` binding, and a by-reference entry reaching through an enclosing *by-value* capture are each their own error; a listed name the body never reads is a warning.
  - **Modes** — `capture_plan` (`src/hir/captures.h`) is the single ordered environment plan both backends build from: the explicit list in source order when there is one, `free_variables` otherwise. A `by_value` entry copies into the environment block as before. A `&`/`&mut` entry stores the *address* of the enclosing frame's storage instead, so reads and writes inside the body reach the original binding. In `llvm_codegen` that address is the local's `alloca`; the bytecode VM has no addressable registers, so `ref_captured_symbols` drives a pre-pass that boxes each by-reference-captured local into a one-slot heap cell which both frames then read and write through.
  - **Borrows** — a `&`/`&mut` entry registers a real borrow of the named local, live for as long as the closure is, through the same `live_view` machinery that tracks `slice`/`cell` views (`src/semantic/borrow_check.cpp`). Two closures holding `&mut x` at once is the same error as two `mut slice` views of one collection.
  - Exercised by `063`–`066` in `src/testdata/codegen_stress/` (each with a stated `# expect:` value, since a by-reference capture silently degrading to a copy would degrade *identically* on both backends and satisfy a cross-backend agreement check), plus fixtures in `src/testdata/semantic_check_test/` and `src/testdata/semantic_borrow_check_test/`.
- The three ways to vary returned behavior: the dependent-type and sum-type forms rely on ordinary generics and sum types, both implemented (see [Generics and Inference](19-generics-and-inference.md)). `box[fn(A) -> B]` does **not** work today — no trait-object/vtable dynamic dispatch exists anywhere in the compiler (no lowering for `box` was found in `src/hir/`, `src/bytecode/`, or `src/llvm_codegen/`; `box` is registered only as a bare type constructor in `src/semantic/types.cpp`). See [Trait Objects](24-trait-objects.md) for the equivalent finding on `box[trait]` generally.

## See also

- [Ownership and Borrowing](14-ownership-and-borrowing.md) — the borrow/move distinction closures follow.
- [`crew`, `par`, `race`](28-crew-par-race.md) — full structured-concurrency semantics for spawn-site borrowing.
- [Trait Objects](24-trait-objects.md) — `box[trait]` as the general form of `box[fn(A) -> B]`.
