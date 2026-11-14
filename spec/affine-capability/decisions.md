# Affine Capability Decisions

This ledger records implementation decisions for the
[affine capability plan](../affine-capability-implementation-plan.md).
An accepted decision does not establish a theorem or complete an implementation task.

## Q1 — Formalization toolchain and proof artifacts

Status: accepted on 2026-11-14. Owning task: T01. Dependent tasks: T02–T09.

### Decision

Use Lean 4 with Lake for the sequential core's formalization. Version-controlled `.lean`
definitions and theorem proofs under `spec/affine-capability/formal/` are the authoritative
proof artifacts. Markdown documents explain those artifacts and link requirement IDs to
fully qualified theorem names. Model tests supplement the proofs.

Pin one exact Lean release in `lean-toolchain` and dependency revisions in Lake's manifest.
Toolchain upgrades require a fresh check of all required theorems and their assumptions.
The [formal artifact contract](formal/README.md) defines the planned modules, build target,
checking workflow, and evidence needed at T09. Package bootstrap and exact version selection
remain T01 work; no Lean package or proof is supplied by this decision.

### Rationale and alternative

The initial core is sequential and exposes its regions, loans, carriers, initialization,
cleanup, and effects explicitly. A direct operational semantics with inductive judgments
and state invariants is an appropriate starting point. Lean supports both those judgments
and executable functions whose correspondence to the judgments can be proved. Its kernel
checks proof terms produced by elaboration and automation.

Rocq with Iris is the principal alternative, with relevant ownership and borrowing precedent
in RustBelt. Lean is selected for this direct sequential development. Concurrency and
verification of unsafe library implementations are outside the initial core; this choice
does not claim that they would require the same proof architecture.

### Trust and scope

Lean is the metatheory tool. The planned Cinder `src/proof/` kernel is a separate finite
certificate checker. Define its certificate language and a reference checker in Lean and
prove acceptance soundness under explicit contexts, storage dependencies, and assumptions.
Lean is not a required dependency of the eventual Cinder executable.

Required proofs may depend on Lean's standard foundational axioms, with the actual
dependencies reported. They must not depend on `sorryAx`, project-defined axioms, or axioms
introduced by native evaluation. Native/runtime boundaries appear as explicit contract
hypotheses in theorem statements, with their scope recorded in the requirement ledger.
Do not hide a required safety property in an unproved hypothesis.

The resulting proofs concern the formal core and reference checker. They do not automatically
verify the C++ checker, value-storage pipeline, native implementation, or backends. Those
correspondence obligations remain separate. Cleanup accounting concerns completed exits;
it does not promise termination or eventual cleanup for diverging programs.

### Sources

- [Lean elaboration and kernel checking](https://lean-lang.org/doc/reference/latest/Elaboration-and-Compilation/)
- [Lean axioms and dependency auditing](https://lean-lang.org/doc/reference/latest/Axioms)
- [Lake builds and project checking](https://lean-lang.org/doc/reference/latest/Build-Tools-and-Distribution/Lake/)
- [RustBelt ownership and borrowing formalization](https://plv.mpi-sws.org/rustbelt/)

## Q2 — Ownership default and explicit copying

Status: ownership-default and contract-inference policies accepted on 2026-11-14;
Q2 is partially resolved. Detailed surface spellings remain candidates.
Owning task: T08. Dependent tasks: T30–T36 and the affected library migrations.

### Decision

Surface value passing moves owned values by default. An ordinary owning argument transfers
its value, authority, and cleanup obligations, leaving its source unavailable. The same
transfer policy applies to ordinary by-value bindings, assignment, returns, and ownership
captures. Fresh expression results may initialize their destinations directly.

Scalars, retained frozen handles, and other values with checked duplication rules may
duplicate under those rules. Duplication must preserve retention and loan dependencies.
Having a structural copy witness does not make an owner duplicable and does not authorize
implicit independent copying. Passing ownership, including through an unconstrained generic
parameter, requires no structural copy witness.

Declared inspection and edit parameters select borrowing or reborrowing. In particular, an
existing edit passed to an edit helper reborrows by default so sequential helpers can reuse
the caller's handle. A returned or stored reborrow continues to constrain the original
handle until all dependent carriers retire. Explicit transfer of that edit handle remains
available, schematically `&>e`, and invalidates its source without discarding dependencies.

Independent copy construction and materialization require explicit requests and structural
witnesses. Freezing an owner is explicit as well: a bare owner cannot satisfy a persistent
sharing parameter by implicit freezing or copy-then-freeze. Schematic `publish(&!x)` freezes
and consumes the original; `publish(&!clone(x))` explicitly materializes and freezes a copy,
preserving `x` when both operations are valid. An existing frozen handle can satisfy the
sharing parameter through independent retention.

The formal core records transfer, duplication, independent copy construction, loans, and
freeze explicitly. Defaults belong to surface elaboration and do not create alternative
implicit core transitions. The [design's conversion table](../affine-capability-core.md#capability-requests-and-conversions)
and the [implementation plan](../affine-capability-implementation-plan.md) carry this policy.

### Rationale and alternative

Move-by-default handles resources, closures, and sealed authority without requiring copies
that may be impossible. It keeps allocating, potentially fallible independent construction
visible and aligns owned-value passing with current Cinder's ownership model. Parameter
capabilities provide reuse through scoped loans; explicit copies provide independent values.

Copy-by-default would preserve the caller's source for copyable inputs, but would insert
potentially expensive construction and require witnesses for ordinary generic value passing.
It would still need a separate transfer route for resources. Explicit copy construction
retains the independence guarantees without making it the default operation.

### Contract inference and programmer constraints

Infer sound, conservative effect, result-origin, and outcome contracts from checked source
bodies by default. Public definitions do not require handwritten contracts merely because
they are public. Export the complete checked summaries so callers never need to inspect a
body. Conservative alternatives are valid when they account for all capabilities, retained
loans, initialization, and cleanup; ambiguity does not automatically require an annotation.

Allow programmer-written constraints to pin desired guarantees. Check inferred behavior
against every supplied constraint. Unconstrained inferred summaries may change with their
implementation, requiring dependent code to be rechecked. Recursive groups can infer or
use conservative candidate summaries, with every body verified before acceptance. Opaque
or native boundaries require sufficient explicit contracts or sound interface defaults.

When inference cannot establish a sufficient contract, explain what information is missing,
why the current conservative summary prevents the requested operation, and where the author
can supply the smallest relevant constraint. Show a legal code change when the body cannot
satisfy that constraint. This is part of the compiler-as-a-teacher mission: annotations are
checked promises, not escape hatches from safety rules. Never infer copying, freezing, or
new authority merely to resolve missing contract information.

### Remaining scope and obligations

Q2 still needs final declaration, result-origin, effect, local-builder, conversion, and
explicit-copy spellings, plus the mappings for `self`, `&mut`, views, legacy `copy`, and
legacy `shared`. Operation names and annotations in this decision are schematic. The
existing `copy` concept and the structural independent-copy witness must remain distinct.

T08 must validate the remaining choices with representative translations. Structural copy,
separation, freeze, lifetime, and cleanup proofs remain required even though copying is
explicit. This decision completes no implementation task and changes no current compiler
or normative-language behavior.

## Remaining gates

Q2 remains partially resolved; Q3–Q8 remain unresolved. V0 has no recorded baseline evidence
yet. The accepted decisions do not authorize general language adoption.
