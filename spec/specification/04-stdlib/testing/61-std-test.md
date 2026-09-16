# 61. Unit Testing (`std.test`)

**Status:** Planned

Defines `std.test`: the `test_failure` type, assertion functions, test-case registration, and the runner that executes a set of test cases and reports results.

## Syntax

No new grammar. A test is an ordinary function; `std.test` types and functions are used like any other stdlib module.

## Semantics

### Test functions

A test function is any `def name() -> result[unit, test_failure]` taking no parameters. It returns `@ok(unit)` on success and `@err(test_failure)` on failure.

```kira
def test_addition() -> result[unit, test_failure]:
    assert_eq(2 + 2, 4)
```

A test function's name has no special meaning by itself; it becomes part of a test run only when passed to `case`/`skipped` ([Test cases](#test-cases)) or matched by name during [discovery](#discovery). Naming a function `test_*` is convention, not a compiler-recognized marker, except where discovery is used.

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

Per-test fixtures (setup/teardown scoped to a single test) are ordinary function calls made at the start and end of a test function's body; no dedicated syntax exists for them.

### Suites

A `test_suite` groups `test_case`s with optional `before_all`/`after_all` hooks that run once for the group, rather than once per case:

```kira
type test_suite = {
    name:       str,
    cases:      list[test_case],
    before_all: option[fn() -> result[unit, test_failure]],
    after_all:  option[fn() -> result[unit, test_failure]],
}

def suite(
    name:       str,
    cases:      list[test_case],
    before_all: option[fn() -> result[unit, test_failure]] = @none,
    after_all:  option[fn() -> result[unit, test_failure]] = @none,
) -> test_suite
```

- `before_all`, when `@some`, is called once before any case in `cases` runs.
- `after_all`, when `@some`, is called once after every case in `cases` has run (or been skipped), whether or not any case failed.
- If `before_all` returns `@err`, no case in `cases` runs; each is recorded as failed with that same `test_failure`. `after_all` still runs.
- `before_all`/`after_all` failures are reported as their own named entries (`<suite name>.before_all`, `<suite name>.after_all`) in the runner's output, distinct from any case.

### Discovery

`suite`/`case`/`skipped` register tests explicitly, for use in a hand-written `main`. `kira test` discovers and runs tests automatically, without any `std.test` call in the tested code.

Discovery is scoped to an inline submodule literally named `tests` (`sub_module_decl`, see [Modules and Imports](../../01-core/12-modules-and-imports.md#use)), not to top-level names in an ordinary module — a name like `before_all` outside a `tests` submodule is an ordinary name and is never treated as a hook:

```kira
module app.geometry:
    def before_all() -> unit:        # ordinary function; not a test hook
        ...

    pub def area(w: float64, h: float64) -> float64:
        w * h

    module tests:
        def before_all() -> result[unit, test_failure]:
            ...

        def test_area() -> result[unit, test_failure]:
            assert_eq(super.area(2.0, 3.0), 6.0)

        def skip_negative() -> result[unit, test_failure]:
            ...
```

- Invoked as `kira test <path>`, in place of `kira <path>`.
- For every module reachable from `<path>`, the driver looks for a direct inline submodule of it named `tests`. A module without one contributes nothing.
- Within a `tests` submodule, every function taking no parameters and returning `result[unit, test_failure]` is classified by name:
  - exactly `before_all` — the suite's `before_all` hook.
  - exactly `after_all` — the suite's `after_all` hook.
  - prefix `skip_` — a case built with `skipped`, named after the function (with the `skip_` prefix kept in the reported name).
  - anything else — a case built with `case`, named after the function.
- A function inside `tests` whose signature does not match (extra parameters, other return type) takes no part in the suite; it is ordinary helper code private to the submodule.
- A `tests` submodule with two or more functions named exactly `before_all`, or two or more named exactly `after_all`, is a compile error.
- Each `tests` submodule with at least one discovered case contributes one `test_suite`, named after its parent module's path; a `tests` submodule with only hooks and no cases contributes no suite.
- The driver synthesizes a `main` equivalent to calling `run_suites` on every discovered suite, in module-graph order, and exits with its return code. A module that already declares its own `main` is left as-is; `kira test` does not override a user-written entry point.

### Runner

```kira
def run(cases: list[test_case]) -> int32
def run_suites(suites: list[test_suite]) -> int32
```

- `run` executes every `test_case` in `cases` in order, except those with `skip = true`; equivalent to `run_suites([suite("", cases)])` with no per-suite name printed.
- `run_suites` executes each `test_suite` in order: its `before_all` (if any), then each of its `cases` in order (except those with `skip = true`), then its `after_all` (if any).
- For each executed case or hook, calls it and records `@ok(unit)` as passed or `@err(test_failure)` as failed.
- For each skipped case, records it as skipped without calling it.
- Prints one line per case or suite hook:
  - passed: `ok <name>`
  - failed: `FAILED <name>: <rendered test_failure>`
  - skipped: `skip <name>`
- `@message(text)` renders as `text`. `@mismatch(expected, actual, message)` renders as `message (expected: <expected>, actual: <actual>)`.
- Prints a summary line: `<passed> passed, <failed> failed, <skipped> skipped`.
- Returns `0` if `failed == 0` across all cases and hooks, otherwise `1`.

## Example

```kira
use std.test.{case, skipped, suite, run_suites, assert_eq, assert_true, test_failure}

var counter: int32 = 0

def reset_counter() -> result[unit, test_failure]:
    counter = 0
    @ok(unit)

def test_addition() -> result[unit, test_failure]:
    assert_eq(2 + 2, 4)

def test_flaky() -> result[unit, test_failure]:
    assert_true(false, "not ready")

def test_increment() -> result[unit, test_failure]:
    counter = counter + 1
    assert_eq(counter, 1)

def main() -> int32:
    return run_suites([
        suite("arithmetic", [case("addition", test_addition), skipped("flaky", test_flaky)]),
        suite("counter", [case("increment", test_increment)], before_all: @some(reset_counter)),
    ])
```

## See also

- [Error Handling](../../01-core/11-error-handling.md) — `result[T, E]` and `?`, used throughout test bodies and assertions.
- [Lambdas](../../01-core/05-lambdas.md) — closures used as `test_case.run` values.
- [Traits](../../02-intermediate/18-traits.md) — the `show`/`eq` bounds on `assert_eq`/`assert_ne`.
- [`std.format`](../strings-and-formatting/53-std-format.md) — the `show` trait used to render mismatch values.
- [Programs and `main`](../../01-core/13-programs-and-main.md) — the ordinary entry-point rules `kira test`'s synthesized `main` follows.
- [Modules and Imports](../../01-core/12-modules-and-imports.md) — module paths and `pub` visibility, which `kira test` discovery is defined over.
