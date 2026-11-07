# `or else` Plan

This plan adds an `or else` operator. `x or else y` gives the value inside an `option` or `result`, or evaluates the fallback `y` when there is none. The fallback is either a value of the payload type or an expression that does not return: `panic(...)`, `return`, `break` or `continue`. The operator lowers to a `match` on its operand, like `?`, so both backends, compile-time evaluation and coroutines need no new code. Once it works, it replaces `option.unwrap`, `option.unwrap_or` and `result.unwrap`, which are then deleted. Deleting them removes the standard library's cases of todo item 16, where `unwrap` moves a payload out of a borrowed `self`. The work is about seven small steps. The first fixes todo item 18, because `or else panic(...)` lowers to the same construct that item describes.

## The language feature

`or else` is a two-word operator, spelled like `not in`. Its left side is an `option[T]` or a `result[T, E]`, and the whole expression has type `T`.

```cinder
let port = config.port or else 8080
let f    = open(path, opts) or else panic("the config file is required")
let user = cache.get(id) or else return @none
let n    = parse(line) or else continue
```

- **Evaluation.** The left side is evaluated once and consumed. On `@some(v)` or `@ok(v)` the result is `v`. On `@none` or `@err(_)` the fallback is evaluated and is the result. The fallback is evaluated only in that case.
- **The fallback.** It must have type `T`, or type `never`. A `panic(...)` call has type `never`. `return [expr]`, `break` and `continue` are statements elsewhere in the language and are accepted as the fallback, where they also have type `never`. They are not made expressions anywhere else.
- **The error is discarded.** With a `result`, the `@err` payload is dropped and the fallback cannot see it. A program that needs the error uses `?`, `match` or `let ... else:`.
- **Precedence.** `or else` binds more loosely than `or` and more tightly than the pipe `|`. It groups to the right, so `a or else b or else 0` is `a or else (b or else 0)`. The fallback may itself suspend, as in `o or else await fetch()`.
- **Nothing breaks.** `or` followed by `else` is a parse error today, so no existing program changes meaning.

## Diagnostics

The compiler explains the rule in plain terms at each of these points:

- **A left side that is not `option` or `result`.** "`or else` needs an `option` or `result` on its left, found `int32`". The help says what `or else` does.
- **A fallback of the wrong type.** "the fallback for an `option[str]` must be a `str`, found `int32`". The help adds that a fallback may also be something that does not return, such as `panic(...)` or `return`.
- **`or` with an `option` or `result` operand.** The existing "expected `bool`" error gains the help "did you mean `or else`?".
- **A call to a deleted `unwrap` or `unwrap_or`.** Once step 7 lands, the "no method" error says to use `or else panic("...")` or `or else <fallback>`.

## Implementation

The work happens in the front end and the HIR lowerer. The backends do not change.

1. **Fix todo item 18.** A `panic(...)` that is the value of a `match` arm in a non-generic function fails to lower with "no concrete checked type is available for this node". `or else panic(...)` lowers to that same arm. Fix it first, with a regression test, so the later steps do not stall on it.

2. **Parser and AST.** Add an `or_else_expr` node with `operand` and `fallback`, a new precedence level between `parse_pipe_expr` and `parse_or_expr`, and a parse rule for the jump forms in the fallback position. Every place that lists `try_expr` gets a matching entry: `ast_clone.cpp`, `ast_traversal.h`, `session.cpp`'s `walk_node`, `snapshot.cpp`'s `node_kind_name`, and `check_lowering_view.cpp`. Update `cinder-grammar.ebnf`, including its precedence table. Add parser tests for precedence, right grouping, the jump forms, and the interaction with `x if c else y`.

3. **Checker.** Add `infer_or_else` next to `infer_try` in `check_expr.cpp`. It infers the operand, rejects anything that is not `option` or `result`, and checks the fallback against `T` as the expected type, so a literal fallback takes `T`'s width. It then joins the two the way `join_branch_type` does, with `never` as the bottom, and uses the wording above in place of the generic "`match` branches have mismatched types". In a generic body the operand's head (`option` or `result`) is always known, so no deferred decision is needed. A test confirms this in a template over `T`.

4. **Ownership and drops.** In `ownership_cfg.cpp`, model the node as a fork. The operand is consumed. The success path produces the payload, and the failure path evaluates the fallback. On the failure path of a `result`, the `@err` payload is dropped, since nothing binds it. The drop records are keyed by the AST node, as the general ownership model requires, so the lowerer reads them instead of deciding drops itself.

5. **Lowering.** Add `lower_or_else` beside `lower_try` in `src/hir/lower.cpp`. It builds a two-arm `hir_match`. The success arm yields the payload, and the failure arm yields the lowered fallback or holds the jump, with `emit_jump_drops` for `return`, `break` and `continue`. It reuses `lower_try`'s generator handling for `return` inside a coroutine. `branch_value_can_suspend` counts a fallback that can suspend. The VM, LLVM and compile-time tiers then run it with no changes.

6. **Tests.** Corpus files in `src/testdata/codegen_stress/` run on both tiers and declare `# expect:` values, because these cases are about drops and representation, which agreement between the tiers does not catch. Each test is broken on purpose once to confirm it can fail.
   - A value fallback, a `panic` fallback, and each jump form.
   - A payload with `impl drop` that is moved out and dropped exactly once. This is todo item 16's repro rewritten with `or else`.
   - A `result` whose `@err` payload has `impl drop` and is dropped exactly once on the failure path.
   - A fallback that awaits, and an operand that is live across an `await`.
   - Use inside a generic function and in compile-time code.
   - Diagnostic tests for each message above, and a refreshed inference snapshot.

7. **Migration and removal.** Rewrite the two calls in `src/std/algo.cn` and the eight test files that use `.unwrap()`. Delete `unwrap` and `unwrap_or` from `src/std/option.cn` and `src/std/result.cn`, and add the "no method" help. Remove the `unwrap` caveat from `54-std-io.md`, and narrow todo item 16 to the general case of a method moving out of `self`, which the language still has to decide.

## Documentation

These updates go in the same change as the code:

- **Chapter 11, error handling.** A new `or else` section next to `?`. The panics section shows `or else panic(...)` in place of `.unwrap()`.
- **Chapter 9, pattern matching.** `let ... else:` is in the grammar but no chapter describes it. Document it here and point to `or else` for the single-expression case.
- **Implementation chapter 66.** One line saying that `or else` and `?` both lower to a `hir_match`.

## Decisions to confirm

- **Deleting `unwrap`.** The plan deletes `unwrap` and `unwrap_or` rather than keeping them alongside `or else`. Keeping them would leave two ways to do one thing and keep the `self` ownership bug in the standard library.
- **No error binding.** A form that names the error, such as `r or else e => ...`, is left out. It can be added later without changing the meaning of the form in this plan.

## Risks

- **A silently dropped or leaked error payload.** If step 4 misses the `@err` drop, programs leak without any error. The drop-count corpus test in step 6 is the check for this.
- **Todo item 18 may be deeper than it looks.** If its fix is large, steps 5 and 6 wait on it. A value fallback does not depend on it and can land first.
- **`return` as a fallback.** Accepting `return` only in this position is a special case in the parser. If more such positions come up later, making jumps into `never`-typed expressions everywhere would be the cleaner change.
