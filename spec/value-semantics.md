# Cinder Value Semantics: Implementation Plan (v1.0.0 – 2026-11-09)

## Summary

Make ordinary Cinder values live directly in their owner's storage. Represent
structs, tuples, fixed arrays, sum types, and view descriptors inline, and construct
aggregate results directly into caller-provided storage. Make return value
optimization (RVO) part of ordinary lowering and named return value optimization
(NRVO) a shared, enabled-by-default compiler pass. Remove the unowned slice-header,
tuple, and sum allocations described in [todo items 17 and 18](todo.md), while
preserving ownership, borrowing, evaluation order, and exactly-once drops on the
bytecode VM and LLVM backends.

This is an implementation plan. T-01 is complete (findings in
[value-semantics-t01-findings.md](value-semantics-t01-findings.md)), T-02 is
complete, T-03 and T-04 are complete for straight-line code, T-04a adds `if` and
`while` to the storage form, and T-05 and T-06 are partly done (storage-form
bytecode and LLVM compilers exist but are not wired into the driver), and T-07
is partly done (runtime intrinsics write results into caller storage), and T-08
is partly done (raw memory holds aggregates inline in the storage form), and T-09
is partly done (closures, patterns, generators, `await`, `await par`/`await race`, `on` blocks); status is under each task below. T-10 through T-14
have not been implemented.

## Objectives & Scope

In scope:

- Store fixed-size values inline in locals, parameters, return destinations,
  aggregate fields, collection elements, globals, closure environments, and
  coroutine frames.
- Separate a value's layout, its ownership rules, its storage lifetime, and its
  calling convention. Passing an address internally must not imply heap allocation
  or source-language borrowing.
- Implement destination-directed construction, aggregate moves and copies,
  mandatory RVO, conservative NRVO, and bounded frame storage on both backends.
- Migrate runtime intrinsics, compile-time VM conversion, generated drop glue,
  generators, tasks, `par`, and `race` to the same representation.
- Prove correctness with ownership-checked execution, allocation counts, drop
  counts, structural lowering checks, and backend parity.

Out of scope:

- Change which source types satisfy `copy`, add implicit `clone`, add user-defined
  move constructors, or introduce a consuming-method-receiver syntax.
- Eliminate the backing allocations required by growable collections, shared
  ownership, escaping closure environments, or suspended tasks.
- Implement garbage collection, tiered JIT promotion, general escape analysis,
  pinning, sum niche optimization, or platform-specific aggregate register ABIs.
- Add panic unwinding. Preserve current panic behavior; do not interpret this
  migration as implementing the specification's unwinding aspiration.

## Assumptions & Open Questions

### Decisions for this plan

1. **Preserve the language's ownership rules.** Chapter 14 makes structs, tuples,
   fixed arrays, and resource handles move by default. Making their representation
   inline does not make them `copy`. Preserve the existing structural `copy` rule
   for sums, the exclusive nature of mutable views, and explicit `clone` behavior.
2. **Use a uniform internal aggregate ABI first.** Return aggregates through a
   hidden result destination. Pass by-value aggregates through storage that belongs
   to the callee's parameter for the duration of the call. Preserve scalar and
   reference passing where already appropriate. Optimize machine register passing
   later without changing source semantics.
3. **Require finite inline size.** Detect a cycle through inline fields, tuple
   elements, fixed-array elements, or sum payloads and report an infinite-size type.
   Pointers, references, and the backing pointers of `list`, `shared`, closures,
   and tasks break such cycles. Do not silently box recursive fields. Inventory
   existing recursive types before enabling this diagnostic and document any
   necessary source migration through an existing owning indirection.
4. **Retain declared receiver semantics.** Stop deriving receiver borrowing from
   whether a type happens to be heap represented. Preserve the current receiver
   contract while replacing that implementation test with explicit passing modes.
   Continue rejecting moves through borrowed `self`, including generic instances;
   a new consuming-receiver design is separate work from todo item 16.
5. **Treat RVO as construction, NRVO as storage reuse.** Neither invokes `clone`,
   introduces a new observable move operation, nor changes destructor ordering.
   Ordinary moves remain a correct fallback when NRVO is ineligible.
6. **Keep one representation at each boundary.** Version internal metadata when
   necessary and rebuild generated artifacts. Do not retain an unversioned mixture
   of old pointer-to-block values and new inline values.

### Questions to resolve during T-01

These are repository inventories, not requests for user approval. Record findings
and implement the decisions above before representation changes begin.

- Which recursive type declarations currently rely on implicit pointer-sized
  aggregate fields? List affected standard-library types and fixtures, and identify
  the owning indirection each needs, if any.
- Which opaque builtins have a fixed concrete representation, and which are
  genuine handles? Record a classification for every supported builtin; fail
  closed for unsupported types rather than falling back to an eight-byte pointer.
- Which string routines own generated bytes, and which return views of static or
  shared bytes? Preserve their established backing-byte lifetime while moving
  descriptors inline. Record any independent backing-byte leaks separately; do
  not claim this plan solves them by moving the descriptor.
- Which runtime/JIT/host entry points and test helpers accept or return an
  aggregate pointer today? Include I/O and platform results, string routines,
  scheduler adapters, static initialization, and compile-time host callbacks.
- Which corpus programs bypass the ownership checker or intentionally use obsolete
  aliasing behavior? Identify replacements using valid source ownership before
  using them as evidence for the migration.

The overview paths in the repository's agent instructions include older merged
documents that are absent in this checkout. Use the existing chapter tree under
`spec/specification/`, `spec/cinder-grammar.ebnf`, and `spec/CONVENTIONS.md`.

## Requirements

### Functional Requirements

- **FR-1:** Give every supported concrete type one checked size, alignment, and
  projection layout shared by both backends, runtime boundaries, and compile-time
  execution. Resolve aliases, refinements, and generic substitutions before layout.
- **FR-2:** Allocate no standalone heap block for a slice/view header, tuple, fixed
  array, struct, or sum merely because it is constructed, passed, returned, copied,
  or moved. A value nested inside a heap owner occupies bytes in that owner's
  allocation.
- **FR-3:** Preserve `copy`, move, borrow, implicit autoref, mutable access, partial
  moves, assignment, and drop behavior. A moved-from place owns nothing transferred
  from it; a copy owns no resource also owned by the source.
- **FR-4:** Construct eligible temporary aggregate results directly in their final
  destination, across calls and nested aggregate expressions, in debug and release
  builds and with HIR inlining disabled.
- **FR-5:** Enable NRVO by default for the eligibility rules below on both backends.
  Make rejected candidates use correct inline moves without heap fallback.
- **FR-6:** Never return an address of a dead activation, retain a pointer into a
  destroyed VM frame, or store a live-across-suspension value in a transient poll
  frame. Preserve rejection of views that outlive their referents.
- **FR-7:** Keep aggregate arguments and results correct through direct, indirect,
  generic, cross-module, closure, intrinsic, JIT, AOT, and compile-time calls.
- **FR-8:** Evaluate each source operand once in source order. Compute a replacement
  before dropping the old value. Preserve all early-exit and cancellation drops.
- **FR-9:** Remove the tuple/sum leak allowances in affected stress fixtures only
  after their replacement bounds pass on both backends.

### Non-Functional Requirements

- **NFR-1 (Performance):** Require zero aggregate-shell allocations for pure value
  loops and returns. Require zero intermediate aggregate transfers for direct RVO
  and eligible NRVO examples, using structural checks in addition to allocation
  counters. Keep required backing allocations bounded by actual owners.
- **NFR-2 (Safety):** Check layout arithmetic, array multiplication, projection
  offsets, bytecode encoding widths, and total frame sizes. Respect alignment,
  including packed accesses and 128-bit members. Preserve thread handoff ownership.
- **NFR-3 (Maintainability):** Put layout, ABI classification, destination planning,
  and NRVO decisions in shared code. Backends must implement the plan, not infer
  ownership or independently choose which values escape.
- **NFR-4 (Observability):** Extend internal dumps/test inspection to show storage
  class, result destinations, aggregate transfers, NRVO decisions, and frame bytes.
  Keep existing heap allocation/live-block counters meaningful.
- **NFR-5 (Compatibility):** Update raw-memory operations and standard-library
  layouts together. Do not promise binary compatibility with old generated code or
  accidentally expose the internal aggregate ABI as the platform C ABI.
- **NFR-6 (Privacy, accessibility, i18n):** No new user data collection or interface
  is introduced. Preserve existing string contents, diagnostics conventions, and
  Unicode behavior; no independent work is required in these areas.

## Architecture & Design Overview

### Current implementation and change points

Use these inspected definitions as starting points. Line spans describe the
checkout used to write this plan and will move during implementation.

| Concern | Current implementation | Consequence |
|---|---|---|
| Slice construction | `src/bytecode_compiler/compile.cpp:2873–2999`; `src/llvm_codegen/codegen.cpp:2948–3024` (`compile_range_index`) | Allocate a two-word header and return its address. |
| Sum construction | `src/bytecode_compiler/compile.cpp:3228–3270` (`compile_variant_init`) | Allocate tag plus uniform payload words. |
| Value classification | `src/llvm_codegen/codegen.cpp:314–361` (`storage_llvm_type`); `src/bytecode_compiler/compile.cpp:1618–1637` (`is_heap_pointer_value`) | Treat aggregates and descriptors as pointers. |
| Nested layout | `src/runtime/layout.cpp:249–354`, `397–419`, `479–532`, `541–610` | Nested aggregates normally cost one pointer; sums use word slots rather than recursive byte layouts. |
| Source ownership and drops | `src/hir/drop_schedule.cpp:189–303`; `src/hir/lower.cpp:3294–3439`; `src/hir/reference_check.cpp` | Track logical ownership and synthesize drops; preserve these facts while changing storage. |
| Existing frame storage | `src/bytecode_compiler/compile.cpp:2303–2323`; `src/llvm_codegen/codegen.cpp:3135–3148`; `src/hir/frame_budget.cpp:30–52` | Provide fixed frame bytes for `uninit`, but do not budget ordinary inline values yet. |
| VM call frames | `src/bytecode/vm.cpp:1093–1114`; `src/bytecode/vm.h:21–24` | Copy register arguments; expose one `slot_value` result. |
| Runtime results | `src/runtime/io.cpp:36–41`; `src/bytecode/vm.cpp:1235–1243`; `src/intrinsics.h:60–65` | Runtime code also constructs allocated result wrappers. |
| Compile-time conversion | `src/comptime_vm/values.cpp:143–190`, `451–473`; `src/comptime_vm/backend.cpp` | Marshal aggregates through pointer-valued slots. |
| Suspension state | `src/runtime/coroutine.h`; `src/hir/live_across_yield.cpp`; backend coroutine code | Save register images and use a fixed two-word yielded-value area. |
| Tail calls and inlining | `src/hir/tail_calls.cpp:460–487`; `src/hir/inline.cpp:950–994` | Must be revisited when callees receive addresses into caller storage. |
| Regression measurement | `src/codegen_stress_test.cpp`; `src/testdata/codegen_stress/177_coroutine_borrow_storage.cn`, `178_coroutine_objects_freed.cn`, `179_par_race_tasks_freed.cn` | Already support independent expected results and heap bounds. |

Read the ownership, drop, views, machine-layer, HIR/VM, LLVM/AOT, and compile-time
chapters before editing their corresponding implementation. Chapter 68 describes
tiered execution as future work; do not add a nonexistent VM-to-native dispatch
bridge to this migration.

### One layout model

Replace pointer-sized aggregate classification with a cached layout descriptor
keyed by concrete type and target layout. Store resolved concrete member types in
semantic results; remove layout's shortcut of interpreting only the outer AST
type-expression shape.

| Type family | Representation owned by a place |
|---|---|
| Scalar | Existing scalar bytes and alignment. |
| `&T`, `&mut T`, raw pointer | One address of the actual referent storage. |
| Struct / tuple | Inline member layouts with explicit offsets and padding. |
| Fixed array | Contiguous inline elements; stride is the checked element layout size. |
| Sum, `option`, `result` | Tag plus aligned storage large enough for the largest variant; only the active payload is initialized. |
| `slice`, `slice_mut`, `str` | Inline `{len, data}` descriptor; data lifetime remains independently governed. |
| `cell`, `cell_mut` | Inline address descriptor; preserve their current view semantics. |
| Standard-library collection / `shared` handle | Inline concrete struct fields; the owned backing allocation remains separate. |
| Function value / closure | Inline fixed descriptor containing entry/environment information; an escaping environment may own a backing block. |
| Generator / task | Inline fixed handle to runtime-owned, address-stable suspension state. |
| Opaque type / trait object | Its explicitly registered concrete layout or fixed handle ABI; never an implicit default pointer. |

Initially retain a `uint64` sum tag with declaration-order tag values. Lay out each
variant's payload like a tuple. Align the payload area to the maximum variant
alignment, then round total size to the maximum of tag and payload alignment.
Provide byte offsets for every payload projection. Defer compact tags and niches.

