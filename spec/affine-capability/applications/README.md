# Complete application examples for A, B, and C

Status: complete source-level design fixtures, not executable compiler conformance tests.
Each listing defines its application types, helpers, entry point, input, and expected result.
The proposed A/B/C semantics and some library/surface forms are not implemented. None of
these listings has been compiled or benchmarked. Expected outcomes are design obligations.
Do not add them to today's compiler tests as if a syntax error verified an ownership rule.

These are bounded deterministic applications so all three options can be compared on the
same workload. They are larger workflows than individual borrow probes, but do not claim
production completeness or demonstrate an entire Python/C++ replacement ecosystem.
The standalone plans are [B](../b-plan.md) and [C](../c-plan.md); A remains specified by
the [affine core](../../affine-capability-core.md) and its surface mapping.

| Application | Input and observable result | A | B | Strengthened C |
|---|---|---|---|---|
| [Report aggregation](report.md) | Samples 2, 4, 6; total 12 plus retained first sample 2; `main` returns 14 | Explicit field helper | Explicit field helper | Method with inferred narrow access |
| [Deferred callbacks](callbacks.md) | Jobs adding 3, 4, 5, dispatched twice; `main` returns 24 | Unique state passed to callbacks | Unique state passed to callbacks | Managed state retained by escaping callbacks; explicit-state version also available |
| [Borrowed registration](registration.md) | Reject at capacity 0; append 8 to source [7]; register at capacity 1; `main` returns stored length 2 | Outcome-specific stored-loan state | Prepare payload, then commit | Inferred outcome-specific stored-loan state |

All successful programs must finish with no dangling reference, double cleanup, or resource
leak other than the documented strong-cycle behavior of the managed candidate. Main returns
are checksums, not conventional process-success codes. Values 100 and above identify an
unexpected branch or managed access conflict in these fixed inputs.

## Shared fixture conventions and library contracts

Only the application code differs between options; do not give A handwritten annotations
where its accepted inference policy supplies the same checked contract. The support below
stands for proposed compiler/standard-library facilities, not missing application helpers.
Imports are implicit for these literate fixtures. A compiler port must replace this support
with actual declarations and record its authoring cost separately.

- Integer arithmetic and casts use the existing spellings. The fixed inputs do not overflow.
- Lists support construction, `.len()`, `.push(value)`, `.as_slice()`, checked indexing,
  shared iteration, and scoped cleanup. Push moves its element, can relocate, and must reject
  conflicting live loans. Indexing includes bounds checks unless a valid witness removes them.
  `as_slice` retains its source loan; copying/moving a slice preserves that loan.
- For A only, `owned(constructor-expression)` is fixture notation for checked direct owner
  construction yielding `own[T]`. It is not a new accepted keyword, a deep copy, or a universal
  heap-boxing rule. Its elaboration must use the real constructors and shared value-storage
  pipeline. The notation makes A's owner/contents distinction visible without inventing
  application-specific allocation helpers. `inspect`/`edit` are A's accepted capability types.
- Bare calls to an inspection/edit parameter borrow or reborrow according to A's surface;
  explicit `&`/`&mut` are shown at important boundaries. Field projection and method access
  do not move stored owners. No function may extract an owner through inspection.
- B/C `fn read(...) -> R` is placeholder syntax for a reusable observing entry, paired with
  the comparison's reusable editing `fn mut`. It is not in today's grammar. A's equivalent
  is `fn(self, ...) -> R`. Such entries cannot move affine environment contents out; effects
  through explicit parameters or managed authority remain in their complete contracts.
- A/B/C capture lists use value capture: affine values move and checked duplicable handles
  duplicate. An empty list captures nothing. Returned closures must retain all dependencies;
  they cannot capture a dead stack borrow.
- C's groups and narrow receiver-access inference are proposed. A/B signatures that request
  a whole receiver retain that requirement even when a body writes only one field.
- C's `managed.new`, `try_read`, `try_edit`, and `borrow_error` have the contracts in the
  [C plan](../c-plan.md#managed-mutable-sharing). A handle passed by value retains the same
  object. The callback cannot return or store an access-dependent loan. A recoverable access
  conflict invokes no callback; access is released after callback cleanup, before returning.
- Lists and managed construction may allocate. Allocation failure is outside the fixed
  successful input trace; the eventual port must specify its language failure policy and
  inject failures to verify partial-construction cleanup. No listing models failed allocation
  as a successfully constructed value. There are no custom finalizers or suspension points.

All application-defined behavior is included in each listing; there are no omitted helper
bodies. Proposed library primitives above are assumptions to implement and validate, not
evidence of safety. In particular, the managed abstraction is an extension beyond A/B's core.

## Fair comparison and required measurements

A/B do not currently define a safe managed mutable wrapper. Their callback versions preserve
the application's numeric behavior by centralizing ownership and passing state. That is an
application API rewrite; it is not equivalent to independently retained mutable object
identity at arbitrary call sites. Record that distinction rather than claiming C dominates
A/B without a runtime tradeoff. If managed sharing is added to A/B, apply the same wrapper
contract and implementation costs to them and repeat the comparison.

| Measure | What to record once the examples are executable |
|---|---|
| Behavior | Main checksum, unexpected/error branches, event order, retained identity, and source independence |
| Programmer work | Declarations/contracts, changed function boundaries, extra phases, explicit state threading, copies, and handle duplication |
| Static precision | Accepted disjoint access, rejected aliases, carried/stored origins, and outcome handling |
| Runtime cost | Necessary backing allocations, retains/releases, runtime access attempts, bounds checks, and cleanup; separate source-visible operations from measured emitted operations |
| Diagnostics | Conflicting operation, original loan/authority, transitive cause, and a specific safe repair |
| Compatibility | Pin an interface; change a helper's reads, writes, stored origins, or call mode; compare against the released baseline |

No annotation, allocation, or performance totals are reported as measurements here. A list
literal does not prove a heap allocation, and an access call in source does not prove its
runtime check survives optimization. Keep the fixed-input checks distinct from adversarial
and failure-injection cases listed with each application and in C01–C10.

## Porting and evidence ledger

| Step | Required evidence | Current status |
|---|---|---|
| Source design | Complete application bodies, support contracts, fixed inputs, expected checksums | Supplied in these documents; proposed semantics |
| Semantic elaboration | A/B/C definitions and exported contracts; verify intended borrow rule rather than unsupported syntax | Pending |
| Execution | VM and LLVM results plus cleanup/lifetime checks with reclamation | Pending |
| Negative cases | Separate rejection fixtures for each adversarial mutation, with matching controls | Pending |
| Failure paths | Allocation/access errors, partial cleanup, reentrancy, and weak/strong-cycle behavior | Pending |
| Ergonomics/performance | Recorded source work and runtime costs using comparable library implementations | Pending |

The next expansion should add an actual parser/file pipeline, a graph/cache with deletion
and generation-sensitive handles, and asynchronous cancellation once its semantics exist.
These initial examples do not settle those workloads or the remaining proof obligations.
