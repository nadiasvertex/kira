# Application: deferred callbacks updating one counter

Read all listings with the [fixture support contracts](README.md#shared-fixture-conventions-and-library-contracts).
A queue stores callbacks after factory functions return, then runs the jobs twice.
Job amounts are 3, 4, and 5; expected `main` result is 24. Every handler is reusable.
The examples are sequential; there is no thread transfer, suspension, or concurrent mutation.

## A: retain jobs, pass uniquely owned state

The queue owns its closures. Each closure receives an edit of the single counter when
invoked, instead of retaining a mutable alias to it. The observing callable entry permits
repeated invocation from a shared iteration over the jobs. Its mutable effect is on an
explicit argument, not on its environment.

```cinder
module applications.a.callbacks

type counter = { total: int32 }
type job = {
    amount: int32,
    handler: fn(self, state: edit[counter], amount: int32) -> unit,
}

def add(state: edit[counter], amount: int32) -> unit:
    state.total = state.total + amount

def make_job(amount: int32) -> job:
    return job {
        amount: amount,
        handler: [] (state: edit[counter], n: int32) => add(state, n),
    }

def dispatch(state: edit[counter], jobs: inspect[list[job]]) -> unit:
    for queued in jobs:
        queued.handler(state, queued.amount)

def main() -> int32:
    var state: own[counter] = owned(counter { total: 0 })
    let jobs: own[list[job]] = owned([make_job(3), make_job(4), make_job(5)])
    dispatch(&mut state, &jobs)
    dispatch(&mut state, &jobs)
    return state.total
```

## B: explicit state in the callback interface

As in A, each call reborrows the state and releases that call's loan before the next job.
The handler does not store its argument or return a loan. `fn read` is the proposed
observing-callable spelling defined in the support contracts, not today's grammar.

```cinder
module applications.b.callbacks

type counter = { total: int32 }
type job = {
    amount: int32,
    handler: fn read(state: &mut counter, amount: int32) -> unit,
}

def add(state: &mut counter, amount: int32) -> unit:
    state.total = state.total + amount

def make_job(amount: int32) -> job:
    return job {
        amount: amount,
        handler: [] (state: &mut counter, n: int32) => add(state, n),
    }

def dispatch(state: &mut counter, jobs: &list[job]) -> unit:
    for queued in jobs:
        queued.handler(state, queued.amount)

def main() -> int32:
    var state = counter { total: 0 }
    let jobs = [make_job(3), make_job(4), make_job(5)]
    dispatch(&mut state, &jobs)
    dispatch(&mut state, &jobs)
    return state.total
```

## C: callbacks retain managed state

The factory captures a retained handle by value. All jobs refer to the same counter;
neither the factories' stack frames nor the dispatcher own its lifetime. Dispatch takes
only jobs. Access conflicts are ordinary errors, propagated without invoking the conflicting
callback. The observing environment entry can exercise managed authority whose runtime
access reservation supplies exclusivity; that effect belongs in the summary.

```cinder
module applications.c.callbacks

type counter = { total: int32 }
type job = {
    amount: int32,
    handler: fn read(amount: int32) -> result[unit, borrow_error],
}

def add(value: &mut counter, amount: int32) -> unit:
    value.total = value.total + amount

def make_job(state: managed[counter], amount: int32) -> job:
    let handler = [state] (n: int32) =>:
        return state.try_edit((value: &mut counter) => add(value, n))
    return job { amount: amount, handler: handler }

def make_jobs(state: managed[counter]) -> list[job]:
    return [make_job(state, 3), make_job(state, 4), make_job(state, 5)]

def dispatch(jobs: &list[job]) -> result[unit, borrow_error]:
    for queued in jobs:
        match queued.handler(queued.amount):
            @ok(_) => ()
            @err(error) => return @err(error)
    return @ok(())

def main() -> int32:
    let state = managed.new(counter { total: 0 })
    let jobs = make_jobs(state)
    match dispatch(&jobs):
        @ok(_) => ()
        @err(_) => return 100
    match dispatch(&jobs):
        @ok(_) => ()
        @err(_) => return 101
    match state.try_read((value: &counter) => value.total):
        @ok(total) => return total
        @err(_) => return 102
```

C also admits the complete B listing as its more explicit representation (with a C module
name). That path has no managed retention/access operations and keeps state in the callback
interface. It does not require group annotations for this workload. Moving from managed C
to that representation changes the callback API; it is an explicit, reviewable optimization.

## Trace, lifetime, and failure cases

Each pass updates total by 3, 4, and 5: 0 → 3 → 7 → 12 → 15 → 19 → 24.
A/B loan the same unique state once per callback. C requests six exclusive accesses and
one final read in source; exact retained operations and emitted checks must be measured.
Factory frames end before dispatch. C's jobs retain their counter, and callbacks capture
no access-dependent loan. The reference graph has no strong cycle. Releasing all jobs
and the last main handle permits final counter cleanup.

On C's dispatch error, preceding successful callbacks stay committed; dispatch is not
transactional. The main error branch cleans up its queue and handle normally. No conflict
is expected under the fixed sequential inputs, so all 100+ outcomes are failures of the
expected trace. Allocation failure requires separate policy and injection fixtures.

- Replace C's retained capture with a borrow of factory-local state: reject escape.
- Retain a scalar/reference/view obtained in `try_read` or `try_edit` beyond its callback:
  reject even though a managed handle keeps the object alive.
- Reenter a handler for the same object during an active edit: return a conflict before
  its inner callback runs. Outer cleanup must still release its reservation exactly once.
- Replace the counter with a managed list and keep an element view across an edit/push:
  reject view escape; object retention does not prevent backing relocation.
- Attempt to send this managed handle across threads or suspend with active access:
  reject under the initial managed contract.
- Add an object-to-handler strong back edge: document retained-cycle behavior. Make it a
  weak edge and verify failed upgrades after final release. No arbitrary cycle collection
  is claimed by this example.

This comparison exposes both benefits and costs: C supports independent retained callbacks
without state threading, while its managed path introduces runtime access and retention.
A/B can obtain the same API only through a separately specified managed extension, or use
the explicit-state application shown here. Numeric equivalence alone does not erase that
API difference.