Keep existing scalar sizes, including zero-sized `unit` and 16-byte alignment for
128-bit integers. Specify zero-sized arrays/aggregates explicitly: they need no
payload allocation, but taking a reference still requires a valid aligned address
for the relevant lifetime. Avoid null-pointer arithmetic and zero-stride division.
Do not use padding or inactive payload bytes for equality, hashing, or drop logic.
Keep compiler-created padding deterministic where bytes cross runtime boundaries.

Update allocators, frame arenas, closure/coroutine state, globals, and collection
storage to honor the requested alignment. The current heap contract only promises
eight-byte alignment, while `layout_of(int128)` reports sixteen; do not assert
sixteen-byte LLVM alignment until the actual owner provides it. Packed projections
require unaligned accesses or byte copies. Preserve borrowing of packed fields by
using conservative alignment for indirect reference accesses, including across
calls; strengthen alignment only when provenance proves it. Do not infer referent
alignment solely from the referenced type.

### Shared storage lowering

Introduce a shared storage form under `src/hir/`, produced from typed HIR after
source ownership validation and reference elaboration. Keep it small: typed places,
explicit aggregate initialization/transfers, call destinations, control flow, and
the existing logical drop/loan information. Do not build a second source type
checker or duplicate backend-specific ownership analysis.

The required concepts below are proposed interfaces, not existing APIs:

```text
place = base storage + concrete type + checked projections
storage = local frame | parameter | caller result | global |
          owner field | closure environment | coroutine frame

initialize_into(destination, expression)
copy_into(uninitialized_destination, copy_source)
move_into(uninitialized_destination, owned_source)
drop_in_place(initialized_place, existing_drop_plan)
call_into(uninitialized_result, callable, prepared_arguments)
```

Track initialization and logical ownership separately from physical bytes. A byte
copy used to implement a move transfers ownership and invalidates the source in
the ownership state; it does not create a second owner. `copy_into` requires an
existing `copy` classification. Neither operation invokes user `clone` implicitly.
Use projection-level state for partial moves and partially constructed values.

Make constructor fields, tuple elements, array elements, and sum payloads accept
their own destination projection. A nested aggregate is initialized in its
containing bytes instead of allocating a child block. Publish a sum's initialized
state only when its active payload is ready. Preserve source evaluation order and
clean up already initialized parts on supported early-exit/cancellation paths.

Run shared storage lowering for normal compilation, compile-time execution, test
helpers that compile HIR directly, and synthesized functions. Proposed ordering:

1. Check source types, moves, loans, and drop obligations; lower to typed HIR with
   explicit references and logical cleanup.
2. Run existing HIR transformations, including optional inlining, then recompute
   capture and suspension facts invalidated by those transformations.
3. Build concrete layouts and shared storage form; propagate construction and call
   destinations. Place suspension-live values in their stable owner.
4. Apply NRVO and safe storage reuse; preserve logical initialization/drop facts.
5. Assign aligned frame offsets and enforce final frame budgets. Revalidate tail
   calls using storage provenance, then verify the resulting storage form.
6. Emit bytecode or LLVM from the same verified decisions.

### Calls and mandatory RVO

For an aggregate `T`, use this conceptual internal signature:

```text
source:       make(arguments...) -> T
internal:     make(result_address: uninitialized T*, arguments...) -> unit
```

The result destination belongs to the caller. Callee construction temporarily owns
its initialized subobjects; successful return transfers that ownership to the
caller. A discarded result still gets valid temporary storage and its normal
statement-end drop. A top-level VM/JIT/compile-time caller supplies result storage
that survives the callee and remains owned until decoding and cleanup finish.

Pass aggregate arguments using private parameter storage with explicit copy/move
semantics. Caller-allocated outgoing slots may physically back these parameters,
but the callee owns their values during the call and performs the parameter drops.
Materialize a `copy` argument separately if mutating the parameter could otherwise
change the caller. Reuse a moved argument's storage only after proving source
unobservability, lifetime, and alias safety. Do not mistake this transport for `&T`.

Propagate result destinations through:

- `return constructor`, tuple/array literals, and slice construction;
- `return other_call(...)`, forwarding the same result address;
- tail expressions and aggregate-producing blocks;
- `if` and `match` branches and their join destination;
- `?`, `or else`, generic instantiations, indirect calls, and nested aggregate
  fields/elements where the result types and destination projections match.

Do this in common lowering with optimization and inlining disabled. LLVM may then
promote memory to registers or remove further copies; correctness and basic RVO
must not depend on that cleanup.

Use a fresh, uninitialized destination. For `x = make(&x)`, compute the new value
in separate storage, then drop the old `x`, then move the replacement into `x`.
Apply the same rule to fields, elements, `swap`, `replace`, and possible aliases.
Forwarding into an enclosing partially initialized object also requires proof that
no argument or later operand can observe that object's unfinished bytes. Attach
LLVM `sret`, alignment, and alias attributes only when their exact contracts hold.

### NRVO eligibility and fallback

Recognize a named local whose storage can become the function's aggregate result
storage from the local's initialization onward. For example:

```cinder
type pair = { a: int32, b: int32 }

def make_pair(n: int32) -> pair:
    var result = pair { a: n, b: 0 }
    result.b = n + 1
    return result
```

For the initial implementation, require all of the following:

1. The candidate is one complete local with exactly the function's result layout;
   it is not a borrowed value, parameter, global, subobject, or coroutine step local.
2. Initialization dominates every use and every reachable normal return. Every
   reachable normal return, including an implicit tail expression and a lowered
   `?` exit, returns that same complete local.
3. The local is not partly moved, captured, yielded, or moved into another owner.
   Whole-value reassignment may use the ordinary replacement protocol.
4. Its address cannot be observed or retained in a way that distinguishes separate
   local/result storage. Initially reject explicit address-taking, stored borrows,
   raw-pointer exposure, and self-referential constructions. Permit a temporary
   receiver/autoref borrow only when existing analysis proves it nonescaping and
   unable to observe storage identity; otherwise reject the optimization.
5. No caller argument, live loan, or output projection aliases the chosen result
   storage, and cleanup can retain the original logical ownership state.

Bind eligible local projections directly to result projections. Keep the local's
logical lifetime and drop obligations until successful ownership transfer. Returning
it must not run a callee drop on the returned object. Any supported exit before
transfer still destroys what the callee owns, exactly once.

Do not merge several simultaneously live return candidates into one slot. Do not
rewrite returned references to locals into valid borrows. Rejecting NRVO must be
semantically invisible: move the completed local into result storage, clear its
ownership flags, and drop the remaining locals in the specified order. Keep
candidate rejection reasons inspectable and testable. More permissive path-sensitive
NRVO can follow after these rules are stable.

### Drops, views, and storage lifetime

Retain `compute_drop_schedule` and the ownership CFG as the source of logical
cleanup facts. Translate existing synthesized drops to the new places; make every
copy/move/return destination agree with the existing flag transitions. Add a
verifier for transformations that redirect storage instead of recomputing source
ownership from pointer-valued expressions.

Dropping an inline aggregate runs its own drop and then its live members in the
specified order; it does not free the address of the aggregate. Dropping an owning
handle may free its backing allocation. Reclaim frame bytes by frame lifetime,
closure fields with their environment, and coroutine fields with their runtime
owner. Keep raw `drop_in_place` distinct from allocation deallocation.

A view's descriptor and referent have separate lifetimes. Returning a descriptor
copies or moves its bytes into caller storage but still cannot extend the lifetime
of local elements it references. Loan-bearing aggregates continue to carry loans
when moved into fields, collections, closures, tasks, or results. Shared views may
copy; mutable views retain exclusive ownership and must not gain live aliases.

Preserve method receiver borrowing, including generic bodies and automatic deref,
using declared/checked passing modes. Change `&aggregate` to address the aggregate's
actual inline bytes, and remove any old double indirection through a slot holding
an owning aggregate pointer.

### Backends and runtime boundaries

**VM:** Keep scalar registers as `slot_value`. Use registers holding addresses to
typed aggregate places; those addresses are transport, not owners. Allocate static
aligned frame byte ranges for local values and call slots. Extend call metadata and
verification for result addresses and aggregate parameters. Keep addresses stable
while the frame-vector grows, nested calls run, or another VM execution polls a
task. Never copy an aggregate by copying its transport pointer. Do not dynamically
allocate fresh frame bytes on each loop iteration.

The VM stack is its explicit activation storage, not necessarily the host C++
thread stack. Host allocations that back an activation are permitted; allocating
a separate Cinder heap object for each tuple, option, or slice descriptor is not.

**LLVM:** Use entry-block allocas or caller/owner projections for aggregate places,
typed scalar loads/stores, and checked byte transfers for aggregate copies/moves.
Use shared layout offsets even for packed and nested values. Generate internal
aggregate signatures consistently for definitions, function descriptors, indirect
calls, and wrappers. Let LLVM optimize the resulting storage; avoid a second NRVO
implementation in this backend.

**Runtime:** Extend the intrinsic wire contract to express aggregate destinations
and descriptor arguments. Update `src/intrinsics.h`, VM intrinsic implementations,
LLVM declarations, and runtime C functions together. Prefer a C ABI with explicit
output pointers and explicit descriptor fields/addresses. Runtime I/O must write
its result into supplied storage instead of allocating a wrapper per operation.
Include string, formatting, platform, reactor, and scheduler adapters. Temporary
adapters must have explicit ownership and cleanup; remove them before completion.

**Compile time and entry points:** Marshal values into typed storage owned by a
hosted-run context and decode before releasing it. Support nested aggregates of
arbitrary checked size rather than copying at most one 16-byte `slot_value` image.
Supply result destinations through `vm_result`/host APIs and JIT wrappers, preserving
clear ownership. Update static reification, constant pools, metadata, AOT startup,
and tests that cast JIT entry addresses to C++ function signatures.

### Collections, closures, generators, and tasks

Use the new element layout for `size_of`, `align_of`, raw pointer indexing,
`uninit`, list allocation/growth, small-list inline capacity, and drop ranges.
Audit hard-coded eight/sixteen-byte strides and narrow byte offsets. Move nested
values into their container's bytes; never store an address of a transient value
where a collection element is expected. Preserve raw-read/raw-write ownership
rules from chapter 38, including `swap`, `replace`, and `drop_in_place`.

Compute closure environments and coroutine frames as typed layouts. Capture owned
aggregates inline; keep borrowed captures as references to their real owners. An
escaping environment may still allocate once, but it must not allocate a separate
block per captured tuple or sum. A plain function value needs no heap wrapper.

Give each suspension-live aggregate an aligned, stable place in its coroutine
frame, including awaited arguments, borrowed temporaries, match subjects, and
branch-result locals. Preserve referent addresses across polls; do not save an
aggregate by saving a pointer to a poll-frame alloca. Recompute borrowed-local and
suspension liveness after any transformation that creates or redirects places.

Replace the fixed two-word yielded/result image with typed result storage owned
by the coroutine, preferably an aligned trailing area in its existing allocation.
Record the result layout, initialization state, and any drop entry point required
by the runtime. A yield transfers the item into that storage; `next()` constructs
its `option[T]` in the caller's destination and transfers the item out. Specify
whether completed task results copy, move once, or remain runtime-owned according
to the existing task API; reject a second non-copy extraction and never duplicate
ownership. Cancellation and final release drop initialized, unconsumed results.

Keep scheduler-visible state addresses stable until all workers and waiters release
them. Update coroutine/runtime headers, VM polling, native step functions, result
extraction, and cancellation together. `par`/`race` task/result tuples use inline
destinations. Give winners and losers explicit result ownership, including a loser
that finishes during cancellation. Preserve crew-scoped loan and thread-safety rules.

### Frame limits and tail calls

Extend the existing per-frame budget to include ordinary aggregates, alignment
padding, outgoing arguments, temporary result storage, and `uninit` buffers after
RVO/NRVO and safe lifetime reuse. Keep the current 1 MiB default unless separately
changed. Reject oversized frames with contributing source locations; do not
silently heap allocate oversized values. Account for final inlining decisions.

Reuse frame ranges only when their complete lifetimes, including loans and pending
drops, do not overlap. Reusing one bounded range per loop site is required; an
unbounded dynamic stack allocation per iteration is not acceptable.

Forwarding an external result destination permits RVO but does not by itself make
a call tail-call-safe. Disallow frame reuse or `musttail` while a callee can reach
caller-frame arguments, locals, live borrows, or pending cleanup. Preserve safe
scalar tail recursion; re-enable aggregate tail calls only when the shared plan
proves lifetime safety and the backend ABI permits them.

## Task Grid

Owner `Compiler implementer` denotes the implementer of this plan, not an assigned
person. Status: `[x]` complete, `[~]` partly done (the task's details section lists what
is done and what is not), `[ ]` not started. H = required for correctness/completion;
M = required integration and documentation.

