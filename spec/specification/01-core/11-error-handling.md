# 11. Error Handling

**Status:** Implemented

Covers `option[T]`/`result[T, E]`, the `?` operator, and the distinction between panics and errors.

## `option` and `result`

 Cinder has no exceptions. A function that might produce nothing returns `option[T]`; a function that can fail returns `result[T, E]`, `T` the success value and `E` the error. Both are ordinary sum types (`src/std/option.cn`, `src/std/result.cn`):

```cinder
type option[T] = @some(T) | @none
type result[T, E] = @ok(T) | @err(E)
```

Both compare with `==` when what they hold does: two `@none`s are equal, and two `@some`s, or two values of the same `result` variant, are equal when their payloads are ([Equality on tuples, arrays, `option` and `result`](../02-intermediate/20-operator-overloading.md#equality-on-tuples-arrays-option-and-result)).

Inspect them with `match`:

```cinder
def find_user(id: int32) -> option[user]:
    ...

match find_user(42):
    @some(u) => println("Found: {u.name}")
    @none    => println("Not found")
```

Errors are ordinary user-defined sum types:

```cinder
type app_error =
    | @file_not_found(str)
    | @permission_denied(str)
    | @parse_failed(str)
```

## `?`

Inside a function whose return type is `result`/`option`, `expr?` unwraps `@ok`/`@some` and yields the payload; on `@err`/`@none` it returns that value from the enclosing function immediately.

```cinder
def load_config(path: str) -> result[config, io_error]:
    let text   = read_file(path)?     # read_file's error is io_error
    let parsed = parse_toml(text)?    # parse_toml's error is parse_error
    return @ok(parsed)
```

The checker (`infer_try` in `src/semantic/check.cpp`) requires:

1. The operand's type is `result[_, _]` or `option[_]` — otherwise: `` `?` requires a `result` or `option` value, found `{type}` ``.
2. The enclosing function's return type (when known) is also `result`/`option` — otherwise: `` cannot use `?` in a function that returns `{type}` ``.

`?` on `option` behaves the same way: `e?` yields the inner value on `@some`, and returns `@none` from the function on `@none`.

### Error-type conversion via `from`

When the operand is `result[_, E1]` and the enclosing function returns `result[_, E2]` with `E1 != E2`, `?` requires `impl from[E1] for E2` (`std.traits.from`, `src/std/traits.conversion.cn`) and applies it automatically: the failure arm reconstructs `@err(E2.from(e))` instead of forwarding `e` unchanged.

```cinder
use std.traits.from

impl from[parse_error] for app_error:
    def from(e: parse_error) -> app_error:
        match e:
            @bad_digit(s) => @parse(s)
            @empty        => @parse("empty")

def load_config(path: str) -> result[config, app_error]:
    let text = read_file(path)?      # io_error propagates unchanged if app_error == io_error
    let n    = parse_num(text)?      # parse_error converts to app_error via the impl above
    return @ok(n)
```

No conversion is attempted (or needed) when the two error types already match, including when either is `unknown` (an unannotated context). When they differ and no matching `impl from[E1] for E2` exists, the checker reports:

```
error: cannot propagate `{E1}` with `?` in a function that returns `result[_, {E2}]`
  help: no conversion from this error type exists
  help: Add `impl from[{E1}] for {E2}` with a `from` method to convert between error types.
```

`option`'s `?` never triggers a conversion — `@none` carries no error payload to convert.

## `or else`

`x or else y` unwraps an `option[T]` or `result[T, E]` and has type `T`. The operand is evaluated once and consumed. On `@some(v)` or `@ok(v)` the result is `v`. On `@none` or `@err(_)` the fallback is evaluated and becomes the result. The fallback is evaluated only in that case.

```cinder
let port = config.port or else 8080
let f    = open(path, opts) or else panic("the config file is required")
let user = cache.get(id) or else return @none
let n    = parse(line) or else continue
```

The fallback is either a value of type `T` or an expression that does not return: `panic(...)`, `return [expr]`, `break` or `continue`. These jump forms are accepted only in this position. A literal fallback takes `T`'s width.

With a `result`, the `@err` payload is dropped and the fallback cannot see it. A program that needs the error uses `?`, `match` or `let ... else:`.

`or else` binds more loosely than `or` and more tightly than the pipe `|`, and groups to the right: `a or else b or else 0` is `a or else (b or else 0)`. In a task body the fallback may suspend, as in `o or else await fetch()`. Because `await` binds tighter than `or else`, write `(await f()) or else x` to unwrap an awaited value.

The checker (`infer_or_else` in `src/semantic/check_expr.cpp`) reports these errors:

1. The operand is not an `option` or `result`: `` `or else` needs an `option` or `result` on its left ``.
2. The fallback has the wrong type: ``the fallback for an `option[str]` must be a `str`, found `int32` ``.
3. `or` used with an `option` or `result` operand gets the added help ``did you mean `or else`?``.

`or else` lowers to a two-arm `match`, as `?` does, so it needs no support in either backend or in compile-time evaluation.

## Limitations

Method resolution for `from` selects the first `impl from[...] for E2` block found for `E2`, regardless of its trait argument — a type with two `impl from[...]` blocks for different source error types does not reliably disambiguate between them. 

## Panics vs. errors

A panic means a bug — an out-of-bounds index, or a `@none` believed to be `@some`. Panics are not for expected failure; use `result` for those. `panic()` is a recognized prelude intrinsic (`src/semantic/check.cpp`). `option` and `result` have no `unwrap`; write `x or else panic("...")`.

```cinder
let v = some_list[999]      # panics if index is out of bounds
let x = opt or else panic("opt is none")   # panics if opt is none
panic("should never reach here")
```

A panic terminates the program. It prints `panic: <message>` on stderr and
exits with status 101 — the same message and the same status whichever
backend ran the program, and whether the panic came from `panic()` in Cinder
or from a check the compiler emitted. A panic in a task prints at once and
then propagates to whoever awaits the task, stopping the task's siblings on the
way ([`crew`, `par`, `race`](../02-intermediate/28-crew-par-race.md)); the
program ends when it reaches the entry function. A panic in a task nothing
awaits prints, and the program carries on.

An out-of-range index is the same event on every container. `array[T, N]`,
`slice[T]` and `str` are bounds-checked by the compiler; `list[T]` checks
itself, in ordinary Cinder (`src/std/list.cn`). Both report `index out of
bounds` and terminate: there is nothing a program can do about having been
wrong about a container's extent, so a bounds violation is never handed back
to anything — not to the program, and not to a host embedding a Cinder tier.

## See also

- [Built-in Types](02-built-in-types.md) — the `from`/`into` traits `?` uses for error-type conversion.
- [Pattern Matching](09-pattern-matching.md) — `if let`/`while let`, the common way to inspect a single `option`/`result` case.
