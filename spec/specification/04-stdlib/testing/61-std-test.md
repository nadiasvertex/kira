# 61. Unit Testing (`std.test`)

**Status:** Partial

Defines `std.test`: the `test_failure` type, assertion functions, test-case registration, and the runner that executes a set of test cases and reports results.

## Syntax

No new grammar. A test is an ordinary function; `std.test` types and functions are used like any other stdlib module.

## Semantics

### Test functions

A test function is any `def name() -> result[unit, test_failure]` taking no parameters. It returns `@ok(unit)` on success and `@err(test_failure)` on failure.

```kira
def test_addition() -> result[unit, test_failure]:
    return assert_eq(2 + 2, 4)
```

A test function's name has no special meaning by itself; it becomes part of a test run only when passed to `case`/`skipped` ([Test cases](#test-cases)) or matched by name during [discovery](#discovery). Naming a function `test_*` is convention, not a compiler-recognized marker, except where discovery is used.

### `test_failure`

Sum-type variants carry only positional fields (`sum_variant` in `spec/kira-grammar.ebnf`), so a mismatch's three parts are unnamed and distinguished by position:

```kira
type test_failure =
    | @message(str)
    | @mismatch(str, str, str)
```

- `@message(text)` — a failure described only by `text`.
- `@mismatch(expected, actual, message)` — in that positional order: an expected value and an actual value, each already rendered to `str`, plus a description.

### Assertions

```kira
def assert_true(cond: bool, message: str) -> result[unit, test_failure]
def assert_false(cond: bool, message: str) -> result[unit, test_failure]
def assert_eq[T: show + eq](actual: T, expected: T) -> result[unit, test_failure]
def assert_ne[T: show + eq](actual: T, expected: T) -> result[unit, test_failure]
def fail(message: str) -> result[unit, test_failure]
```

- `assert_true`/`assert_false` return `@ok(unit)` iff `cond` matches; otherwise `@err(@message(message))`.
- `assert_eq` returns `@ok(unit)` iff `actual == expected`; otherwise `@err(@mismatch(rendered_expected, rendered_actual, "values not equal"))`. Each value is rendered via string interpolation (`"{value}"`), not a direct `.show()` call — a built-in scalar type (`int32`, `float64`, ...) has no callable `show` method of its own, only the compiler's built-in interpolation path for it, so rendering must go through interpolation to work uniformly across both built-in and user-defined `show` types.
- `assert_ne` returns `@ok(unit)` iff `actual != expected`; otherwise `@err(@mismatch(rendered_expected, rendered_actual, "values unexpectedly equal"))`, rendered the same way.
- `fail` always returns `@err(@message(message))`.
- A test function composes assertions with `?`:

```kira
def test_parse() -> result[unit, test_failure]:
    let n = parse_int("42")?
    assert_eq(n, 42)?
    return assert_true(n > 0, "parsed value must be positive")
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
    return for (a, b, expected) in inputs =>
        case("add({a}, {b})", () => assert_eq(a + b, expected))
```

Per-test fixtures (setup/teardown scoped to a single test) are ordinary function calls made at the start and end of a test function's body; no dedicated syntax exists for them.

### Suites

A `test_suite` groups `test_case`s with optional hooks: `before_all`/`after_all` run once for the whole group, `before_each`/`after_each` run once per case:

```kira
type test_suite = {
    name:        str,
    cases:       list[test_case],
    before_all:  option[fn() -> result[unit, test_failure]],
    after_all:   option[fn() -> result[unit, test_failure]],
    before_each: option[fn() -> result[unit, test_failure]],
    after_each:  option[fn() -> result[unit, test_failure]],
}

def suite(
    name:        str,
    cases:       list[test_case],
    before_all:  option[fn() -> result[unit, test_failure]] = @none,
    after_all:   option[fn() -> result[unit, test_failure]] = @none,
    before_each: option[fn() -> result[unit, test_failure]] = @none,
    after_each:  option[fn() -> result[unit, test_failure]] = @none,
) -> test_suite
```

