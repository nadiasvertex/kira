# Application: report aggregation with a retained sample

Read all three listings with the [fixture support contracts](README.md#shared-fixture-conventions-and-library-contracts).
Each application retains a view of the first sample, aggregates the whole list into another
field, and then uses the retained view. Input is `[2, 4, 6]`; expected `main` result is 14.
No deep copy or runtime alias check is required by the chosen static representation.

## A: explicit field authority

A's `writes` bound alone would not narrow a whole-receiver edit. This application gives the
helper only the counter's authority. The sample list stays borrowed while counter updates
run; neither the containing report nor sample backing can be replaced during that borrow.

```cinder
module applications.a.report

type report = { items: own[list[int32]], total: int32 }

def add_total(total: edit[int32], value: int32) -> unit:
    *total = *total + value

def main() -> int32:
    var app: own[report] = owned(report {
        items: owned([2, 4, 6]),
        total: 0,
    })
    let first: inspect[int32] = &app.items[0]
    for sample in &app.items:
        add_total(&mut app.total, *sample)
    return app.total + *first
```

## B: explicit field borrowing

B uses the same decomposition without owner-handle types. Passing only `app.total` avoids
the whole-receiver conflict. This requires field paths and the provenance rules, which
the historical compiler probe lacked; it is a proposed B program, not a current acceptance.

```cinder
module applications.b.report

type report = { items: list[int32], total: int32 }

def add_total(total: &mut int32, value: int32) -> unit:
    *total = *total + value

def main() -> int32:
    var app = report { items: [2, 4, 6], total: 0 }
    let first = &app.items[0]
    for sample in &app.items:
        add_total(&mut app.total, *sample)
    return app.total + *first
```

## C: inferred partial receiver access

The inferred contract for `add_total` requires exclusive access only to `self.counters`;
it reads and writes `total`. It does not require even shared access to `self.samples`.
The call acquires that partial access directly, and its body is checked with no authority
to access `items`. Groups make that separation exportable across a module boundary.

```cinder
module applications.c.report

type report = { items: list[int32], total: int32 }
    group samples = { items }
    group counters = { total }

extend report:
    def add_total(mut self, value: int32) -> unit:
        self.total = self.total + value

def main() -> int32:
    var app = report { items: [2, 4, 6], total: 0 }
    let first = &app.items[0]
    for sample in &app.items:
        app.add_total(*sample)
    return app.total + *first
```

## Trace and rejection cases

All listings initialize total to 0, update it to 2, 6, then 12, and add the retained first
sample 2. The return expression reads the sample before cleanup releases its backing.
The helper does not store its input loan, and no list shape change occurs.

- Add a push or backing replacement to C's method, directly or through a helper: reject
  the call while samples are borrowed; show the transitive effect in the diagnostic.
- Hold `&mut app.items[0]` and call a method that reads `items` but only writes `total`:
  reject on its read requirement. Narrow writes alone must not accept this case.
- Move or replace the report while `first` is later used: reject before destroying backing.
- Move a mutable reborrow's parent and access through it while the child is live: reject.
- Export C's method and add a sample read in a later release: detect the strengthened access
  requirement even if the write set is unchanged.

The application measures the difference between field threading and narrow method contracts.
It does not show A accepting a whole-receiver borrow merely because its write set is narrow.