Progress as of 2026-11-12: T-01 to T-04a are complete. T-05 to T-09 are partly done on
both storage-form backends (bytecode VM and LLVM), which now compile a program of
several modules with calls between them. T-09 covers closures, patterns (with guards),
generators, tasks and `await`, `par`/`race`/`on`, droppable task results, `crew`
blocks, and sums that own drops (pattern tests and drops over such sums, which also
lets a generator hand out droppable items). T-10 is partly done and T-11 (NRVO) is done and T-12 (frame budgets) is partly done (see below). T-13 and T-14 are not started.

The driver still uses the pointer-per-aggregate form. `src/storage_corpus_test.cpp`
measures how ready the codegen stress corpus is for the switch. It runs each program
through the storage form on the bytecode VM and compares `main` with `# expect:`, or
with the legacy VM when a file has no expected value. A file whose answer depends on
the representation (`size_of` of a struct) states the inline-layout answer with
`# storage-expect:`, which replaces `# expect:` when the driver switches. The programs
listed in `src/testdata/storage_corpus/baseline.txt` must keep passing. On 2026-11-13,
after scalar tail calls, `cell[T]` dereference in both backends, destructuring `let`
subjects read in place, tuple and array patterns on fixed arrays, and values more
than 64 KiB into one slot, 183 of 189 programs passed. Corpus 102 kept its purpose
but now declares 90000 rather than 100000 `int64`s in `filled`, because its two fill
arrays are inline frame storage and the original pair needed about 1.08 MB, over the
1 MiB frame limit. The remaining failures: generic lambda temporaries and drops (129,
130), iteration over `str` (076; needs runtime decode intrinsics on the descriptor),
`crew` errors (158, 173), and a closure that
pushes onto a captured `list` (071). The last is a language gap rather than a
storage one: implicit capture is always a bitwise copy, while chapter 16 says a
closure that does not escape captures by borrow. The legacy form shares the list's
heap header through the copy, so the push shows; the storage form pushes onto the
closure's own copy, and the two copies share a buffer that a reallocation frees
(`spec/todo.md`). The
driver switches when the whole corpus passes, because decision 6 rules out a
program that mixes the two forms.

| Status | ID | Task | Owner | Priority | Depends On | Acceptance Criteria |
|---|---|---|---|---|---|---|
| [x] | T-01 | Inventory representations and capture baselines | Compiler implementer | H | — | Every boundary and opaque/recursive type classified; baseline evidence recorded. |
| [x] | T-02 | Implement concrete recursive byte layouts | Compiler implementer | H | T-01 | Nested layouts, sum offsets, cycle diagnostics, and alignment tests pass. |
| [x] | T-03 | Add shared storage form and aggregate ABI | Compiler implementer | H | T-02 | Verified destination/transfer plans cover calls and constructions. |
| [x] | T-04 | Integrate ownership, references, and drops | Compiler implementer | H | T-03 | Moves, borrows, partial initialization, and drops agree with places. |
| [x] | T-04a | Add structured control flow to the storage form | Compiler implementer | H | T-04 | `if`, `while`, `break`, `continue`, and early `return` lower and verify with joined initialization and drop state. |
| [~] | T-05 | Implement VM frame values and result destinations | Compiler implementer | H | T-04a | VM executes nested aggregate calls without shell allocations. |
| [~] | T-06 | Implement LLVM values and result destinations | Compiler implementer | H | T-04a | LLVM/JIT/AOT use the shared ABI and pass the same value cases. |
| [~] | T-07 | Migrate runtime intrinsic boundaries | Compiler implementer | H | T-05, T-06 | Runtime operations write aggregate results into owned destinations. |
| [~] | T-08 | Migrate containers, statics, and compile-time execution | Compiler implementer | H | T-07 | Nested elements, raw memory, globals, and hosted calls use identical layouts. |
| [~] | T-09 | Migrate captures, generators, and task results | Compiler implementer | H | T-08 | Suspension/cancellation and large results preserve ownership and stable addresses. |
| [~] | T-10 | Complete mandatory RVO propagation | Compiler implementer | H | T-09 | All listed expression forms construct directly with inlining disabled. |
| [x] | T-11 | Implement shared NRVO | Compiler implementer | H | T-10 | Eligible locals use result storage; rejected cases use correct inline moves. |
| [~] | T-12 | Enforce final frame budgets and tail-call safety | Compiler implementer | H | T-11 | Final storage plans are bounded, aligned, and lifetime-safe. |
| [ ] | T-13 | Prove leak removal and backend parity | Compiler implementer | H | T-12 | Ownership-checked regressions and structural/allocation gates pass on both tiers. |
| [ ] | T-14 | Remove legacy representation and update references | Compiler implementer | M | T-13 | Old aggregate boxing paths gone; todo/specs describe verified behavior. |

## Task Details

### T-01 — Inventory representations and capture baselines

**Goal:** Establish the complete migration boundary before changing layouts.

1. Use `graft ask`, `graft callers`, and exhaustive `graft grep` to inventory the
   change points above and all users of heap-value classification, `layout_of`,
   sum slot helpers, fixed register images, intrinsic signatures, and frame storage.
2. Record the opaque-type, recursive-type, string-lifetime, and host-boundary
   findings from Assumptions & Open Questions in this document.
3. Run the current relevant tests and record existing failures separately. Record
   allocation/live-block counts for slices, tuple/option loops, `read_exact`,
   `read_to_end`, generator `next`, and corpus 177–179.
4. Prepare valid ownership-checked fixtures and independent expected values. Add
   fixtures with passing behavioral baselines first; tighten allocation bounds in
   T-13 when the implementation supports them. Do not commit intentionally failing
   allocation expectations as a permanent intermediate state.

### T-02 — Implement concrete recursive byte layouts

**Goal:** Make inline layout correct before either backend consumes it.

1. Expose concrete substituted struct fields and sum payload types through semantic
   results/type metadata; resolve qualified types, aliases, and const-generic arrays.
2. Extend `src/runtime/layout.{h,cpp}` with cached descriptors, byte projections,
   array strides, variant layouts, and checked size/alignment arithmetic.
3. Implement cycle detection with a useful inline-field path diagnostic. Keep
   cycles through fixed-size owning/reference handles finite.
4. Implement packed and zero-sized layout rules, unsupported-type errors, and
   matching `size_of`/`align_of` answers.
5. Provide actual aligned allocation/frame storage for every supported layout;
   update allocator and arena tests, including arrays of `int128` values.
6. Retain old layout APIs only behind explicitly named migration adapters until
   their consumers migrate. Do not change live code to new sizes while it still
   stores old pointers under the same type identity.

**Status: complete (2026-11-09).** The checker records concrete struct fields and
sum payloads per type in `checked_types::shapes` (`src/semantic/aggregate_shape.h`).
`runtime::value_layouts` (`src/runtime/value_layout.{h,cpp}`) computes cached byte
layouts from them. Nothing in the compiler reads these layouts yet, so no live
size changed. Three points differ from the steps above:

- The new engine is a separate file rather than an extension of `layout.{h,cpp}`.
  The old API is unchanged and is not renamed to adapter names; renaming it would
  touch every backend call site. T-05 and T-06 remove it as each consumer moves.
- `value_layouts::size_of`/`align_of` return the new answers, but the `size_of[T]()`
  and `align_of[T]()` intrinsics still answer from the old `layout_of`. They switch
  when the backends do, so that the intrinsic never disagrees with the stride the
  code actually uses.
- The infinite-size diagnostic is produced by the engine but not yet shown to
  users. No pass asks for a layout until T-05, and the checker accepts a type that
  contains itself (`type direct = { inner: direct }`) today. T-05 must report the
  engine's error when it first lays out such a type.

Aligned storage: `cinder_heap_alloc_aligned` and `cinder_heap_realloc_aligned`
(both allocator modes), `bump_arena::allocate(bytes, align)`, and
`runtime::aligned_buffer`, which now backs the VM's per-frame `uninit` storage at
16-byte alignment. LLVM frame allocas and the plain heap entry points are
unchanged.

### T-03 — Add shared storage form and aggregate ABI

**Goal:** Express final destinations and ownership transfers once.

1. Add typed places, storage classes, projections, construction, copy/move, and
   call-result operations under `src/hir/`, with source spans and logical owner IDs.
2. Add ABI classification for scalars, references, handles, and inline aggregates,
   including direct/indirect callable signatures and synthesized functions.
3. Implement `initialize_into` and result forwarding for straight-line constructors
   and calls from the start; do not introduce temporary boxing as the new default.
4. Implement fresh outgoing parameter/result places and initialization-state edges.
   Preserve source argument order and parameter cleanup responsibility.
5. Add a shared verifier for projection bounds, initialization, valid transfers,
   storage lifetime, signature agreement, and result ownership on normal return.
6. Add stable dumps and tests. Route each compilation entry point through this
   preparation layer as its backend is migrated; keep representation versions
   explicit during the transition.

**Status: complete for straight-line code (2026-11-09).** `src/hir/storage.{h,cpp}`
defines the form (slots, places with member/element/payload/deref projections,
operations, stable dumps). `src/hir/abi.{h,cpp}` classifies values as zero-sized,
scalar, address, handle, descriptor, or aggregate and builds internal signatures
`make(result_address?, args…)`. `src/hir/storage_lower.{h,cpp}` lowers HIR to the
form and `src/hir/storage_verify.{h,cpp}` checks it. `storage_test` pins dumps,
exercises the verifier's rules by corrupting lowered functions, and was
mutation-checked against the lowering. No backend consumes the form yet. Points
that differ from the steps above:

- Straight-line only at T-03. String literals, array fill, closures, and
  coroutine/generator bodies are still refused by name; the refusal list is
  returned beside the module rather than the function being half lowered.
  T-04a adds `if` and loops, and scalar compound assignment.
- HIR already schedules drops as explicit calls. T-03 refused droppable
  assignment and temporaries; T-04 translates the calls (see its status).
- Outgoing arguments use `outgoing_argument` slots consumed by the call. Scalar
  arguments are snapshotted into temporaries only when a later sibling has
  effects.
- Aggregate assignment is built in a fresh temporary and moved in, so the old value
  can be read while the new one is built. NRVO (T-11) applies only to a returned local, not to an assignment.

### T-04 — Integrate ownership, references, and drops

**Goal:** Preserve the language's existing resource and borrowing guarantees.

1. Carry checked passing modes, copy classifications, drop plans, loan provenance,
   and partial-move/initialization flags into storage lowering.
2. Update reference elaboration and receiver handling to use actual places and
   explicit semantic modes, independent of heap classification.
3. Translate scope/temporary/early-exit drops to places; keep specified drop order.
   Distinguish dropping an inline value from freeing an owning allocation.
4. Implement replacement through temporary storage and ownership commit. Preserve
   source state until the replacement is ready and avoid duplicate drops.
5. Reject generic moves through borrowed `self` rather than relying on pointer
   aliases to implement them. Keep the consuming-receiver question separate.
6. Add meaningful checked tests for partial moves, copy sums, mutable views,
   borrowed aggregate fields, and constructor operands that exit early.

**Status: complete for straight-line code (2026-11-09).** Lowering marks the drop
calls HIR synthesizes (`hir_call::is_drop`, set in `build_drop_calls`) and the
storage form keeps them as `call.drop` operations on the address of a place. The
verifier now owns the ownership rules, with `check_leaks` on by default:

- A value owes its own drop and one drop per droppable field, read from the
  checker's drop plan. A move removes the moved part from what is owed, so a
  partial move leaves exactly the remaining fields owing a drop.
- A value's own drop runs at most once (`double_drop`). Nothing reads, borrows,
  moves, or writes inside a value after its drop ran until a write replaces it
  (`use_after_drop`). A drop of bytes behind a reference is rejected.
- A write over a value that needs a drop is accepted only after its drops ran
  (`already_initialized` otherwise). That is the replacement protocol: HIR
  builds the new value in a local, drops the old one, then stores the local, and
  lowering writes it straight in. A droppable slot cannot end or return while it
  still owes a drop (`leaked_value`).
- Lowering now handles the blocks HIR builds around discarded temporaries and
  values computed before scope-exit drops, and no longer refuses assignment
  over, or discarded results of, droppable types.

Covered by `storage_test` over `drops.cn`: constructors, scope-exit drops of
locals and owned parameters, field-wise drops, a partial move, a replaced field,
a discarded temporary, a borrow, and an argument moved to a callee. Each rule
was mutation-checked. Not done, and carried forward:

- Control flow. T-04a adds `if`, `while`, `break`, `continue`, and early `return`.
  Drop flags for values moved on some paths and the drop of a sum with
  droppable payloads (HIR expresses it as a `match`) are still refused;
  `match`, `while let`, and `let else` are done (see T-09).
- Steps 2 and 5. Reference elaboration already produces explicit `&`/`*` in HIR,
  so the form reads places from it unchanged. Rejecting generic moves through a
  borrowed `self` is a checker rule and was not touched.
- Closure and generator drops (`hir_closure_drop`, `hir_coroutine_cancel`) are
  translated since T-09; see below.

### T-04a — Add structured control flow to the storage form

**Goal:** Let the storage form describe functions with branches and loops, so a
backend can compile real function bodies from it. T-04 left this open, and no
other task owned it.