- `before_all`, when `@some`, is called once before any case in `cases` runs.
- `after_all`, when `@some`, is called once after every case in `cases` has run (or been skipped), whether or not any case or hook failed.
- If `before_all` returns `@err`, no case in `cases` runs (nor do `before_each`/`after_each` for any of them); each case is recorded as failed with that same `test_failure`. `after_all` still runs.
- `before_all`/`after_all` failures are reported as their own named entries (`<suite name>.before_all`, `<suite name>.after_all`) in the runner's output, distinct from any case.
- `before_each`, when `@some`, is called immediately before each non-skipped case's `run`.
- `after_each`, when `@some`, is called immediately after each non-skipped case, and is itself reported as its own named entry (`<case name>.after_each`) whether it passes or fails — separate from the case's own pass/fail entry.
- If `before_each` returns `@err`, the case's `run` is not called; the case is recorded as failed with that `test_failure`. `after_each` still runs for that case.
- `before_each`/`after_each` are not called for a skipped case.

### Discovery

`suite`/`case`/`skipped` register tests explicitly, for use in a hand-written `main`. `kira --test` discovers and runs tests automatically, without any `std.test` call in the tested code.

Discovery is scoped to an inline submodule literally named `tests` (`sub_module_decl`, see [Modules and Imports](../../01-core/12-modules-and-imports.md#use)), not to top-level names in an ordinary module — a name like `before_all` outside a `tests` submodule is an ordinary name and is never treated as a hook:

```kira
module app.geometry

use std.test.{assert_eq, assert_true, test_failure}

def before_all() -> unit:        # ordinary function; not a test hook
    println("not a test hook")

pub def area(w: float64, h: float64) -> float64:
    return w * h

module tests:
    def before_all() -> result[unit, test_failure]:
        println("geometry suite starting")
        return @ok(unit)

    def after_all() -> result[unit, test_failure]:
        println("geometry suite done")
        return @ok(unit)

    def before_each() -> result[unit, test_failure]:
        return @ok(unit)

    def after_each() -> result[unit, test_failure]:
        return @ok(unit)

    def test_area() -> result[unit, test_failure]:
        return assert_eq(super.area(2.0, 3.0), 6.0)

    def test_zero_area() -> result[unit, test_failure]:
        return assert_eq(super.area(0.0, 5.0), 0.0)

    def skip_negative() -> result[unit, test_failure]:
        return assert_true(super.area(-1.0, 5.0) >= 0.0, "negative width should not underflow")
```

`kira --test` compiles this module, discovers the `tests` submodule as a suite named `app.geometry`, and prints (`demo/test-discovery.kira` is a runnable version of this, with nested submodules each carrying their own suite):

```
geometry suite starting
ok app.geometry.before_all
ok app.geometry.test_area
ok app.geometry.test_area.after_each
ok app.geometry.test_zero_area
ok app.geometry.test_zero_area.after_each
skip app.geometry.skip_negative
geometry suite done
ok app.geometry.after_all
6 passed, 0 failed, 1 skipped
```

- Invoked as `kira --test <path>`, in place of `kira <path>`.
- For every module reachable from `<path>`, the driver looks for a direct inline submodule of it named `tests`. A module without one contributes nothing. An inline submodule is itself a module, so this applies at every nesting depth: a `tests` submodule of a submodule is a suite named after *that* submodule's path, letting each submodule keep its tests next to the code they cover.
- A `tests` submodule is not searched for a further `tests` submodule of its own.
- Within a `tests` submodule, every function taking no parameters and returning `result[unit, test_failure]` is classified by name:
  - exactly `before_all` — the suite's `before_all` hook.
  - exactly `after_all` — the suite's `after_all` hook.
  - exactly `before_each` — the suite's `before_each` hook.
  - exactly `after_each` — the suite's `after_each` hook.
  - prefix `skip_` — a case built with `skipped`, named after the function (with the `skip_` prefix kept in the reported name).
  - anything else — a case built with `case`, named after the function.
- A function inside `tests` whose signature does not match (extra parameters, other return type) takes no part in the suite; it is ordinary helper code private to the submodule.
- A `tests` submodule with two or more functions sharing any one of the four exact hook names is a compile error.
- Each `tests` submodule with at least one discovered case contributes one `test_suite`, named after its parent module's path; a `tests` submodule with only hooks and no cases contributes no suite.
- The driver synthesizes a `main` equivalent to calling `run_suites` on every discovered suite, in module-graph order, and exits with its return code. A source file that already declares its own `main` is compiled unchanged; `kira --test` does not override a user-written entry point.
- Sources that parse cleanly, declare no `main`, and yield no suite are an error naming the scanned sources and showing how a test is declared — `--test` was asked to run tests and there are none.

### Runner

```kira
def run(cases: list[test_case]) -> int32
def run_suites(suites: list[test_suite]) -> int32
```

- `run` executes every `test_case` in `cases` in order, except those with `skip = true`; equivalent to `run_suites([suite("", cases, @none, @none, @none, @none)])` with no per-suite name printed.
- `run_suites` executes each `test_suite` in order:
  1. its `before_all`, if any;
  2. for each case in `cases`, in order: if `skip = true`, record it skipped and move on; otherwise call `before_each` (if any), then — only if `before_each` did not fail — the case's `run`, then `after_each` (if any);
  3. its `after_all`, if any.
- For each executed case or hook, calls it and records `@ok(unit)` as passed or `@err(test_failure)` as failed.
- For each skipped case, records it as skipped without calling it or its `before_each`/`after_each`.
- Prints one line per case or hook:
  - passed: `ok <name>`
  - failed: `FAILED <name>: <rendered test_failure>`
  - skipped: `skip <name>`
- `@message(text)` renders as `text`. `@mismatch(expected, actual, message)` renders as `message (expected: <expected>, actual: <actual>)`.
- Prints a summary line: `<passed> passed, <failed> failed, <skipped> skipped`.
- Returns `0` if `failed == 0` across all cases and hooks, otherwise `1`.

## Example

```kira
use std.test.{case, skipped, suite, run_suites, assert_eq, assert_true, test_failure}

def before_all_hook() -> result[unit, test_failure]:
    println("counter suite starting")
    return @ok(unit)

def test_addition() -> result[unit, test_failure]:
    return assert_eq(2 + 2, 4)

def test_flaky() -> result[unit, test_failure]:
    return assert_true(false, "not ready")

def test_increment() -> result[unit, test_failure]:
    return assert_eq(1 + 1, 2)

def main() -> int32:
    return run_suites([
        suite("arithmetic", [case("addition", test_addition), skipped("flaky", test_flaky)],
              @none, @none, @none, @none),
        suite("counter", [case("increment", test_increment)],
              @some(before_all_hook), @none, @none, @none),
    ])
```

## Implementation status

Fully implemented and end-to-end tested — the checker fix, the library, and `--test` discovery all landed together (`src/semantic/check.cpp`'s `infer_method_call`, `src/llvm_codegen/codegen.cpp`'s `compile_function_value`, `src/std/test.kira`, `src/driver/test_discovery.cpp`, `src/cli_test.cpp`'s `test_build_runs_std_test_suite_via_llvm_tier`/`test_build_discovers_and_runs_tests_submodule_via_llvm_tier`/`test_build_discovers_nested_tests_submodules_via_llvm_tier`/`test_build_test_mode_leaves_existing_main_unchanged`/`test_test_mode_without_any_tests_is_an_error`/`test_test_mode_hooks_without_cases_find_no_tests`, and `src/testdata/codegen_stress/080_fn_typed_struct_field_call.kira`/`081_fn_typed_struct_field_list_heterogeneous.kira`) — one gap remains:

- **`suite`'s default parameter values are not lowered.** Default parameter values are a general compiler gap (`spec/todo.md`; the compiler's own diagnostic on hitting one reads "default parameter values are not lowered by the first milestone"), not specific to `std.test`. Until that lands, every call to `suite(...)` must pass all six arguments explicitly — `@none` for any hook a suite doesn't need — rather than omitting trailing ones, exactly as the `suite` calls in this chapter's examples do. `run`'s single-suite wrapper passes all four explicitly for the same reason.
- The synthesized `--test` runner never needs a bare cross-module function reference as a value (a separate, broader gap than the one this chapter's own fix addresses — a bare `use`-imported or qualified function name used as a plain value, outside call position, is not reliably lowered in every position yet); it always wraps each discovered function in a zero-arg lambda calling it by qualified path (`() => app.geometry.tests.test_area()`), which is unaffected.

## See also

- [Error Handling](../../01-core/11-error-handling.md) — `result[T, E]` and `?`, used throughout test bodies and assertions.
- [Lambdas](../../01-core/05-lambdas.md) — closures used as `test_case.run` values.
- [Traits](../../02-intermediate/18-traits.md) — the `show`/`eq` bounds on `assert_eq`/`assert_ne`.
- [`std.format`](../strings-and-formatting/53-std-format.md) — string interpolation, used to render mismatch values.
- [Programs and `main`](../../01-core/13-programs-and-main.md) — the ordinary entry-point rules `kira --test`'s synthesized `main` follows.
- [Modules and Imports](../../01-core/12-modules-and-imports.md) — module paths and `pub` visibility, which `kira --test` discovery is defined over.
