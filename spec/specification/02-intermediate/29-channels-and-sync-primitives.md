# 29. Channels and Sync Primitives

**Status:** Partial

`channel[T]` and `watch[T]`.

## `channel[T]`

Channels pass ownership of values between tasks:

```cinder
let (sender, receiver) = channel[str](capacity: 32)

crew c:
    c.spawn(async:
        for line in read_lines("file.txt"):
            await sender.send(line)
        sender.close()
    )
    c.spawn(async:
        while let @some(line) = await receiver.recv():
            process(line)
    )
```

Sending on a full channel suspends the sender (backpressure). Receiving on an empty, closed channel returns `none`.

## `watch[T]`

For a value that changes over time with multiple readers, `watch[T]`:

```cinder
let (writer, reader) = watch[config](initial: default_config())

# in one task:
writer.set(new_config)

# in another task:
await reader.changed()    # suspends until the value changes
let current = reader.get()
```

## Implementation status

Partial. Channels and `watch` work on both backends, in `std.sync` (`src/std/sync.cn`); `reader.get()` differs from the text above for a value that is not `copy`.

- `channel[T](capacity)` returns a `(sender[T], receiver[T])` pair. A capacity of 0 is treated as 1. `await sender.send(v)` suspends while the channel is full and returns `result[unit, T]`, giving the value back as `err` if the channel is closed. `await receiver.recv()` suspends while the channel is empty and returns `none` once it is empty and closed. `sender.clone()` makes another sender; the channel closes when `close()` is called, when the last sender is dropped, or when the receiver is dropped. Values still queued when the last handle goes are dropped.
- `watch[T](initial)` returns a `(watch_writer[T], watch_reader[T])` pair. `writer.set(v)` replaces the value and drops the old one. `await reader.changed()` suspends until the value changes after the reader last saw it. Readers can be cloned, and each tracks what it has seen.
- Deviation: `reader.get()` needs `T: copy`. A value that is only `clone` is read with `reader.cloned()`. A single method covering both would need a bound of the form "copy or clone", which the language does not have.
- Handles are `send` and `share` when `T: send` ([Data-Race Freedom](30-data-race-freedom.md#implementation-status)).
- The waits are built on the scheduler's word wait and wake ([The Task Scheduler](../05-implementation/69-task-scheduler.md#waiting-on-a-word)). Corpus file `171` and `std_test/sync_drops.cn` exercise them.

## See also

- [The Execution Model](26-concurrency-execution-model.md) — the task/context model channels move values between.
- [`crew`, `par`, `race`](28-crew-par-race.md) — the structured-concurrency scope channels are typically used within.
- [Data-Race Freedom](30-data-race-freedom.md) — `mutex`/`rwlock`/`atomic`, the other synchronization primitives in `std.sync`.
