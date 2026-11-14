# Application: fallible registry borrowing caller input

Read all listings with the [fixture support contracts](README.md#shared-fixture-conventions-and-library-contracts).
The registry stores borrowed rows without copying their elements. It rejects a row when
full. The application first tries at capacity zero, appends 8 to the rejected source `[7]`,
raises capacity to one, and retries. Expected `main` result is the stored row's length, 2.
The registry is still live during the mutation after the first rejection and is dropped
before its source. This exercises stored loans, not just loans in the returned payload.

## A: per-outcome stored-loan states

The inferred output summary says `@err` stores no input loan and `@ok` stores `row`'s
loan in `reg.names`. Before discrimination the possible stored loan is protected. The
first error arm can then mutate the source. Explicit per-outcome contract syntax is not
invented here; A's source-body inference must produce the complete checked summary.

```cinder
module applications.a.registration

type registry = { names: own[list[slice[int32]]], limit: usize }

def register(reg: edit[registry], row: slice[int32]) -> result[unit, int32]:
    if reg.names.len() >= reg.limit:
        return @err(1)
    reg.names.push(row)
    return @ok(())

def main() -> int32:
    var source: own[list[int32]] = owned([7])
    var reg: own[registry] = owned(registry { names: owned([]), limit: 0 })
    match register(&mut reg, source.as_slice()):
        @ok(_) => return 100
        @err(_) => source.push(8)
    reg.limit = 1
    match register(&mut reg, source.as_slice()):
        @ok(_) => return reg.names[0].len() as int32
        @err(_) => return 101
```

## B: return the row, then commit

B's union summary for the direct `register` API would retain the possible stored loan
even after `@err`, so mutating `source` while the registry remains live would be rejected.
Instead, split validation from mutation. The candidate loan travels in the success payload;
only a successful match stores it. B already tracks dependencies of live payload values;
this needs no outcome-specific mutation summary. The first failure stores nothing, and
the dead preparation result cannot keep its input loan alive.

No callback or other mutation occurs between preparation and commit. If registration
acquired locks or consulted concurrently changing state, splitting it this way would need
additional invariants and could change behavior. The refactoring is valid for this bounded
sequential registry.

```cinder
module applications.b.registration

type registry = { names: list[slice[int32]], limit: usize }

def prepare(used: usize, limit: usize, row: slice[int32]) -> result[slice[int32], int32]:
    if used >= limit:
        return @err(1)
    return @ok(row)

def main() -> int32:
    var source: list[int32] = [7]
    var reg = registry { names: [], limit: 0 }
    match prepare(reg.names.len(), reg.limit, source.as_slice()):
        @ok(row) =>:
            reg.names.push(row)
            return 100
        @err(_) => source.push(8)
    reg.limit = 1
    match prepare(reg.names.len(), reg.limit, source.as_slice()):
        @ok(row) =>:
            reg.names.push(row)
            return reg.names[0].len() as int32
        @err(_) => return 101
```

## C: inferred outcome state, direct API

C keeps the direct registration API. Its store-flow summary records the source backing
in the registry only on success. This is not inferred rollback: the error path actually
performs no store. There are no groups because no caller needs partial registry access,
and no managed wrapper is necessary for scoped borrowed registration.

```cinder
module applications.c.registration

type registry = { names: list[slice[int32]], limit: usize }

def register(reg: &mut registry, row: slice[int32]) -> result[unit, int32]:
    if reg.names.len() >= reg.limit:
        return @err(1)
    reg.names.push(row)
    return @ok(())

def main() -> int32:
    var source: list[int32] = [7]
    var reg = registry { names: [], limit: 0 }
    match register(&mut reg, source.as_slice()):
        @ok(_) => return 100
        @err(_) => source.push(8)
    reg.limit = 1
    match register(&mut reg, source.as_slice()):
        @ok(_) => return reg.names[0].len() as int32
        @err(_) => return 101
```

## Trace and rejection cases

At capacity zero all versions leave the registry empty. The source grows from length 1
to 2 while no registry value carries its loan. The retry stores a slice into that backing;
the registry's live contents now carry the source loan. The stored slice observes two
elements and reports length 2. Registry cleanup ends the stored loan before source cleanup.
No deep element copy is part of any version. Lists may allocate backing under the shared
failure policy; successful insertion must not silently discard the new stored dependency.

- Push to or destroy `source` after successful insertion and then read the registry:
  reject in every option, including through an indirect call or alias.
- Return a registry containing a slice into a callee-local source: reject at the store/escape
  boundary; moving the registry does not extend the source's lifetime.
- Store the row before returning `@err`: A/C must retain its loan in the error outcome too.
- Save the result without matching it, mutate the source, then inspect the registry:
  reject while success remains possible. Discarding the result does not discard stored loans.
- Match a saved outcome after an intervening permitted operation changes a fact: do not
  restore a pre-call epoch, initialization claim, or capability on that match arm.
- Overwrite the returned result, or join its match arms: retain every still-reachable store
  dependency and cleanup obligation; do not tie them to the result variable's lifetime.
- Change an exported registration body to store on error: detect the incompatible outcome
  contract against the released interface, even if its may-write set is unchanged.

The A/C advantage here is avoiding the prepare/commit API rewrite. It does not imply
automatic inference of arbitrary logical postconditions or a transactional error policy.
