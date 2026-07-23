# 29. Channels and Sync Primitives

**Status:** Planned

`channel[T]` and `watch[T]`.

## `channel[T]`

Channels pass ownership of values between tasks:

```kira
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

```kira
let (writer, reader) = watch[config](initial: default_config())

# in one task:
writer.set(new_config)

# in another task:
await reader.changed()    # suspends until the value changes
let current = reader.get()
```

## Implementation status

Design only; no implementation of any kind. No `channel` or `watch` type, function, or method exists anywhere in `src/std/`, and neither runtime backend has any channel or synchronization machinery. This chapter depends on [`crew`, `par`, `race`](28-crew-par-race.md) and [The Execution Model](26-concurrency-execution-model.md), neither of which is implemented beyond parsing, so nothing here can be written and compiled today.

## See also

- [The Execution Model](26-concurrency-execution-model.md) — the task/context model channels move values between.
- [`crew`, `par`, `race`](28-crew-par-race.md) — the structured-concurrency scope channels are typically used within.
- [Data-Race Freedom](30-data-race-freedom.md) — `mutex`/`rwlock`/`atomic`, the other unimplemented synchronization primitives.
