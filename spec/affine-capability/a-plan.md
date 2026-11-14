# Design A: affine capability core plan

Status: proposed design and implementation plan. No task or proof below is complete. This
extracts A from the [affine capability core](../affine-capability-core.md) and the
[ergonomics comparison](ergonomics-comparison.md). It defines implementation and validation
obligations, not a claim that the current checker implements A. Reconcile adoption with
T01/T08 of the [affine roadmap](../affine-capability-implementation-plan.md) and accepted
decisions.

## Goal and boundaries

A makes ownership and authority explicit in a small capability core. Owned values move by
default; inspection, editing, transfer, and permanent freezing are distinct operations.
Regions identify storage, an ownership forest captures exclusive ownership, and loans carry
their granting dependencies. Checked footprints permit field and partition precision while
storage identity and transitions prevent stale paths from authorizing access to replacement
storage.

A prioritizes explicit authority and pinned contracts. Its core includes `own[T]`,
`shared_read[T]`, `inspect[T]`, and `edit[T]`, explicit independent copying, and permanent
freeze. Result origins, stored loans, callable modes, write footprints, and outcome states
are part of the contract model. Inference can reduce annotations, but does not replace
checking the exported contract.

This plan does not include cyclic mutable ownership, arbitrary interior mutation, raw
pointers outside trusted boundaries, concurrent mutation, or implicit shared mutable
ownership. Async/cancellation and compile-time execution need separately defined boundaries
and evidence. Existing compiler support does not waive those boundaries.

## Rules

1. **Values and capabilities.** Owned values move by default. `own[T]` grants exclusive
   authority over a region and its owned descendants; `shared_read[T]` is permanently
   immutable and may duplicate its retained handle; `inspect[T]` is a scoped observation;
   `edit[T]` is an affine exclusive loan. A structural `copy` witness permits explicit
   construction only; it never makes an owner implicitly duplicable.
2. **Regions and places.** A region identifies an allocation. Checked field paths and
   partitions refine footprints without inventing allocation identities. Exclusive
   ownership forms a forest. Overlap is determined by resolved region and footprint, not
   source variable names. Unknown aliasing is conservative.
3. **Loans and reborrows.** Every loan records its target, granting authority, and lifetime
   dependencies. Reborrowing preserves those dependencies and suspends conflicting parent
   access. Moving a handle does not remove its active loan or grant access through a
   suspended parent. Edit loans are affine; inspection duplication retains the same loan.
4. **Carriers and origins.** Every live value slot carries the loans required by its
   contents, including temporaries, arguments, aggregates, results, and closure captures.
   Move, construction, storage, and return preserve dependencies. Contracts state result
   origins and which argument authority may retain loans into destination storage.
5. **Scoped and retained values.** Scoped references and views cannot outlive their origins.
   Persistent frozen handles retain storage independently. Scoped loans cannot be promoted
   implicitly to retained sharing. Tasks, statics, type-erased values, and other boundaries
   reject scoped values unless a separately checked boundary is defined.
6. **Liveness and cleanup.** A loan remains active while any carrier, dependent loan, or
   pending cleanup requires it. Cleanup runs exactly once for each initialized owned
   component and while its required loans remain valid. Drop flags account for conditional
   moves; observable cleanup cannot be moved earlier merely to satisfy a borrow check.
7. **Authority and footprints.** Calls acquire the authority declared by their signatures.
   Write footprints describe effects and invalidate facts for affected storage versions;
   they do not grant authority by themselves. Whole-receiver authority remains whole even
   when the implementation writes one field. Structural transitions such as relocation,
   replacement, and destruction check outstanding loans against old storage.
8. **Contracts and outcomes.** Public contracts cover origins, store flows, callable modes,
   write footprints, transitions, and result outcomes. Unknown contracts use conservative
   defaults. Pending result alternatives retain their possible loans and obligations;
   matching later cannot restore invalidated facts or authority, and errors imply no rollback.
9. **Copying and freezing.** `copy(x)` constructs an equivalent value with independent
   mutable storage when the structural witness and observation authority permit it.
   `copy_owned(h)` materializes represented contents into a fresh owner. Freezing consumes
   mutable ownership and permanently removes edit authority over the frozen region and its
   owned descendants. Neither operation sheds embedded scoped loans or duplicates external
   authority.
