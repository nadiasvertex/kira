# 61. Unit Testing (`std.test`)

**Status:** Planned

Defines `std.test`: the `test_failure` type, assertion functions, test-case registration, and the runner that executes a set of test cases and reports results.

## Syntax

No new grammar. A test is an ordinary function; `std.test` types and functions are used like any other stdlib module.

## Semantics

### Test functions

A test function is any `def name() -> result[unit, test_failure]`. It takes no parameters. It returns `@ok(unit)` on success and `@err(test_failure)` on failure.

```kira
def test_addition() -> result[unit, test_failure]:
    assert_eq(2 + 2, 4)
```

### `test_failure`

```kira
type test_failure =
    | @message(str)
    | @mismatch(expected: str, actual: str, message: str)
```

- `@message(text)` — a failure described only by `text`.
- `@mismatch(expected, actual, message)` — a failure with an expected and an actual value, each already rendered to `str`, plus a description.

### Assertions

```kira
def assert_true(cond: bool, message: str) -> result[unit, test_failure]
def assert_false(cond: bool, message: str) -> result[unit, test_failure]
def assert_eq[T: show + eq](actual: T, expected: T) -> result[unit, test_failure]
def assert_ne[T: show + eq](actual: T, expected: T) -> result[unit, test_failure]
def fail(message: str) -> result[unit, test_failure]
```

- `assert_true`/`assert_false` return `@ok(unit)` iff `cond` matches; otherwise `@err(@message(message))`.
- `assert_eq` returns `@ok(unit)` iff `actual == expected`; otherwise `@err(@mismatch(expected.show(), actual.show(), "values not equal"))`.
- `assert_ne` returns `@ok(unit)` iff `actual != expected`; otherwise `@err(@mismatch(expected.show(), actual.show(), "values unexpectedly equal"))`.
- `fail` always returns `@err(@message(message))`.
- A test function composes assertions with `?`:

```kira
def test_parse() -> result[unit, test_failure]:
    let n = parse_int("42")?
    assert_eq(n, 42)?
    assert_true(n > 0, "parsed value must be positive")
```

### Test cases

```kira
type test_case = { name: str, run: fn() -> result[unit, test_failure], skip: bool }

def case(name: str, run: fn() -> result[unit, test_failure]) -> test_case
def skipped(name: str, run: fn() -> result[unit, test_failure]) -> test_case
```

- `case(name, run)` builds a `test_case` with `skip = false`.
- `skipped(name, run)` builds a `test_case` with `skip = true`.

Parameterized tests are ordinary data-driven code that builds a `list[test_case]`:

```kira
def cases_for_add() -> list[test_case]:
    let inputs = [(1, 1, 2), (2, 2, 4), (0, 0, 0)]
    for (a, b, expected) in inputs =>
        case("add({a}, {b})", () => assert_eq(a + b, expected))
```

Fixtures (setup/teardown) are ordinary function calls made at the start and end of a test function's body; no dedicated fixture syntax exists.

### Runner

```kira
def run(cases: list[test_case]) -> int32
```

- Executes every `test_case` in `cases` in order, except those with `skip = true`.
- For each executed case, calls `run` and records `@ok(unit)` as passed or `@err(test_failure)` as failed.
- For each skipped case, records it as skipped without calling `run`.
- Prints one line per case:
  - passed: `ok <name>`
  - failed: `FAILED <name>: <rendered test_failure>`
  - skipped: `skip <name>`
- `@message(text)` renders as `text`. `@mismatch(expected, actual, message)` renders as `message (expected: <expected>, actual: <actual>)`.
- Prints a summary line: `<passed> passed, <failed> failed, <skipped> skipped`.
- Returns `0` if `failed == 0`, otherwise `1`.

## Example

```kira
use std.test.{case, skipped, run, assert_eq, assert_true, test_failure}

def test_addition() -> result[unit, test_failure]:
    assert_eq(2 + 2, 4)

def test_flaky() -> result[unit, test_failure]:
    assert_true(false, "not ready")

def main() -> int32:
    return run([
        case("addition", test_addition),
        skipped("flaky", test_flaky),
    ])
```

## See also

- [Error Handling](../../01-core/11-error-handling.md) — `result[T, E]` and `?`, used throughout test bodies and assertions.
- [Lambdas](../../01-core/05-lambdas.md) — closures used as `test_case.run` values.
- [Traits](../../02-intermediate/18-traits.md) — the `show`/`eq` bounds on `assert_eq`/`assert_ne`.
- [`std.format`](../strings-and-formatting/53-std-format.md) — the `show` trait used to render mismatch values.
