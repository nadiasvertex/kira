# `std.time`

**Status:** Implemented

Durations, the monotonic clock, and sleeping. A sleeping task gives up its thread until its deadline passes, so other tasks run in the meantime.

`std.time` (`src/std/time.cn`) is written in Cinder over two intrinsics: `rt_monotonic_now` reads the clock, and `rt_sleep_until` returns a task that the event loop behind the `io` context finishes at a deadline ([The Task Scheduler](../../05-implementation/69-task-scheduler.md#the-io-event-loop)).

## Types

```cinder
pub type duration = { nanos: uint64 }
pub type instant = { nanos: uint64 }
```

- `duration` is a span of time in nanoseconds.
- `instant` is a point on the monotonic clock. The clock never goes backwards, and its starting point is arbitrary, so instants are compared and subtracted within one run of a program and mean nothing on their own.

## Durations

```cinder
extend duration:
    pub def from_nanos(n: uint64) -> duration
    pub def from_micros(n: uint64) -> duration
    pub def from_millis(n: uint64) -> duration
    pub def from_secs(n: uint64) -> duration
    pub def as_nanos(self) -> uint64
    pub def as_micros(self) -> uint64
    pub def as_millis(self) -> uint64
    pub def as_secs(self) -> uint64
```

The `from_` functions build a duration from a count of the named unit. The `as_` methods return the whole duration in that unit, rounded down.

## Instants

```cinder
pub def now() -> instant

extend instant:
    pub def duration_since(self, earlier: &instant) -> duration
    pub def elapsed(self) -> duration
    pub def after(self, d: &duration) -> instant
```

- `now` reads the monotonic clock.
- `duration_since` is the time from `earlier` to this instant, or zero if `earlier` is later.
- `elapsed` is the time from this instant to now.
- `after` is the instant `d` after this one.

## Sleeping

```cinder
pub async def sleep(d: duration) -> unit
pub async def sleep_until(t: instant) -> unit
```

`sleep` suspends the calling task for at least `d`. `sleep_until` suspends it until the clock reaches `t`, and returns at once if `t` has passed. A sleeping task holds no thread, so many tasks can sleep on one `io` worker at once. A sleep is cancelled like any other `await`: a `race` that a sleeper loses stops it without waiting for its deadline.

```cinder
use std.time.{duration, sleep}

async def nap(ms: uint64, tag: int32) -> int32:
    await sleep(duration.from_millis(ms))
    return tag

async def main() -> int32:
    # Both naps run at once, so this takes about 50 ms, not 100.
    let (a, b) = await par:
        nap(50, 1)
        nap(50, 2)
    # The 10 ms nap wins; the 5 s one is cancelled.
    let first = await race:
        nap(5000, 0)
        nap(10, 40)
    return a + b + first
```

## Implementation status

Everything in this chapter is implemented on both backends. A sleep that is cancelled stays in the event loop's timer queue until its deadline, where it is discarded.

## See also

- [`std.io`](54-std-io.md#asynchronous-io) — asynchronous reads and writes, which wait in the same event loop.
- [Concurrency: The Execution Model](../../02-intermediate/26-concurrency-execution-model.md) — the `io` context.