10. **Proofs and capabilities.** A proof is tied to the region identity, relevant storage
    version, and checked proposition it establishes. A sealed capability cannot be forged
    or duplicated by safe code, including code in its declaring module. Native authority
    creation belongs to an explicit trusted boundary.

## Deliberate precision and trust limits

- **Explicit authority.** Callers and signatures expose the capability needed for an
  operation. Write annotations refine effect invalidation but cannot weaken a receiver's
  declared access requirement.
- **Storage identity.** Equal syntax paths do not prove equal storage after transitions;
  unequal paths do not prove separate allocations. Region and backing provenance must be
  checked through aliases and ownership edges.
- **Proof invalidation.** A fact survives only while its observed storage version and
  dependencies remain valid. A positional bounds proof can survive element reordering when
  shape is unchanged; a maximum-element claim cannot.
- **Trusted boundaries.** Raw pointers, native operations, and authority minting need
  contracts covering lifetime, aliasing, relocation, initialization, and cleanup. Names,
  private constructors, and annotations are not evidence of those properties.
- **No implicit fallback.** Static ownership rejection does not silently introduce
  reference counting, runtime borrowing, deep copy, freezing, or managed mutation.

## Interfaces and diagnostics

Export complete contracts for separately compiled code: authority requirements, write
footprints, origins, store flows, transitions, outcomes, callable modes, and guarantees.
Check implementations against trait, native, and function-value contracts. Record released
interfaces and compare compatibility against that baseline; local build history is
insufficient. Unknown or stale interfaces must fail conservatively.

Diagnostics should identify the conflicting loan, its carrier and origin, the operation
requiring authority, and any transitive call or cleanup path. For invalidated proofs, report
the storage transition that changed the relevant identity or version.

## Dependency-ordered implementation work

| Task | Depends on | Deliverable and acceptance condition |
|---|---|---|
| A01 | — | Formalize the sequential core, capability conversions, region forest, carrier invariant, cleanup, and failure semantics. State supported and rejected boundaries. |
| A02 | A01 | Prove preservation, progress modulo defined runtime failures, nonconflicting access, permanent freeze, nonescape of dead storage, and sound proof erasure for the core calculus. |
| A03 | A01 | Implement region and footprint checking, reborrows, partitions, structural transitions, and storage-version facts. Reject aliasing and stale-path counterexamples. |
| A04 | A03 | Implement carriers, origin/store-flow contracts, callable modes, and outcome states across calls, closures, generics, and recursion. Check callee bodies against exported contracts. |
| A05 | A04 | Integrate ownership transfer, explicit copy/freeze, partial initialization, drop flags, and exactly-once cleanup across all exits. |
| A06 | A04 | Implement explicit and inferred contract checking, interface serialization/versioning, released-contract compatibility, native boundaries, and diagnostics. |
| A07 | A05, A06 | Port the complete A/B/C application corpus and library baseline. Record annotations, rewrites, copies, checks, runtime costs, and conservative rejections. |
| A08 | A02, A07 | Map formal, implementation, runtime, backend, and adoption evidence to the affine roadmap gates. No production execution path proceeds without its required evidence. |

These tasks are proposed work, not completed gates. The [complete application corpus](applications/README.md)
and the adversarial cases in the [core proposal](../affine-capability-core.md) define validation
inputs. Compare behavior and cleanup paths, not signature counts alone. Distinguish paper
ports, executable checks, and measured runtime costs; do not report speculative results as
evidence.

## Validation and decision gate

Validate conflicting aliases, moved suspended parents, local loan escape, stored loans through
mutable parameters, backing replacement, pending outcomes, error paths, partial moves,
exactly-once cleanup, explicit independent copying, permanent freeze, forged capabilities,
and stale proof rejection. Execute representative programs with allocation and cleanup
failures where relevant. Tests provide evidence but do not replace the soundness argument.

Adoption requires both the formal obligations and roadmap gates to be met. Compare A with
the [B plan](b-plan.md), [C plan](c-plan.md), and complete applications across safety,
programmer work, runtime cost, and implementation complexity. A remains a design candidate
until that evidence exists.