**Status: complete (2026-11-09).** The form gains structured markers in the flat
operation list: `if`/`else`/`end_if` (the condition is a scalar operand),
`loop`/`end_loop`, `block`/`end_block`, and `exit N`, which leaves the N-th
enclosing loop or block (0 is the innermost; `if` regions are not counted).
`exit` is the only way out of a loop. The verifier replays the form once and
joins state where paths meet:

- Where branches meet, each slot's initialization is reduced to what both paths
  know. A value that owes a drop on one path and not on the other is
  `join_mismatch`, because that needs a drop flag, which the form does not model.
  A slot that ended on one path and still holds a value on the other is also a
  `join_mismatch`. A path that returned or exited does not take part in the join.
- A loop body must end in the state the loop was entered with, so the second
  iteration sees what the first did. Storage declared inside the body ends before
  the loop repeats, which lowering emits. Complete parts are compared as whole
  values, so a struct built field by field equals one assigned whole.
- Only the markers that close a region may follow a `return` or `exit` in the
  same region. A function must end on a path that has returned, and every region
  must be closed (`missing_return`, `code_after_return`, `bad_operation`).

Lowering handles `if` with `elif` and `else` as a statement or a value built in
its destination, `while` with its `step` (which a `for` loop uses), `break`,
`continue`, `while true`, and `return` from inside either. A `while` becomes a
loop that tests its condition and exits when it fails, then a block holding the
body, then the step, so `continue` leaves the block and the step still runs.
Compound assignment on a scalar is lowered because the `for` desugaring needs it.
Each condition is held in its own slot, which ends on both sides of the branch.
A block of source code that yields a value now ends in an expression statement,
and lowering reads that as the block's value.

Covered by `storage_test` over `flow.cn`: exact dumps for value-producing and
statement `if`, `elif` chains, a result built in the caller's bytes from each arm,
`while` with locals, `while true` with `break`, `continue` with a step, nested
loops (exit depths), `return` from a loop, and drops inside a branch and inside a
loop body. Verifier tests corrupt lowered functions for each new rule. Mutation
checks (join drop check, loop-carried check, exit depth, branch termination) each
failed the tests. Not done:

- `match`, `while let`, and `let else` were added afterwards; see "Patterns in
  the storage form" under T-09 below. Drop flags remain rejected.
- `break` and `continue` inside an expression that has live temporaries are
  refused by name. Early exits inside constructor operands are not covered.
- Wrapping and saturating compound assignment, and compound assignment on a
  non-scalar target, are refused.

### T-05 — Implement VM frame values and result destinations

**Goal:** Execute inline values on the reference tier.

**Status: partly done (2026-11-10).** A second bytecode compiler,
`bytecode_compiler/storage_compile.{h,cpp}`, builds bytecode from the storage
form. It is not called by the driver, because decision 6 forbids mixing the two
representations in one program and the storage form cannot yet express
strings, generators, or globals. Done:

- Metadata: `bytecode_function::abi` records whether the result goes through an
  address and the size, alignment, and by-address flag of the result and each
  source parameter. Registers `[0, param_count)` hold the hidden result address
  (when present) and then each non-zero-sized parameter.
- Opcode: `op_copy_bytes dst, src, imm32` (`memmove`).
- Frames: a scalar, address, or handle with no address taken lives in a
  register. Everything else lives at a compile-time offset in the frame's
  aligned, address-stable byte storage. Signed integers narrower than 64 bits
  are sign-extended after a load.
- Compiled: constructors built in place, member/element/payload projections,
  assignment (replacement built before the old value ends), borrows, calls with
  results built in the caller's bytes, parameters by address, drops, `if`,
  loops, `break`, `continue`, early `return`, scalar arithmetic.
- Entry points: `vm::run_into` and `vm::run_hosted_into` take caller-owned
  result storage and refuse storage that is too small, misaligned, or paired
  with a function that has no result address.
- Lowering fix found on the way: `and`/`or` evaluated their right side
  eagerly; they now lower to a conditional.
- Covered by `storage_compile_test` over `bytecode_storage_test/values.cn`,
  `drops.cn`, and `hir_storage_test/flow.cn`, with expected values worked out by
  hand (layout offsets, tags, narrow widths, a 3000-deep recursion that grows
  the frame vector, a heap-allocation count of zero, no `op_alloc` in the
  output). Mutation checks (short copy, payload offset, sign extension) each
  failed a test; the first sign-extension test did not and was replaced.

`never` and trailing values (done 2026-11-12). `never` has a zero-sized layout. A call that returns it is followed by a new `unreachable` operation, which the verifier accepts only after such a call (the ends of its outgoing slots may intervene) and treats as the end of the path; the bytecode compiler emits a panic there as a backstop and LLVM emits `unreachable`. A branch that ends in such a call writes no value to the destination its siblings write. The body's trailing expression statement (HIR's typing of a trailing `match` or `if` as the function's value) is lowered as a return of that expression, except when every path through it already leaves, which keeps the earlier form. The test fixture now lowers and compiles the stdlib modules a program reaches (`panic` is in `std.panic`), each in full. Tests: `storage_test` over `flow.cn` (exact forms for a guarded `panic`, a diverging `if` arm, a trailing `panic`; a trailing `match` returned through its temporary and a trailing `if` building a pair in the result; an `unreachable` after an ordinary operation and a `never` call without one are rejected) and `storage_cases.h` on both backends (the values, and on the VM the panics; the native runtime ends the process on an explicit panic, which `case_runner::reports_explicit_panic` records). Mutation checks (no `unreachable` emitted, a `never` branch lowered into the destination, the tail never returned, every `match` taken to leave) each failed a test. Across the stdlib, 465 of 516 functions now lower (452 before); what remains is string literals, container length and data, run-time indexing, array fills, and two single cases.

