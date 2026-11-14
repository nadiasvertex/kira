# Affine Capability Formal Artifacts

Status: artifact contract selected by [decision Q1](../decisions.md#q1--formalization-toolchain-and-proof-artifacts).
The Lean package, toolchain pin, executable model, and proofs have not been implemented.
T01 remains incomplete; T02–T09 and G0 remain pending.

## Package and module contract

Create a Lake package here, with one exact release in `lean-toolchain`, package configuration
in `lakefile.toml` or `lakefile.lean`, and committed dependency revisions in
`lake-manifest.json`. Dependencies must not float to branch heads during verification.

The planned default library target is `AffineCapability`. Its root module
`AffineCapability.lean` must import every module containing required definitions or theorems.
The following are planned module paths, not existing proof artifacts:

| Module under `AffineCapability/` | Plan task | Responsibility |
|---|---|---|
| `Syntax.lean` | T02 | Core terms, types, distinct identity classes, and judgments |
| `State.lean` | T02 | Heap typing, forests, loans, carriers, versions, and state invariants |
| `Primitives.lean` | T03 | Primitive transitions, construction failures, and commit points |
| `Control.lean` | T04 | Evaluation order, continuations, joins, loops, and cleanup exits |
| `Contracts.lean` | T04 | Calls, effects, origins, and complete outcome alternatives |
| `Safety.lean` | T05 | Preservation, progress modulo stated failures, conflict freedom, permanent freeze, and lifetime safety |
| `CopyCleanup.lean` | T06 | Copy equivalence, source preservation, mutable separation, and completed-exit accounting |
| `Certificate.lean` | T07 | Finite certificate syntax, terminating reference checking, soundness, and dependency preservation |
| `Erasure.lean` | T07 | Correspondence after static evidence erasure, retaining required runtime tests |
| `Model.lean` | T03–T07, T29 | Executable reference transitions and proved correspondence to the declarative semantics |

Split these modules as needed without weakening requirement coverage. Keep the reference
model independent of the later C++ verifier's implementation routines. Executing the model
provides test evidence; the correspondence theorems require kernel-checked proofs.

## Reproducible checking contract

T01 must select a release supporting the following workflow and validate the commands from
this directory. These are planned commands; they have not been run against a package here:

```sh
lake build AffineCapability
lake check
```

The first builds the named umbrella library. The second must rebuild/export and replay all
required modules through Lean's kernel and reject nonstandard axioms. Use Linux CI for the
sandboxed `lake check` workflow. Pin the CI environment and bootstrap toolchain as well as
the package dependencies. A clean verification run must not rely on prebuilt project proof
artifacts. Record the exact supported commands and environment once bootstrap is complete.

Independent checker replay with `lake check --paranoid` may provide additional evidence;
record whether it ran and which checkers accepted. Compiled `.olean` files and exports are
derived artifacts tied to the pinned toolchain. Retain them with verification evidence when
useful, but keep the `.lean` sources and clean reconstruction procedure authoritative.

## Theorems, assumptions, and evidence

The T01 requirement ledger must map every required property to its source rule, expected
behavior, planned task, fully qualified theorem name, hypotheses, and evidence status.
At T09, every required theorem must exist, be imported by the umbrella module, and check.
Review theorem statements and definitions as well as proof acceptance: a kernel check does
not establish that a statement faithfully expresses the proposal.

Audit the transitive axiom dependencies of each required theorem. The permitted foundational
axioms are `propext`, `Classical.choice`, and `Quot.sound`; report which are actually used.
Reject `sorryAx`, project-defined axioms, and native-evaluation axioms. Automation is permitted
when its output checks without adding forbidden assumptions. Use explicit theorem hypotheses
for modeled native contracts, and list the corresponding implementation obligations.

T09 evidence must include:

- The source revision, exact Lean release, dependency manifest, and checking environment.
- Clean build and kernel-check commands, exit results, and logs.
- The complete required-theorem inventory, statements/hypotheses, and axiom reports.
- Requirement coverage, remaining limitations, and native/implementation trust boundaries.
- Separate status for executable-model tests and implementation correspondence evidence.

Missing proofs, failed checks, exhausted checking budgets, or unreviewed assumptions leave
the affected requirement unestablished. No model-test result or compiled executable replaces
the theorem evidence required by T05–T07 and G0.
