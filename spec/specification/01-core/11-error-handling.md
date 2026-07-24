# 11. Error Handling

**Status:** Partial — see Implementation status

Covers `option[T]`/`result[T, E]`, the `?` operator, and the distinction between panics and errors.

## `option` and `result`

Kira has no exceptions. A function that might produce nothing returns `option[T]`; a function that can fail returns `result[T, E]`, `T` the success value and `E` the error. Both are ordinary sum types (`src/std/option.kira`, `src/std/result.kira`):

```kira
type option[T] = @some(T) | @none
type result[T, E] = @ok(T) | @err(E)
```

Inspect them with `match`:

```kira
def find_user(id: int32) -> option[user]:
    ...

match find_user(42):
    @some(u) => println("Found: {u.name}")
    @none    => println("Not found")
```

Errors are ordinary user-defined sum types:

```kira
type app_error =
    | @file_not_found(str)
    | @permission_denied(str)
    | @parse_failed(str)
```

## `?`

Inside a function whose return type is `result`/`option`, `expr?` unwraps `@ok`/`@some` and yields the payload; on `@err`/`@none` it returns that value from the enclosing function immediately.

```kira
def load_config(path: str) -> result[config, io_error]:
    let text   = read_file(path)?     # read_file's error is io_error
    let parsed = parse_toml(text)?    # parse_toml's error is parse_error
    return @ok(parsed)
```

The checker (`infer_try` in `src/semantic/check.cpp`) requires:

1. The operand's type is `result[_, _]` or `option[_]` — otherwise: `` `?` requires a `result` or `option` value, found `{type}` ``.
2. The enclosing function's return type (when known) is also `result`/`option` — otherwise: `` cannot use `?` in a function that returns `{type}` ``.

`?` on `option` behaves the same way: `e?` yields the inner value on `@some`, and returns `@none` from the function on `@none`.

## Implementation status

The design describe `?` as *converting* a propagated error through `from` when the operand's error type differs from the enclosing function's declared error type — e.g. propagating a `parse_error` out of a function that returns `result[_, io_error]` via `impl from[parse_error] for io_error`. **This conversion is not implemented.** `lower_try` in `src/hir/lower.cpp` desugars `x?` to a two-arm match whose failure arm returns the original subject value *unchanged*:

> "The failure arm returns the *original* subject value rather than reconstructing `@err(e)`/`@none` — it's already exactly that value; the checker only requires the enclosing function to also return a result/option (not that the two share the same success type), so nothing needs rebuilding here." (`src/hir/lower.cpp`, `lower_try`)

`infer_try` in `src/semantic/check.cpp` likewise only checks that both sides are `result`/`option` wrappers — it does not compare the operand's error type against the function's declared error type, and no `from`-conversion call is inserted anywhere in the desugaring. In practice, `?` only propagates cleanly today when the operand's error type is identical to (or otherwise unifies with) the enclosing function's error type; the multi-error-type example above, and the accompanying `` cannot propagate `parse_error` with `?` `` / "no conversion from ... exists" diagnostic shape from the old tutorial, describe a target design with no corresponding implementation found in `src/semantic` or `src/hir`.

## Panics vs. errors

A panic means a bug — an out-of-bounds index, `.unwrap()` on a `@none` believed to be `@some`. Panics are not for expected failure; use `result` for those. `panic()` is a recognized prelude intrinsic (`src/semantic/check.cpp`); `.unwrap()` is defined on `option`/`result` in `src/std/option.kira` / `src/std/result.kira`.

```kira
let v = some_list[999]      # panics if index is out of bounds
let x = opt.unwrap()        # panics if opt is none
panic("should never reach here")
```

## See also

- [Built-in Types](02-built-in-types.md) — the `from` mechanism `?` uses (in its designed, not-yet-implemented form) for conversion.
- [Pattern Matching](09-pattern-matching.md) — `if let`/`while let`, the common way to inspect a single `option`/`result` case.