String literals (done 2026-11-12). A `str` literal lowers to a `string_literal` operation holding its decoded bytes; the bytes are static data (the VM's `string_constants`, read by the new `op_str_const_data`, which yields their address without the heap header `op_load_str_const` makes; a private LLVM global), and the operation writes the `{len, data}` descriptor into its destination. The verifier requires the destination to be a `str`. Tests: `storage_test` over `forms.cn` (exact forms for a returned literal and one through a local, which is a copy since `str` is copied; a literal written into an `int32` field is rejected) and `storage_intrinsic_cases.h` over `bytecode_storage_test/strings.cn` on both backends (scalar count of a non-ASCII literal, a literal chosen by a branch, returned, escaped, empty, compared with a host string and with another literal, stored in a struct field and read back, with zero allocations throughout). Mutation checks (the text truncated in the lowering, the VM storing the data word over the length, LLVM storing a length off by one) each failed a test.

Descriptor words (done 2026-11-12). A `str` or `slice` descriptor's length and data words are places of their own (`length` and `data` projections, offsets 0 and 8, each carrying the type it is read as: `usize`, and a raw pointer to the elements, `*byte` for a `str`). `xs.len()` reads the length word, or is the count of an array; `xs.as_ptr()` reads the data word, or takes the address of an array, which the verifier accepts as a pointer to the array's elements (an empty array's address is its zero-sized slot's); `slice_from_raw_parts(p, n)` writes both words, and a descriptor is complete once both are written. A computed container is built in a temporary first. `uninit[T, N]` buffers stay refused by name; no stdlib function reaches one. Tests: `storage_test` over `forms.cn` (exact forms for a length, a data word, an array's address, and a view built from parts; a length read as `int64`, a data word read through the length, a view with only its length written, and an `int64` array's address typed as a byte pointer are rejected) and `storage_intrinsic_cases.h` over `bytecode_storage_test/views.cn` on both backends (byte length of a non-ASCII parameter and literal, the first byte through the data word, an array element through its address, an empty array's address, and a view built over allocated words read back through its own length and data). Mutation checks (each backend reading the length at the data offset, the verifier counting a descriptor complete with only its length, the part type check skipped, the array address taken at element 1, any pointer accepted for an array's address) each failed a test. Across the stdlib, 500 of 516 functions now lower; what remains is run-time indexing (20), array fills (2), a field of a computed value, and a `while let` over a droppable option.

Run-time indexing (done 2026-11-12). `xs[i]` on an array, `str`, or `slice` at an index that is not a constant is a `checked_index` compute (the index against the length; both `usize`, panicking with `index_out_of_bounds`), a `pointer_offset` from the data word or the array's address with the element's size as the stride, and a place through the resulting pointer, so it can be read or assigned. A constant index into an array stays an element projection. `xs[a..b]` checks `b <= len` and then `a <= b` with `checked_end`, widens an inclusive end by one first, and writes the view's data and length words. The verifier now requires every offset's stride to equal the size of what its result points at. An element that needs a drop is refused at a run-time index, since reading it would move it out of its container; no stdlib function reaches that case. The storage test runners now give a panicking function that returns through an address a result buffer, and the LLVM runner opens a `hosted_panic_scope`, since a bounds panic is fatal otherwise. Tests: `storage_test` over `forms.cn` (exact forms for a byte at an index and a substring; a bounds check into an `int64`, a range bound checked into an `int64`, and a byte offset stepping 8 bytes are rejected) and `storage_intrinsic_cases.h` over `bytecode_storage_test/indexing.cn` on both backends (reads and writes at an index, a 16-byte tuple stride, bytes of a `str`, substrings, inclusive ranges, an empty range, a window over an array, and out-of-bounds indexes and ranges on each). Mutation checks (either check skipped in the verifier, the start checked against the length instead of the end, an inclusive end not widened, and each backend's compare off by one) each failed a test. Across the stdlib, four functions are still refused: two array fills, a field of a computed value (`str::contains`), and a `while let` over a droppable `option[test_suite]`.

Fills, computed fields, and loops over droppable values (done 2026-11-12). `[v; n]` builds `v` into element 0, and a new `replicate` operation copies element 0 over the rest of the array. Both backends double the written prefix with constant-size copies (`op_copy_bytes`, `memcpy`), so a 4096-byte buffer takes 12 copies and no loop. The verifier accepts only element 0 of the destination as the source, written and `copy`, and counts the whole array written afterwards. A field, tuple element, or constant array index of a computed value reads from a temporary that lasts to the end of the statement (`make_pair(n).b`). A `while let` whose subject needs a drop, which is how a `for` over values that need a drop arrives, tests the variants as `let else` does: the payload is moved or dropped in the body, and the loop is left where the pattern fails. The comment that said HIR never sends such a loop was wrong for `for` loops. The storage tests do not run the ownership check, so a fixture that fills an array with a tuple (which moves) lowered to a refusal rather than a diagnostic; the fixture was corrected. Tests: `storage_test` over `forms.cn` (exact forms for a fill and a field of a call's result; a fill copied from element 1, and one whose element 0 was never written, are rejected) and `storage_intrinsic_cases.h` over `bytecode_storage_test/computed.cn` on both backends (fills of 5, 7, and 4096 elements; a field, a tuple element, and an array element of a call's result; `while let` and `for` over an iterator of droppable values, including a `break` and an item moved into a call, checked through a drop log). Mutation checks (the VM fill doing nothing, the LLVM fill skipping its last copy, the verifier taking any source, and `while let` ignoring droppable subjects) each failed a test. Every stdlib function now lowers to the storage form.

Array drops (done 2026-11-12). An owned `array[T, n]` whose elements need a drop now drops them, in index order, on every tier. The checker's drop plan for such an array names the element, a `*mut T` to it, and the length. HIR lowers the drop to a loop over a raw pointer to the first element (`container_data` with `takes_elements`), calling each element's drop. In the storage form, `takes_elements` adds a `forget` of the array after its address is taken: the verifier treats `forget` like `drop_in_place`, and the backends emit nothing for it. The verifier accepts a drop of a receiver reached through a raw pointer, since raw memory is the program's responsibility, and it still reports an array that is neither dropped nor given up. `std.mem.forget[T]` gives a value up without dropping it, by a raw write into `uninit[T, 1]`; `list.from_array` calls it after its raw reads. `uninit` storage lowers in the storage form as a new `zero_fill` operation, allowed only on `uninit` and counted as writing it; the VM runs it as `op_zero_bytes` and LLVM as a memset. Three other gaps on the way were closed. Dereferencing a `cell[T]` or `cell_mut[T]` reaches the `T` (`storage_env::pointee`). A method that owns its receiver (each `into_iterator` impl's `into_iter`, which the checker records in `ownership_results::consuming_receivers`) takes `self` as a `T` passed by address, and its caller moves the receiver in (`hir_function::owns_receiver`, `hir_call::receiver_by_value`). A raw-pointer return of another pointer type (`*mut T` returned as `*T`) goes through a temporary of the result type. The storage backends also run `rt_atomic_fetch_add`/`rt_atomic_fetch_sub` inline, as the legacy tiers do. Tests: `codegen_stress/188_array_element_drops.cn` (a local, a parameter, a temporary, a struct field, an array of arrays, and a `list` literal, 50 passes; `# storage-max-live-blocks: 0` in `storage_corpus_test`, `# max-live-blocks:` on the legacy tiers) and `storage_test` over `hir_storage_test/arrays.cn` (exact forms of an array drop, a raw write into `uninit`, and an owned receiver and its caller; an array that is never given up, a zero fill of other storage, and a receiver moved before it is filled are rejected). Mutation checks (a `forget` that drops, no array plan, the leak check skipping arrays) each failed a test.

Statics (done 2026-11-12). A module's reified statics are `global` slots, built by a function the lowering adds, `static init` (a name no source function can have), which a program runs once per module, in module order, before anything else. The module lists its statics (`storage_module::globals`). The verifier gives the static init the only unwritten `global` slots and requires each to be complete at its return; every other function starts with them written, cannot write them, and cannot move out of them. The VM lays every module's statics out in one zeroed, 16-byte-aligned block owned by the `vm` (`bytecode_module::global_byte_size`); a new `op_global_addr` gives a function the address of one at entry, and `static_initializers` names the functions a host runs first. LLVM gives each static its own internal global and registers one constructor in `llvm.global_ctors` that calls each static init; the JIT runs it when it loads the module, and an AOT executable before `main`. Struct statics are not globals: HIR folds a read of one into its value, as before. Three fixes came from the stdlib functions a list static reaches. The verifier's join now settles a struct member by member, so a struct whose optional fields are each dropped under their own `if` verifies; its normalization had also folded a fully written struct back into a whole value and forgotten what a test had shown about a field's variant. A variable read through `ptr_cast` (which has no HIR node) is copied into a temporary of the cast type. The address of a `T` may be typed `cell[T]` or `cell_mut[T]`. A `list` static still fails: `list.from_array` gives up its array argument by raw reads, and the checker never drops an owned `array[T, n]` of droppable elements, so the verifier reports the array's drop as owed (`spec/todo.md` item 19). Tests: `storage_test` over `hir_storage_test/statics.cn` (exact forms of a static init and of readers; a write outside the static init, a move out of a static, a static init that leaves a static incomplete, and a wrong starting state on either side are rejected) and over `joins.cn` (the member-wise join, a struct whose second optional field is never dropped, the `ptr_cast` copy, a `cell[T]` address and one of the wrong type), and `storage_intrinsic_cases.h` over `bytecode_storage_test/statics.cn` on both backends (array, struct, nested struct, tuple, and `str` statics read from several functions, by index and by a `for`). Mutation checks (each verifier rule, normalization forgetting the narrowing, the join not member-wise or spending an owed part, the cast not retyped, the cell check off or untyped, the VM sharing one offset or skipping the static init, LLVM not registering the constructor or aliasing every static to one global) each failed a test.

Calls between modules (done 2026-11-12, both storage backends). `compile_storage_module` takes a program of storage modules, the entry module first, and compiles them as one unit with one function table. An entry-module function keeps its name; any other module's is `module::name` (`hir::program_function_name`, shared by both backends and the mangling the legacy compilers use), so modules may share a function's name. A call resolves through `program_callee_name`: the callee's `owner_module` when the call names one, else the calling module. The synthesized drop calls carry the owner of the type's `impl drop`, so a value whose type lives in another module is dropped by a call into it. The verifier gained `verify_options::program`, the other modules, and checks a call into one of them against the function it names (a call into a module not given is taken on trust, as before). Tests: `storage_fixture.h` builds a program from a fixture and the companions it names with `# with: <file>` lines, lowers each module, and verifies them against each other; `bytecode_storage_test/modules.cn` with `modules_tools.cn` through `storage_module_cases.h` on both backends (an aggregate result built in the caller's frame by the other module and borrowed back to it, a result of the other module's type forwarded, an aggregate argument handed over, a 24-byte value both ways, a chain inside the other module, and a droppable value consumed there and dropped here, with drop counts; the entry module has a `make` with another signature than `tools.make`); `storage_compile_test` checks the table holds `make` and `tools::make` with their own conventions and modules; `storage_test` pins the owners the lowering records for the calls and the drops, and checks the verifier catches a skewed cross-module call only with the program and compares the local call with the local function. Mutation checks (callee key ignoring the owner, every function keyed bare, verifier never reading the program, verifier resolving every callee locally) each failed a test.

Not done:

- Items 4 (index and range operations, patterns, aggregate equality) and 6
  (indirect calls) in part: the compiler refuses by name `drop_in_place` of
  types other than `fn`, generators, and tasks, 128-bit scalars, and offsets or
  frames beyond operand limits. Anything the storage form refuses is out of
  reach until it grows strings and globals.
- Removing `emit_alloc_slots` from the old compiler, which waits on the driver
  switch.

1. Extend bytecode function/call metadata with aggregate ABI and frame-place
   information. Keep source parameters distinct from hidden transport operands.
2. Add checked aggregate transfer/address operations as needed and update opcode
   operand definitions, instruction sizes, disassembly, and verification together.
3. Extend frame allocation with correctly aligned, address-stable byte storage.
   Preserve scalar register allocation and live address constraints.
4. Compile constructors, projections, index/range operations, assignment, patterns,
   references, equality, calls, and returns through typed places. Remove per-value
   `emit_alloc_slots` from migrated ordinary aggregate construction.
5. Extend VM and hosted entry APIs to accept caller-owned result storage. Decode
   returned aggregates before their owner releases that storage.
6. Verify nested calls, deep recursion, frame-vector relocation, indirect calls,
   and values larger than sixteen bytes without relying on LLVM behavior.

### T-06 — Implement LLVM values and result destinations

**Goal:** Match VM value behavior in native code.

**Status: partly done (2026-11-10).** A second LLVM compiler,
`llvm_codegen/storage_codegen.{h,cpp}`, builds IR from the storage form. Like the
T-05 bytecode compiler it is not called by the driver (decision 6). Done:

- Frame values: every slot is an entry-block byte `alloca` of its layout size and
  alignment. Scalars are loaded and stored at their own width (`bool` is a byte
  in memory and `i1` in registers); aggregate transfers are `memmove`.
- Internal signature follows `abi_signature::operands()`: hidden result `ptr`
  first with a `void` return, by-address parameters as `ptr`, scalar results
  returned directly with `signext`/`zeroext` by type. No heap allocation symbol
  is declared or called.
- Alignment: accesses claim the smaller of the natural alignment and what the
  slot base and offset guarantee; packed projections are accessed unaligned and
  anything reached through a loaded address uses alignment 1.
- Shared scalar emission: checked arithmetic, casts, literals, and panics moved
  from `codegen.cpp` into `scalar_ops.{h,cpp}` (`scalar_builder`), used by both
  LLVM compilers so the panic reasons match.
- Entry thunks (`__cinder_entry_<name>(const uint64_t *args, void *result)`,
  `storage_codegen_options::entry_thunks`) give the JIT one call shape;
  `jit_module::run_storage` calls them and turns a panic into a
  `panic_reason`.
- Same cases as T-05: the value cases moved to `src/testing/storage_cases.h`
  and run through both backends (LLVM at O0 and O2), plus structural IR checks
  (hidden result parameter, parameter by address, no allocator, alignment 1 on
  packed accesses through addresses). Mutation checks (alignment over-claim,
  short `memmove`, payload offset, flipped result extension) each failed a test.

Calls between modules are done as for T-05 (2026-11-12): every function of
every module is declared in one `llvm::Module` before any body, named by
`hir::program_function_name`, and a call into another module is a direct call;
entry thunks are named after the symbol they wrap.

Not done: index and range operations, aggregate equality, `drop_in_place` of
types other than `fn`, generators, and tasks, 128-bit scalars; scalar tail
calls are not specially handled; the AOT entry (`cinder build`) and external
runtime wrappers still use the old compiler; the driver switch.

1. Replace heap-value storage classification with the shared layout and ABI plan.
   Emit aggregate definitions/calls, function descriptors, and return destinations
   consistently across modules.
2. Implement place addresses and checked scalar/aggregate accesses, using actual
   allocation alignment and packed projection alignment.
3. Replace ordinary aggregate heap constructors with destination construction;
   emit copy/move transfers only where the shared plan requires them.
4. Update JIT invocation wrappers, AOT entry generation, external runtime wrappers,
   and structural IR checks for hidden result parameters.
5. Verify modules and run the same cases as T-05 at low optimization. Keep scalar
   calling behavior and valid scalar tail recursion working.

### T-07 — Migrate runtime intrinsic boundaries

**Goal:** Remove runtime-created wrapper leaks as well as compiler-created ones.

**Status: partly done (2026-11-10).** Intrinsics whose result is a `result`,
`str`, or other aggregate now have a second runtime entry point that writes into
caller storage, and both storage-form compilers call it. The old entry points
stay until T-14 because the typed-HIR compilers still call them. Done:

- Metadata: `intrinsic_signature::into` marks 29 rows in `src/intrinsics.h`
  (stdio handles, open/close/read/write/flush, pipe, set_nonblocking, the string
  and formatting functions that return a `str` or `find_result`, and the six
  platform queries). `rt_str_len_scalars`, `rt_str_eq`, and `rt_str_cmp` return
  scalars and are unchanged. `intrinsic_into_symbol` names the symbol
  `cinder_<name>_into`.
- Runtime: each `_into` function takes the destination first, returns `void`,
  and writes every byte of the result, padding zero. A `str` allocates only its
  data bytes, and an empty one allocates nothing. Layout constants live in
  `src/runtime/result_out.h`; `value_layout_test` checks them against the
  compiler's layouts. The legacy and `_into` functions share their cores
  (`io.cpp`, `fmt.cpp`).
- VM: `op_call_intrinsic_into` passes the destination address as the first
  argument register; its handler table is built by name and aborts if a row's
  `into` flag and its handler disagree. `op_call_intrinsic` is unchanged.
- LLVM: the `_into` symbol is declared on first use with a `ptr` first
  parameter and a `void` return.
- Validation: `hir::check_intrinsic_signature` compares a call's `abi_signature`
  with the symbol (result in a register or in storage, parameter count, scalar
  width, pointer parameters). Both storage compilers run it and refuse a
  mismatch.
- Tests: `src/testing/storage_intrinsic_cases.h` runs on both backends (success
  and `EBADF`/`EAGAIN` paths, a pipe round trip, 500 repeated pipe and string
  calls with zero net heap blocks, one allocation per non-empty `str`, none for an
  empty one); `result_out_test` calls the entry points directly against the host;
  `intrinsic_abi_test` covers the signature check. Mutation checks (error code
  off by one, extra allocation, swapped `uname` fields, disabled signature check)
  each failed a test.

Not done: asynchronous reactor, scheduler, and coroutine adapters; `match` and
string literals in the storage lowerer (so the fixtures call the intrinsics
directly); reading a field of a call result without a local; removing the
legacy wrapper-allocating symbols (T-14); the driver switch; the inline atomics.

1. Extend intrinsic wire metadata for explicit output destinations and inline view
   arguments; update signature validation and declarations.
2. Migrate native runtime functions and VM intrinsic handlers together. Replace
   allocated `make_result_ok`/`make_result_err` wrappers with writes to supplied
   result storage; include platform and asynchronous I/O paths.
3. Migrate descriptor readers/writers for strings, slices, formatting, and runtime
   callbacks. Specify alignment and lifetime at every C boundary.
4. Test success and error paths with repeated pipe/file operations, including
   callers that immediately propagate, discard, or destructure the result.
5. Remove old runtime wrapper-allocation adapters once all call sites migrate.

### T-08 — Migrate containers, statics, and compile-time execution

**Goal:** Make nested value storage and host conversion consistent.

**Status: partly done (2026-11-10).** Raw memory behind `*T` and `*mut T` now
holds aggregates inline in the storage form, on both storage-form backends.
Done:

- Places: `p[i]` on a raw pointer lowers to a `pointer_offset` computation
  (`p + i * stride`, with the stride the element's size under the concrete
  layouts) and a dereference of the result. An array at a constant index is an
  `element` projection. Nested fields of a slot (`p[1].to.b`) project from there.
- Ownership: the verifier follows chapter 38 behind a raw pointer. A read moves
  or copies the value out and leaves the slot untracked; a write never drops the
  old value; a value read out is dropped once by its new owner. The reference
  rule (no move out of borrowed bytes) is unchanged.
- Layout questions: a `size_of`/`align_of` literal carries its question
  (`hir_literal::question`) and the storage lowerer answers it from
  `value_layouts`, so `size_of[pair]()` is 16 where HIR's own answer is 8.
- Encoding limits: the VM's `op_addr_indexed` has a one-byte element size, so
  strides over 255 bytes are multiplied out first; LLVM uses a byte GEP.
- Fix: `let r = &mut x` in the storage lowerer copied the value of `x`; it now
  binds the address.
- Tests: `bytecode_storage_test/raw_memory.cn` through
  `storage_memory_cases.h` on both backends (strides checked from outside by
  reading the same bytes through other pointer types, including 320-byte slots;
  one allocation for 200 pairs; drop counts for read, overwrite, swap, and
  replace of a resource-owning element); `storage_test` covers the lowered form
  and the verifier rules. Mutation checks (stride +8, VM large-stride path off,
  layout question ignored, drop counted twice, raw move forbidden, offset type
  check off) each failed a test.

Not done: `std.mem`, `vec[T]`, and `list[T]` themselves (calls across modules
compile since 2026-11-12, but their bodies use `uninit`, generics, and strings
the storage form does not express); `uninit` stack buffers; `drop_in_place` as
a storage operation (the machine-layer function `drop_in_place` is ordinary
code);
`src/comptime_vm` (`values.cpp`, hosted calls, `backend.cpp`)
and `CINDER_COMPTIME_VERIFY`. A field assignment through a `&mut` binding
(`r.b = 50`) does not lower to HIR yet ("no concrete checked type"); the
fixture writes the whole value instead.

1. Audit `src/std/mem.cn`, list/collection/shared implementations, pointer indexing,
   `uninit`, and all element stride/offset encoding limits against the new layout.
2. Store nested aggregates inline in buffers and fields. Preserve move-only element
   ownership during growth, removal, iteration, overwrite, and destruction.
3. Implement raw aggregate reads/writes and `drop_in_place` with the machine-layer
   contract; exercise `swap` and `replace` with resource-owning values.
4. Update globals, static initialization/reification, constant emission, and layout
   metadata. Keep frozen globals stable and preserve static-read ownership rules.
5. Update `src/comptime_vm/values.cpp`, hosted-call APIs, host callbacks, and
   `src/comptime_vm/backend.cpp` to use typed storage and explicit lifetime owners.
6. Test nested generics, aggregates larger than a register, and compile-time/runtime
   result agreement. Run `CINDER_COMPTIME_VERIFY=1` on the relevant driver tests.

### T-09 — Migrate captures, generators, and task results

**Goal:** Keep values valid across suspension and cross-thread ownership transfer.

**Status: partly done (2026-11-11).** Patterns, closures, and generators now live in the
storage form on both storage-form backends. Patterns were added first because a
generator's `.next()` returns an `option[T]` that only `match` or `while let` can
read, so no storage-form program could observe a generator without them.

Patterns in the storage form. `test_variant` (`dest <- source is @variant`) reads
a sum's tag into a `bool`; both backends load the tag word at the layout's
`tag_offset` and compare it with the variant's tag. Lowering handles `match` (as a
statement or a value built in its destination), `while let`, and `let ... else`
over constructor, literal, tuple, struct, wildcard, and or-patterns. Tests nest so
a payload or literal is read only after its variant is confirmed; each arm is a
flag set by its tests and an `if` on it; the last arm has no test, since the
checker requires every value to be covered. A local subject is used in place and
a computed one is built in its own slot, which a `while let` rebuilds on every
iteration. The verifier now lets a payload move out of a sum that needs no drop
(the sum then cannot be read whole), and lets a payload slot be read once it
exists. Tests: `storage_test` over `hir_storage_test/patterns.cn` (exact dumps for
a value `match`, a statement `match`, literal patterns, an aggregate payload moved
out, `while let`, and `let else`; verifier corruptions for an unknown variant, a
non-sum, an aggregate destination, an unbuilt sum, and a payload moved twice) and
`storage_pattern_cases.h` over `bytecode_storage_test/patterns.cn` on both
backends. Mutation checks (tag constant, tag comparison, the source-initialized
check, the sum split, the last-arm shortcut, the arm value destination, and the
`while let` exit) each failed a test. Refused by name: a subject that needs a drop
(the drop of a sum with droppable payloads is still not modeled), an arm guard, a
subject that is part of a larger value (`match self.field`), and range, array, and
string patterns. A `for` loop over a droppable item still needs drop flags.

Sums that own drops (done 2026-11-11). HIR drops a sum with droppable payloads as a `match` over its variants whose arms call `drop` on the payload places, and a pattern arm that binds a payload moves it out and drops the others, so the storage form needed no new operation. Two things were missing. The lowering refused every pattern test on a subject that needs a drop; it now accepts a sum whose patterns ask only for the variant (constructor over wildcards, or a wildcard) and puts the tag test directly in the arm's `if`. The verifier learned which variant a sum holds: an `init_node` carries the variants it can still be, an `if` that reads the flag written by the `test_variant` just before it narrows the tested slot on each side, and `first_owed` for a whole sum owes only the payloads of the variants still possible. A payload can leave a sum that needs a drop only when its variant is known (`partial_move` otherwise), and a sum split that way owes what is left of the variant. At a join, two whole sums that differ only in their possible variants merge into one that can be either; a path that owes nothing on both sides still leaves the slot empty. Tests: `storage_test` over `hir_storage_test/sumdrops.cn` (exact forms for the scope-end drop, a bound payload, a payload left with the sum, `let else`, and a loop that ends on `none`; refusals of a tuple subject and an or-pattern; verifier corruptions for a payload taken before the variant is known, a payload not dropped where the variant holds one, a test for the wrong variant, an `if` that reads another flag, and a flag overwritten before the `if`) and `storage_pattern_cases.h` and `storage_generator_cases.h` on both backends (scope-end drops, bound and unbound payloads and their drop order, `while let` over `option[tracked]` with and without `break`, `let else`, assignment over a sum, a sum passed to a callee, and a generator whose items need a drop, read by `while let`, by one `match`, and discarded). Mutation checks (narrowing on each side, the owed payloads of a narrowed sum and of a split one, the join merge, the explode check, the flag the `if` reads, the lowering's refusal and its direct tag test, and `let else` termination) each failed a test; the flag check first survived, so its corruption was added. HIR never gives `lower_while_let` a subject that owes drops (it writes a `loop` around a `match`), so that case is refused as malformed. Task results that are such sums (option, result-like, user sums) are dropped the same way at the handle's drop: one tag test per variant that owns drops, each directly in its `if` (`task_results.cn`, `tasks.cn`; mutation checks of the dispatch, the tested variant, the payload drop, and the nested-sum refusal each failed a test, after a two-variant result was added to catch the tested-variant mutation). A sum inside another result value is refused by name.

Literal and nested tests on a droppable sum (done 2026-11-12). A `match` or `let else` over a sum that owes drops may now test literals on scalar parts, nested constructors, and tuple and struct patterns; or-patterns and ranges are refused by name. A nested test is the `if` on a `test_variant` of a payload place. Three verifier changes support it: narrowing walks the path, splitting each whole value on the way (the enclosing variant is already known) and skipping a path no variant can reach; a tag test of a split sum whose variant is known is accepted (the tag stays when payloads leave) unless a drop touched it; and a join merges variant sets at any depth, treating a split sum that still has all its payloads as the whole sum. The lowering lets a match subject be a part of a value (HIR's drop glue for a sum nested in a sum), and resets `terminated_` at each failure site: a nested pattern has several, each holding a copy of the remaining arms or the `else` block, and a copy that leaves used to hide the next, which the verifier caught as a move before the variant was known. Tests: `storage_test` over `sumdrops.cn` (exact forms for a literal payload, nested arms, nothing bound with the scope-end drop through both tags, and a nested `let else`; corruptions for a nested payload taken before the inner variant, under the wrong inner or outer variant, after the arms joined (nested and top level), and a tag test after the payload's drop) and `storage_pattern_cases.h` on both backends (literal holding and failing with the payload bound or left with the sum, nested take and peek, arms that all leave, nested `let else`; all with drop counts). Mutation checks (path walk, impossible-path skip, tag readability and its dropped check, the split/whole merge, the variant merge, the literal and or-pattern acceptance, the subject alias, and both resets) each failed a test. Three checks of states the lowering does not produce (the child-count check in the merge, and the payload-complete and single-variant checks when a split sum is rejoined) survive mutation; they guard against a hand-built form. Not done: or-patterns and ranges over a droppable sum; the failure path is copied at every place a test can fail, so a pattern with many nested tests grows the code.

Match arm guards (done 2026-11-12). An arm with a guard tests its pattern, runs the `let`s HIR places ahead of the guard (they bind the names the guard reads), evaluates the guard, and runs the body when it holds; a failed pattern and a failed guard both go on to the remaining arms, which are built at each of the two places (a guard therefore doubles the code of the arms after it, and nested tests add more copies). On a subject that needs a drop the guard is accepted when the bindings are plain values that need no drop, so the subject stays whole through a failed guard; it is refused by name when a binding needs a drop or when HIR runs a drop (of a payload the pattern does not name, which it does when a later arm takes that payload) before the guard, because a failed guard would find the payload gone. Tests: `storage_test` over `patterns.cn` and `sumdrops.cn` (exact forms for a guarded arm, an always-matching arm with a guard, and a guard on a sum that owns a drop with the scope-end drop through both tags; both refusals; the verifier accepts the rest) and `storage_pattern_cases.h` over `bytecode_storage_test/patterns.cn` on both backends (guard reads bindings, pattern failing and guard failing, a statement `match`, a guard on an always-matching arm, guards that run once in order and only when their pattern holds, drop counts on a guarded droppable sum, and two literal tests before a guard with every arm leaving). Mutation checks (skipping the pattern test, each refusal, the bindings, the remaining arms at the guard's failure site, the guard's temporary, and the `terminated_` reset) each failed a test, except the guard on the last arm, which is unreachable (the checker requires an unguarded arm to cover the rest) and so is untested. Not done: a guard whose pattern binds a value that needs a drop.

Generators in the storage form (done 2026-11-11). A `generator def` is a constructor plus a step function whose frame is the state block (see chapter 62). Items are handed over by address, so the fixed yielded-value image is not used and an item of any size works (a 24-byte struct is tested). Both backends implement `make_generator`, `resume`, `take`, `dispatch`, `suspend`, `cancelled`, `finish`, and `drop_in_place` of a generator. Tests: `storage_test` over `hir_storage_test/generators.cn` (exact dumps for the constructor, the step, and `.next()`; verifier corruptions for the entry, capture offsets, step-only operations, a suspension placed twice, an item read after hand-over, and the `take` types) and `storage_generator_cases.h` over `bytecode_storage_test/generators.cn` on both backends (order and sums, yields inside branches and loops, parameters of mixed size, a borrow held across suspensions, 24-byte items, passing and returning a generator, staying finished, and drop counts for a generator that ran to the end, was dropped while suspended, and never started). Mutation checks (resume index, finish status, item word, cancel status, status comparison, and the drop on each backend; the lowering's comparison, depth, and finish status; the verifier's placed-once and item-consumed rules) each failed a test; the first fixture let the finish-status mutation survive, so `stays_finished` was added. Reading an item that needs a drop through `.next()` works since the sums slice below (the match over the `option` drops it). Not done: liveness-based sizing of the state block (every slot has its own offset), and the adapters.

Tasks in the storage form (done 2026-11-11). An `async def` lowers like a generator whose body is `yield result; return`, so the constructor, step, state block, and result hand-over by address are the generator's. A new `await #k` operation is a resume point in a task's step: it polls the awaited task (`rt_task_await_poll`) and returns `pending` when that task is not finished, and the scheduler resumes the step at the same point. The awaited task is a state-block slot, and the cancel exit drops it before the other drops. In a plain function (including the entry `async def main`), `await` and `.get()` call `rt_task_block_on` and then `take`; `await` also drops the task. `rt_task_*` and `rt_crew_*` calls borrow their handle arguments. Block expressions now lower inside their own region, so a `let` in one no longer ends the enclosing temporaries early. Tests: `storage_test` over `hir_storage_test/tasks.cn` (exact dumps for the constructor, step, await, block-on, and spawn forms; verifier corruptions for a stray await, an await placed twice or not named by the dispatch, non-task operands, a task resumed by hand, mistyped results, and a cancelled wait that leaks the awaited task) and `storage_task_cases.h` over `bytecode_storage_test/tasks.cn` on both backends (single awaits, loops of awaits, `.get()`, `spawn`, entry `main`, dropping an unstarted task, results larger than a register, `unit` results, and a borrow held across an await). Mutation checks (await status, resume index, and branch sense on each backend; task recognized as a coroutine on each backend; the lowering's cancel-exit drop, block-on, borrowed handle arguments, await depth, and block-expression region; the verifier's result type, placed-once, resume-of-task, and await-of-generator rules) each failed a test; the last two first survived, so the fixture gained a generator held by a task and the test now checks the rule's message. Not done: the remaining adapters.

Group waits and `on` blocks in the storage form (done 2026-11-11). `await par` and `await race` build a tuple of task handles in the owning frame (the handles are moved into it) and wait on it with two new forms: `await all|any #k dest <- tuple` in a step, which polls with `rt_task_await_all_poll`/`rt_task_await_any_poll` (the coroutine, the tuple's address, the member count) and returns `pending` until the group is done, and `block_on_group all|any tuple` in a plain function, which calls `rt_task_block_on_all`/`rt_task_block_on_any`. The runtime reads the tuple in place (an 8-byte stride of handles), so no descriptor is built. The cancel exit drops every awaited task (a single task, each group member, or the `on` task) before the other drops; the legacy lowering names them through `coroutine_cancel_check`. `race` reads its winner through the tuple. `on(pool)` blocks already lower as ordinary tasks and now run on both backends. Tests: `storage_test` over `hir_storage_test/tasks.cn` (exact dumps for `together` and `first_wins`; verifier corruptions for a group await on a non-group, a wait mode on a single task, a block inside a step, a block with mode `one`, and a tuple of non-tasks) and `storage_task_cases.h` over `bytecode_storage_test/tasks.cn` (`par`, `race`, `on(pool)`, and a plain-function group wait, expecting 141264) on both backends. Mutation checks (all/any swapped in the await and in the block on each backend; member count on each backend; tuple passed by value; the lowering's mode and block names; the group-await refusal; the three cancel-exit drop lists; the verifier's four rules) each failed a test. One mutation survived because the lowering of a cancel of a whole tuple of tasks was unreachable, since the legacy lowering names each member; that branch was deleted. Known limits: the plain function's group wait also leaves the per-member `rt_task_block_on` the legacy lowering emits afterwards, which is redundant.

Task results that need a drop (done 2026-11-11, for infallible tasks). Reading a result already worked, because the reader owns what `take` moves out. The gap was a task that finished and was never read: its handle's drop leaked the result. A new operation `has_item dest = task` tests whether the object's value word holds an item address, and `take` now clears that word. Dropping a handle whose result type needs a drop first calls `rt_task_is_finished` (a task still running owns its result, so an unfinished one is skipped), then `has_item`, then takes the item into a temporary and drops it with synthesized calls: the type's own `drop`, then each droppable field in declaration order, or each droppable tuple member. The `storage_env` type table is now mutable so lowering can intern the `&mut T` receiver of that call. A result that is a sum with droppable payloads, a closure, or a generator, and any fallible task whose result needs a drop, are refused by name at the handle's drop. Tests: `storage_test` over `hir_storage_test/task_results.cn` (exact dumps for a plain, struct, and tuple result; verifier corruptions for a taken result that is never dropped and for an item test on a non-coroutine or into a non-flag; the fallible refusal) and `storage_task_cases.h` over `bytecode_storage_test/tasks.cn` on both backends (counting drops for a result read by `get`, by `await`, finished and never read, a struct and a tuple result in declaration order, read then handle dropped, and never started). Mutation checks (`take` keeping the item and `has_item` inverted on each backend; the lowering's early return, fallible refusal, field, own-drop and tuple-member drops; the verifier's handle and flag rules) each failed a test; the field and tuple mutations first survived because the HIR fixture had no struct or tuple result, so those fixtures were added. Not done: a task that is still running when its handle is dropped (a `race` loser) owns its result until it stops, and nothing disposes of that result if it finishes afterwards; this needs the runtime to hold drop glue for the result, or the dropper to wait. Reading a droppable generator item through `.next()` is still refused.

Crews in the storage form (done 2026-11-11, for `crew` blocks without `errors()`). A `crew` block is `rt_crew_new`, one `rt_crew_spawn` per task, a wait for the crew, and `rt_crew_free`. `rt_crew_spawn` takes the task it is given (the argument is moved) and returns the same handle, so the crew is the only handle argument that is borrowed; before this the task temporary stayed owed and the block was refused. A borrowed handle is now read in place even when a later argument has effects, because a copy would take it from its owner. The wait is a new `await crew #k` mode in a step (it polls `rt_crew_join_poll`) and `rt_crew_join` in a plain function; the verifier checks the operand is an initialized crew. A handle dropped while the crew runs leaves the task to the crew (`cancel_on_drop`). When the block's value is a task or a tuple of tasks, the legacy lowering's cancel check after the wait now releases them, which it did not before: no name owned them yet, so a cancellation there leaked them, and the verifier reported it. Tests: `storage_test` over `hir_storage_test/crews.cn` (exact forms for a crew in a task, a discarded handle, and a plain function; verifier corruptions for a crew wait on a task, a crew never made, a wait on a crew slot nothing wrote, and a cancelled block that leaks its tasks; `forms.cn` for the refused `errors()`) and `storage_task_cases.h` over `bytecode_storage_test/tasks.cn` on both backends (tasks handed back out of the block and read, a crew in a plain function, a loop of spawns, and a child still running when the body ends, left by falling off the end, by `return`, and in a plain function, so a block that did not wait would leave the cell unset). Mutation checks (the lowering's crew detection, handed-over spawn argument, in-place borrowed read, wait mode, and `errors()` refusal; the legacy cancel's held tasks; the verifier's crew and initialization checks; and, on each backend, the poll's function, operand, and result) each failed a test, except a VM operand count of three for a crew, which is equivalent because the intrinsic ignores the extra register; the uninitialized-crew check first survived, so that corruption was added. Not done: `c.errors()` (refused by name), a `.get()` inside a task body while another task handle is alive (the legacy lowering schedules no exit drops for it, so the verifier rejects the form), and tasks that are still running when their last handle is dropped.
Done (closures):

- Representation: a `fn` value is a 24-byte aggregate `{entry, env, glue}` held
  inline. The environment is one heap block per closure that captures anything,
  with each captured value inline at its own alignment (a typed layout, not
  register images). A closure with nothing to capture has a null environment and
  no block. Closure bodies and drop glue take the environment as a hidden first
  operand (`abi_signature::takes_environment`).
- Captures: `make_closure` stores each capture as a copy (bitwise; the source keeps
  ownership), a move (the closure owns it), or a reference (an address, reached in
  the body through an `env_indirect` slot). The verifier refuses to move a value
  out of an environment slot, checks capture offsets and alignment, and checks the
  entry and glue signatures against the module.
- Drops: the glue drops the owned captures in reverse capture order and frees the
  block with `release_environment`. `drop_in_place` of a `fn` value loads the glue
  and runs it when it is not null. Passing, returning, and dropping a closure
  behave as for any other aggregate.
- Calls: a call through a `fn` value loads the entry and environment from the
  value's bytes. The VM has `op_call_function` and `op_drop_function_value`; LLVM
  calls through the loaded pointer. The old heap closure opcodes remain for the
  typed-HIR compilers.
- Tests: `bytecode_storage_test/closures.cn` through `storage_closure_cases.h` on
  both backends (captures at offsets 0, 8, and 16; by-reference writes; drop
  counts and order for scope end, passing, several captures, and returning; one
  heap block per closure and none left behind). `storage_test` covers the lowered
  form and the verifier rules. Mutation checks (glue word zeroed, capture offset
  shifted, glue call removed, bounds check off, captured-move check off) each
  failed a test.

Not done: `crew`; a plain function name
used as a value; a closure capturing a zero-sized value; a closure that takes
ownership of a value captured by an enclosing closure; `drop_in_place` of types
other than `fn` and generators; generic and inlined lambdas in the storage form.

1. Replace uniform register-image environments/state entries with typed layouts
   for aggregate captures, parameters, locals, and initialized result storage.
2. Update capture and suspension analyses for projected places and borrowed
   temporaries. Put suspension-live objects directly in stable frame storage.
3. Replace the fixed yielded-value image and update `src/runtime/coroutine.h`,
   scheduler/reactor integration, native steps, VM polling, and final release.
4. Implement arbitrary-size generator/task result transfer, active-result flags,
   and correct disposal of unconsumed results on cancellation or handle drop.
5. Update `par`, `race`, `crew`, and `on` adapters to embed their tuples and
   descriptors in the owning storage. Preserve winner/loser and capture cleanup.
6. Run checked tests that suspend while borrowing an inline member or slice
   descriptor, migrate between workers, and cancel before/after producing a result.
   Include a non-copy result with an observable drop and a result over sixteen bytes.

### T-10 — Complete mandatory RVO propagation

**Goal:** Make direct result construction systematic across expression forms.

**Status: partly done (2026-11-11).** The lowering was already destination-driven, so the work was to measure it and fix what the measurement found. `hir_storage_test/rvo.cn` holds one function per form: a returned constructor, call, `if`, and `match`; tuple and array literals of calls; sum and `option` payloads; nested fields; `?`; `or else`; indirect and generic calls; chained returns; and a discarded or zero-sized result. `storage_test` asserts that the eligible ones contain no `copy_into` or `move_into` (the lowering runs on HIR with no inlining) and pins the exact form of four. Two defects came out of it. First, `x?` in a function whose result has a different success type returned the operand as it was, which the verifier rejected (`move joins result[A, E] and result[B, E]`); the failure is now rebuilt in the result. Second, a `?` in a later part of a struct, tuple, array, or sum literal left the earlier parts built in the destination and leaked them; the lowering now notes each finished part that needs a drop and a `return` inside the construction drops them newest first. The verifier lets a variant be published over leftovers of another variant when they owe nothing (they are scratch), and reports `leaked_value` when they do. Tests: `storage_test` over `rvo.cn` (zero transfers, exact forms of `ret_if`, `sum_payload`, `tuple_calls`, `ret_noisy_if`, the rebuilt failure for `result` and `option`, `duo_try` with its drop, `tuple_try`, `two_builds`; a corruption that removes the drop before the failure is published) and `storage_rvo_cases.h` over `bytecode_storage_test/rvo.cn` on both backends (fields weighted by powers of ten so a misplaced field changes the sum; `?` in every position with a different success type; drop counters for a failure after one, after two, and before any part is built, and for a whole value). Mutation checks (the partial drops, their registration, their release when the value is finished, their order, the rebuild, the failing variant, the verifier's release of leftovers and its leak check) each failed a test; the order mutation first survived until the three-part fixture was added. Not done: slice construction (`xs[a..b]` is refused as range indexing, under T-07); a tuple whose members need a drop has no drop plan, so a tuple of droppables cannot be consumed by a pattern (the checker schedules no drop for it, an older gap); `break` and `continue` inside a value under construction are refused (no surface syntax reaches them yet); an assignment still builds its replacement apart, as the plan requires when the target can be observed, and does not build in place even when it cannot; a returned named local is handled by T-11; no instruction-level check of the emitted code, since both backends read the same form and the form is what is asserted.

1. Extend shared destination propagation through all forms listed under Calls and
   mandatory RVO, including synthesized branches and error propagation.
2. Construct nested payloads and returned call results in their final projections.
   Preserve temporary storage when assignment or operand aliasing requires it.
3. Verify completion/initialization joins and cleanup on branches that never commit
   a result. Preserve handling of diverging expressions and zero-sized results.
4. Assert no intermediate aggregate transfers for eligible direct constructions
   and chained returns with HIR inlining disabled on both backends.

### T-11 — Implement shared NRVO

**Goal:** Reuse return storage for eligible named locals in ordinary builds.

1. Implement candidate discovery, dominance/return checks, loan/address analysis,
   and the conservative eligibility rules above in a shared HIR storage pass.
2. Redirect candidate initialization and projections to result storage, preserving
   logical local lifetime, drop flags, and cleanup before ownership transfer.
3. Implement ordinary inline move fallback with no heap allocation. Record why a
   candidate was rejected and never diagnose valid source merely for missing NRVO.
4. Provide an internal test option to disable NRVO while retaining mandatory RVO.
   Compare output, drop order, and ownership with NRVO enabled/disabled.
5. Check both the shared plan and emitted code for the eligible single-local case:
   no separate candidate buffer and no aggregate transfer at return.

**Status: done (2026-11-11).** `hir/storage_nrvo.{h,cpp}` is a pass over each lowered function, on by default; `storage_lower_options{.nrvo = false}` turns it off. When every `return` moves the same whole local into the result, the local's places become the result's, the moves and the local's `storage_dead` are removed, and its slot is deleted. The local must be a `frame_local` of the result's type, empty on entry, first written outside every `if` and loop, moved only at the returns, never captured, and never have its address taken (a synthesized `drop` call excepted). Anything else keeps the move, and `storage_lowering::nrvo` records the reason. A replacement assignment still builds apart and moves in. The pass exposed one verifier defect: a loop that mutates a local built in the result compared unequal at its ends because stale member records stayed under a whole value; `normalize` now clears them. Tests: `storage_test` over `hir_storage_test/nrvo.cn` (exact forms for `named`, `called`, `reassigned`, `two_returns`, `looped`, `sum_local`, and a droppable local; every declined case with its reason; the pass off; corrupted functions that capture the local, move it early, start it initialized, give it another type, or read it before it is written), and `storage_rvo_cases.h` over `bytecode_storage_test/rvo.cn` on both backends, run with NRVO on and off so results and drop counts must agree. Twelve mutations (each eligibility check, the return-move removal, the slot renumbering, and the `normalize` fix) each failed a test. Not done: a returned local that is assigned a whole new value inside a branch before its first use, and a returned field or projection of a local, keep the move; no instruction-level check of the emitted code, as for T-10.

### T-12 — Enforce final frame budgets and tail-call safety

**Goal:** Keep automatic storage bounded and callees' addresses valid.

1. Compute aligned frame ranges from the final storage plan, including ordinary
   locals, call slots, borrowed temporaries, `uninit`, and partial-construction state.
2. Reuse nonoverlapping ranges only after considering loans and pending drops.
   Exclude values stored in caller/global/closure/coroutine owners from local bytes.
3. Enforce the frame limit after transformations and report contributing spans.
   Test checked arithmetic and instruction-encoding overflow separately.
4. Revalidate tail calls with result/argument storage provenance; reject unsafe
   `musttail` or VM frame reuse and retain safe cases.
5. Assert constant frame usage for repeated slicing and value construction loops.
   Preserve native stack probes and VM call-depth checks.

**Status: partly done (2026-11-11).** `hir/storage_frame.{h,cpp}` computes each function's frame from the final storage form (after NRVO): the `frame_local` and `outgoing_argument` slots of nonzero size, each at its layout alignment, with checked arithmetic. Other owners' storage (parameters, caller result, globals, closure and coroutine blocks) is excluded. A function over `storage_lower_options::max_frame_bytes` (1 MiB by default, the existing language limit) is refused with `frame_too_large`, naming the total, the limit, and the three largest contributors, with the largest one's span; `storage_lowering::frames` records every plan. The bytecode compiler's 32-bit offset check is tested separately by lifting the language limit. Neither storage backend emits a tail call or `musttail`, which is now asserted on both, so frame reuse cannot reach a caller's outgoing arguments or locals. Tests: `storage_test` over `hir_storage_test/frames.cn` (padding, zero-sized slots, sum/size/padding overflow, array and outgoing-argument contributions, equal frames for one and two loop bodies, NRVO saving the local's bytes, refusal at a lowered limit and at the real limit for a 4.8 GB local, a limit that is exact to the byte, message order), `storage_compile_test` (encoding limit; no `op_tail_call`; results of tail-position calls) and `storage_codegen_test` (no `musttail`). Ten mutations (padding, each slot class, zero-size skip, both overflow checks, message order, the check itself, the comparison, the VM encoding check) each failed a test; two survived until the outgoing-argument and ordering tests were added.

Scalar tail calls (done 2026-11-13, item 4). A call HIR marks as a tail call stays a tail call in the storage form (`storage_op::tail_call`) only where `hir::is_safe_tail_call` admits it after NRVO: a direct call whose operands and result are scalars or nothing, followed only by `storage_dead` and the return of its whole result, in a function that is not a step, a closure body, or the static initializer and does not return through an address. A scalar cannot carry an address into the frame and nothing runs in the frame afterwards, so this is the provenance rule in its simplest sound form; a call that passes or returns bytes of the frame keeps the frame. The verifier applies the same rule to every marked call. The VM emits `op_tail_call`; LLVM emits `musttail` when the callee's prototype matches the caller's and the `tail` hint otherwise. Tests: `storage_test` over `hir_storage_test/tail_calls.cn` (the exact form of a self tail call, mutual recursion, no mark for an aggregate argument or a forwarded result, and verifier corruptions for a marked call with an aggregate argument and for work after a marked call), `storage_compile_test` (one `op_tail_call` in `count_down` and none in the aggregate cases, and `count_down` run a million deep) and `storage_codegen_test` (`musttail` only in `count_down`); corpus 138 passes. Mutations (aggregate arguments admitted, work after the call admitted, the VM emission removed, LLVM limited to the hint) each failed a test, and the million-deep run overflows the VM stack without the emission. Not done: lifetime-based reuse of frame ranges (every slot keeps its own bytes, which is safe and constant in loops but larger than needed); tail calls that pass aggregates or descriptors, which need argument storage that outlives the caller's frame; `uninit[T, N]()` and the partial-construction state are not part of the storage form yet, so the plan sees only their slots; no instruction-level check of the emitted frames.

### T-13 — Prove leak removal and backend parity

**Goal:** Demonstrate both semantic correctness and removal of unwanted allocation.

1. Implement the test matrix below with independent expected values, meaningful
   drop counts, and strict per-case allocation bounds.
2. Run ownership-checked standard-library/CLI regressions as well as differential
   stress tests. Repair invalid stress programs rather than treating checker bypass
   as the contract for the new representation.
3. Tighten corpus 177–179 bounds to remove slice/tuple/sum allowances. Separate
   scheduler/backing allocations from shell allocations; require zero residual
   shell blocks without hiding them behind process-exit arena cleanup.
4. Run debug and release suites with inlining enabled/disabled, then supported
   address/undefined-behavior checks and scheduler ThreadSanitizer checks. Add any
   missing sanitizer configuration explicitly; do not assume one already exists.
5. Record results and remaining unrelated failures. Do not close todo items 17/18
   until ordinary functions, generators, runtime results, and `par`/`race` all pass.

### T-14 — Remove legacy representation and update references

**Goal:** Leave one documented value model with reproducible verification.

1. Remove migrated `is_heap_pointer_value` decisions, pointer-sized nested layout
   fallbacks, old sum slot APIs, obsolete wrapper constructors, and temporary ABI
   adapters. Retain genuine heap-owner operations with explicit ownership names.
2. Update chapters 6, 10, 14–17, 27–28, 38–39, 60, and 65–68 where their representation,
   size queries, runtime results, or optimization descriptions changed. Preserve
   source `copy` classification and accurately describe remaining heap owners.
3. Mark todo items 17 and 18 resolved with links to tests and this design; keep
   unrelated todo entries accurate. Update this plan's task status only from
   completed work and recorded evidence.
4. Update affected Bazel targets and metadata versions. Refresh the context graph
   after the code migration with `graft build` and review the resulting diff.

## New Code

The following names describe the intended implementation boundaries. Add small
files with one clear purpose; use `spec/CONVENTIONS.md` for C++ naming and style.

| Proposed file / existing area | Purpose |
|---|---|
| `src/hir/value_storage.h` | Define typed places, storage classes, initialization/transfer operations, and the prepared storage program. |
| `src/hir/value_lowering.{h,cpp}` | Lower typed HIR into explicit destinations and shared aggregate ABI decisions. |
| `src/hir/value_verify.{h,cpp}` | Verify storage lifetimes, projections, initialization, and call/result contracts. |
| `src/hir/return_storage.{h,cpp}` | Implement shared named-return analysis and result-storage reuse; retain rejection reasons. |
| `src/hir/value_lowering_test.cpp`, `src/hir/return_storage_test.cpp` | Test destinations, ownership-preserving transformations, and NRVO eligibility independently of backend optimizer behavior. |
| `src/runtime/layout.{h,cpp}` and tests | Own complete concrete layout descriptors, checked byte offsets/strides, cycle handling, and alignment. |
| `src/semantic/` type metadata and checked results | Expose resolved member types and semantic passing/copy/drop modes. |
| `src/hir/` existing lowering, reference, capture, suspension, frame, inline, tail-call passes | Integrate places without losing source ownership facts or invalidating analysis. |
| `src/bytecode/`, `src/bytecode_compiler/` | Implement frame places, transfers, aggregate calls, output storage, and verifier metadata. |
| `src/llvm_codegen/` | Implement place emission, internal aggregate ABI, native/JIT wrappers, and low-optimization structural tests. |
| `src/intrinsics.h`, `src/runtime/`, corresponding `src/std/` declarations | Share the new explicit runtime wire contract and aligned storage ownership. |
| `src/comptime_vm/`, `src/driver/` | Prepare the same storage program and marshal hosted/entry results safely. |
| Existing `BUILD.bazel` files and `src/testdata/` suites | Register new source files and semantic, execution, allocation, and optimization regressions. |

## Tests

### Required cases and evidence

| Area | Cases | Required evidence |
|---|---|---|
| Layout | Nested/qualified generic structs, tuples, arrays, sums, packed fields, `int128`, zero-sized members, recursive rejection | Exact sizes/offsets/alignment and expected diagnostics; agree with `size_of`/pointer stride. |
| Basic values | Scalar-only tuple, struct, array, `option`, `result`, payload-free sum loops | Correct values; zero shell heap allocations and zero live shell blocks on both tiers. |
| Slices | Local slicing loops, passing a view, returned view of a borrowed parameter, nested descriptor fields | No descriptor allocations; original referent seen; returned view of a local still rejected. |
| RVO | Direct constructor return, call chains, tail expressions, branches, nested fields, `?`, `or else`, generic and indirect calls | No intermediate aggregate transfers for eligible cases at low optimization with `--no-inline`. |
| NRVO | One mutated return local, repeated returns of that local, whole reassignment | Result destination is the local's only storage; no return transfer; identical drops with NRVO disabled. |
| NRVO fallback | Different return locals, parameter return, partial move, captured local, address exposure, early `?`, return projection | Correct inline moves without shell allocation; inspectable rejection reasons. |
| Aliasing/evaluation | `x = f(&x)`, replacement of a field, borrowed receiver, operand side effects, constructor exits | Exactly-once evaluation in source order; old value dropped only after replacement is ready. |
| Ownership | Non-copy payload, `copy` sum independence, mutable view moves, partial moves, generic borrowed `self` | Checked diagnostics and exactly-once active-member drops; no implicit clone or duplicated owner. |
| Containers/raw memory | `list[(T, U)]`, nested sums, growth/pop/iteration, small-list spill, `shared[T]`, `swap`/`replace` | Inline stride and alignment; expected backing allocations only; each resource drops once. |
| Runtime results | Repeated reads/writes, success/error/propagation/discard, async I/O | No per-operation result wrapper allocation; correct result payload and ownership. |
| Closures/calls | Owned aggregate captures, referenced field, direct/indirect/cross-module/generic calls | No separate captured-value boxes; safe environments and independent by-value parameters. |
| Suspension | Large yielded/task result, borrowed temporary, borrowed member, resumed branch/match subject, worker handoff | Stable addresses, initialized result flags, valid values after intervening calls/polls. |
| Cancellation/concurrency | Never-started task, suspended task, unconsumed finished result, `par`/`race` winners/losers | Exactly-once resource drops after workers finish; no leaked aggregate shells; race checks pass. |
| Hosted/native entry | Compile-time nested results, static reification, JIT aggregate entry, AOT executable | Result storage survives decoding; final bytes/layout agree across all entry paths. |
| Frame/tail calls | Long loops, nested calls/recursion, near/over limit, call with pointers to frame arguments | Bounded frame bytes, useful oversized-frame diagnostic, safe tail-call selection. |

Allocation tests must isolate measured code from setup and existing runtime-owned
backing allocations. Test loop lengths `N` and `10*N` to catch hidden per-iteration
growth. Heap counters alone cannot prove RVO/NRVO: inspect the shared storage plan
and emitted bytecode/LLVM for eliminated local buffers and transfers as well.

Run these existing focused targets during implementation, adding the new HIR test
targets once introduced:

```sh
bazelisk test --config=debug //src/runtime:layout_test //src/runtime:allocator_test //src/runtime:allocator_arena_test //src/runtime:arena_test
bazelisk test --config=debug //src/hir:lower_test //src/hir:inline_test //src/hir:captures_test //src/hir:live_across_yield_test
bazelisk test --config=debug //src/semantic:check_test //src/semantic:ownership_check_test
bazelisk test --config=debug //src/bytecode:vm_test //src/bytecode_compiler:compile_test //src/bytecode_compiler:register_alloc_test
bazelisk test --config=debug //src/llvm_codegen:codegen_test //src/llvm_codegen:aot_test
bazelisk test --config=debug //src:codegen_stress_test //src:std_test //src:cli_test
bazelisk test --config=debug --test_env=CINDER_COMPTIME_VERIFY=1 //src:cli_test
bazelisk test --config=tsan //src/runtime:scheduler_test //src/runtime:reactor_test //src:codegen_stress_test
```

Before completing T-13, run the full suites:

```sh
bazelisk test --config=debug //...
bazelisk test --config=release //...
```

Exercise inlining and NRVO combinations through test-harness preparation options;
`--no-inline` is an existing Cinder option, not a Bazel test flag. Add sanitizer
coverage for native generated code explicitly, including the JIT path where the
chosen tooling supports it; instrumenting only the C++ compiler host does not
automatically instrument generated code.

## Review Checklist

- [x] Scope covers todo items 17 and 18 on both backends and runtime boundaries.
- [x] Value layout is separate from semantic `copy`, moves, borrows, and ABI transport.
- [x] RVO is required in ordinary lowering; NRVO has explicit eligibility and fallback.
- [x] Caller result lifetime, replacement aliasing, partial initialization, and drops
  have concrete rules.
- [x] Containers, compile-time execution, captures, arbitrary-size coroutine results,
  cancellation, alignment, frame limits, and tail calls are included.
- [x] Existing evidence and proposed APIs are distinguished; references use files
  present in this checkout rather than absent overview documents.
- [x] Task dependencies, acceptance criteria, files, tests, and completion gates are
  specified without requesting further design approval.
- [x] Remaining repository inventories are assigned to T-01 with fixed design
  defaults; no unresolved inventory is silently assumed to be complete.
- [x] Complete T-01 inventories and record actual baseline results before coding
  (see [value-semantics-t01-findings.md](value-semantics-t01-findings.md)).
- [x] Implement T-02 (concrete recursive byte layouts).
- [x] Implement T-03 (shared storage form and aggregate ABI).
- [x] Implement T-04 (ownership, references, and drops) for straight-line code.
- [x] Implement T-04a (structured control flow in the storage form).
- [ ] Implement T-05 through T-14 and record passing verification before closing
  the corresponding todo items.
