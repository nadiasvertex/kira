# Kira Phase 3 Plan

This document turns the phase-3 roadmap into an implementation backlog tied to
the current repository layout.

It reflects the following project decisions:

- Phase 3 should end with a real semantic-analysis subsystem, not more logic
  embedded in `src/cli.cpp`.
- Type inference should support true generic generalization.
- Phase 3 should include `match` exhaustiveness and unreachable-arm checking.
- Typed IR still belongs to phase 4. Phase 3 should analyze the parser AST
  directly and keep typed IR as a separate later boundary.

## Goals

By the end of phase 3, the compiler should be able to:

- build symbol tables and lexical scopes
- resolve names across modules and local scopes
- infer and type-check core expressions, functions, and patterns
- infer omitted top-level function signatures with rank-1 generic
  generalization
- reject non-exhaustive `match` expressions and unreachable arms
- validate generics, `where` clauses, traits, impls, associated types,
  concepts, and contracts at the semantic level
- produce specific, teaching-oriented diagnostics

## Non-Goals

The following should not block phase 3:

- typed HIR or MIR
- LLVM lowering
- borrow checking or ownership enforcement beyond basic type-shape handling
- a finalized runtime ABI
- full dependent-type or refinement-proof solving
- full CTFE beyond whatever later phases need

## Current Repository State

Today, the semantic entry point is still the driver in `src/cli.cpp`.

Implemented there now:

- session module graph validation
- session-local import validation
- duplicate module-scope declaration checks
- qualified-path validation for a narrow subset of type and module references

Missing there now:

- dedicated semantic library
- lexical value-level name resolution
- local scope tracking
- semantic type representation
- unification and constraint solving
- inferred types for AST nodes
- function signature inference
- pattern typing
- exhaustiveness analysis
- trait and impl validation beyond path resolution

Relevant existing files:

- `src/cli.cpp`: current driver and semantic logic
- `src/cli.h`: driver API
- `src/k-parser/ast.h`: parser AST surface for phase 3
- `src/cli_test.cpp`: end-to-end driver tests
- `src/driver_stress_test.cpp`: parser-driver corpus smoke test
- `src/BUILD.bazel`: current top-level build layout

## Architectural Direction

Phase 3 should introduce a new semantic subsystem under `src/semantic/`.

The driver should become an orchestrator:

1. parse source files
2. call semantic analysis
3. render diagnostics
4. emit metadata only for files that survive semantic analysis

The semantic subsystem should be structured as explicit subpasses rather than
one monolithic walk.

Recommended semantic pipeline:

1. declaration indexing
2. module and lexical scope construction
3. name resolution
4. type expression lowering into semantic types
5. constraint generation and unification
6. function and expression inference
7. pattern typing
8. exhaustiveness and usefulness checking
9. generic bound, trait, impl, associated-type, concept, and contract
   validation

## Type System Strategy

### Core Model

Introduce a semantic type arena with stable `type_id` handles.

The type model should include at least:

- primitive types
- `unit`
- `never`
- error type
- inference variables
- tuple types
- function types
- array, slice, reference, and pointer types
- nominal type applications
- type parameters
- associated type projections

### Generic Generalization

True generic inference should use qualified rank-1 polymorphism.

Represent inferred and declared polymorphic types as schemes:

- `forall vars. obligations => type`

Required behaviors:

- instantiate schemes on lookup
- generalize immutable `let` bindings when allowed
- generalize top-level functions after inference converges
- do not generalize `var`
- treat polymorphic recursion as requiring explicit annotations

Because Kira already has `var` and plans `&mut`, phase 3 should use a value
restriction or relaxed value restriction instead of unrestricted let
polymorphism.

### Top-Level Function Inference

Omitted top-level function signatures should be inferred across the call graph,
not only within one body.

Implementation rule:

- infer top-level functions by strongly connected component

This allows:

- intra-module and cross-session function references within the current compile
  session
- mutually recursive inference within one SCC when monomorphic during the
  inference step

This does not imply inferred polymorphic recursion.

### Trait and Operator Model

Operator typing should be trait-driven from the start.

Even if some operations are initially backed by built-in rules, the semantic
model should still expose them as prelude-provided capabilities so the later
trait system does not force a rewrite of the inferencer.

## Pattern Typing and Coverage

Pattern typing should be a first-class semantic pass.

Supported pattern forms for phase 3:

- wildcard
- binding
- literal
- tuple
- constructor
- struct
- array
- range
- option/result wrappers
- ref pattern
- `or`
- grouping and aliases

Pattern typing should produce:

- scrutinee constraints
- bound names and their semantic types
- diagnostics for incompatible pattern forms

Coverage analysis should be a separate pass that runs after pattern typing.

Phase 3 should include diagnostics for:

- non-exhaustive `match`
- unreachable arms
- redundant alternatives inside `or` patterns when practical

Coverage should be exact where practical for:

- sum types
- `bool`
- `option`
- `result`
- tuples composed from finite spaces

Coverage may be conservative for open domains like:

- `str`
- `float*`
- user-defined spaces that are not finitely enumerable

In conservative cases, `_` should remain the way to prove exhaustiveness.

## Proposed File Layout

Add a new directory and Bazel target:

- `src/semantic/BUILD.bazel`
- `src/semantic/analysis.h`
- `src/semantic/analysis.cpp`
- `src/semantic/session.h`
- `src/semantic/session.cpp`
- `src/semantic/module_index.h`
- `src/semantic/module_index.cpp`
- `src/semantic/symbols.h`
- `src/semantic/symbols.cpp`
- `src/semantic/scopes.h`
- `src/semantic/scopes.cpp`
- `src/semantic/types.h`
- `src/semantic/types.cpp`
- `src/semantic/schemes.h`
- `src/semantic/schemes.cpp`
- `src/semantic/constraints.h`
- `src/semantic/constraints.cpp`
- `src/semantic/resolution.h`
- `src/semantic/resolution.cpp`
- `src/semantic/infer.h`
- `src/semantic/infer.cpp`
- `src/semantic/patterns.h`
- `src/semantic/patterns.cpp`
- `src/semantic/coverage.h`
- `src/semantic/coverage.cpp`
- `src/semantic/traits.h`
- `src/semantic/traits.cpp`
- `src/semantic/contracts.h`
- `src/semantic/contracts.cpp`

Recommended test files:

- `src/semantic/semantic_test_support.h`
- `src/semantic/resolution_test.cpp`
- `src/semantic/type_check_test.cpp`
- `src/semantic/inference_test.cpp`
- `src/semantic/pattern_test.cpp`
- `src/semantic/coverage_test.cpp`
- `src/semantic/traits_test.cpp`

This exact file split may change slightly during implementation, but the
separation of concerns should stay intact.

## Driver Integration Plan

`src/cli.cpp` should stop owning semantic details directly.

Driver-side responsibilities should become:

- file loading
- source manager construction
- lexer and parser invocation
- one call into semantic analysis
- rendered diagnostics and summary reporting
- metadata emission for surviving modules

`src/cli.h` likely does not need major API changes yet. The public surface can
stay at `compile_sources()`, while the semantic library remains an internal
dependency of `//src:cli`.

## Metadata Policy

`src/module_metadata.proto` should remain mostly parser-oriented during early
phase 3.

Do not block type-checking work on immediate metadata expansion.

Only extend metadata once one or more of these stabilize:

- exported semantic signatures
- resolved imports or symbol references
- semantic error summaries worth persisting

Any metadata expansion must stay additive.

## Implementation Backlog

The work below is ordered as a recommended sequence of PR-sized milestones.

### Milestone 1: Extract Existing Semantics

Status: completed

Purpose:

- create `src/semantic/`
- move current module/import/path validation out of `src/cli.cpp`
- preserve existing behavior and diagnostics

Files to add:

- `src/semantic/BUILD.bazel`
- `src/semantic/analysis.h`
- `src/semantic/analysis.cpp`
- `src/semantic/module_index.h`
- `src/semantic/module_index.cpp`
- `src/semantic/resolution.h`
- `src/semantic/resolution.cpp`

Files to edit:

- `src/BUILD.bazel`
- `src/cli.cpp`

Tasks:

- move `module_session_index` and related helpers
- move `semantic_resolution_index` and related helpers
- move duplicate declaration-scope checks
- move current qualified-path validation walker
- keep diagnostics text unchanged so current tests stay valid

Tests:

- keep `src/cli_test.cpp` green
- keep `src/driver_stress_test.cpp` green
- add `src/semantic/analysis_test.cpp` for direct semantic-regression coverage

Exit criteria:

- no intended user-visible semantic behavior change
- `src/cli.cpp` becomes smaller and delegates semantic work

Completed work:

- added `//src/semantic:semantic`
- routed `src/cli.cpp` through `semantic::validate_semantics(...)`
- preserved the existing end-to-end driver behavior and diagnostics
- added direct semantic regression tests covering duplicate modules, parent
  module boundaries, unresolved imports, duplicate declaration scopes, and
  unresolved qualified paths

### Milestone 2: Introduce Symbol and Scope Infrastructure

Purpose:

- replace ad hoc module records with real symbols and scopes

Files to add:

- `src/semantic/symbols.h`
- `src/semantic/symbols.cpp`
- `src/semantic/scopes.h`
- `src/semantic/scopes.cpp`
- `src/semantic/session.h`
- `src/semantic/session.cpp`
- `src/semantic/resolution_test.cpp`

Files to edit:

- `src/semantic/analysis.cpp`
- `src/semantic/resolution.cpp`

Tasks:

- add stable IDs for symbols and scopes
- add separate namespaces for module/type names and value names where needed
- build module scopes for files and inline submodules
- build lexical scopes for functions, blocks, lambdas, `where`, loops, and
  match arms
- support local shadowing rules for immutable bindings
- resolve local identifiers and preserve current qualified-path support

Tests:

- new unit tests for scope lookup and shadowing
- keep current end-to-end driver tests green

Exit criteria:

- local name resolution exists as an explicit semantic concept
- current path validation is implemented on top of the new symbol model

### Milestone 3: Add Semantic Types, Schemes, and Unification

Purpose:

- establish the minimum type machinery required for inference

Files to add:

- `src/semantic/types.h`
- `src/semantic/types.cpp`
- `src/semantic/schemes.h`
- `src/semantic/schemes.cpp`
- `src/semantic/constraints.h`
- `src/semantic/constraints.cpp`
- `src/semantic/type_check_test.cpp`

Tasks:

- add a type arena and `type_id`
- add primitive and prelude types
- add inference variables
- add substitutions and unification
- add an error type for cascade suppression
- add literal typing and defaulting rules
- add scheme instantiation and generalization primitives

Tests:

- direct unit tests for type unification
- direct unit tests for scheme instantiation and generalization
- tests for numeric literal defaulting

Exit criteria:

- semantic code can represent inferred and polymorphic types cleanly

### Milestone 4: Core Expression and Statement Type Checking

Purpose:

- type-check ordinary function bodies and local code

Files to add:

- `src/semantic/infer.h`
- `src/semantic/infer.cpp`
- `src/semantic/semantic_test_support.h`
- `src/semantic/inference_test.cpp`

Files to edit:

- `src/semantic/analysis.cpp`
- `src/semantic/resolution.cpp`

Tasks:

- type-check identifiers and literal expressions
- type-check unary and binary expressions
- type-check calls and returns
- type-check `let`, `var`, and assignment
- type-check blocks, `if`, and `if` expressions
- type-check tuples and arrays
- type-check lambdas
- type-check casts and `try`
- type-check `where_expr`
- infer local bindings

Diagnostics to implement:

- unknown name
- type mismatch
- not callable
- wrong argument count
- named argument mismatch
- assignment to immutable binding
- return type mismatch

Tests:

- successful local inference cases
- failing local mismatch cases
- return checking cases
- immutability checking cases

Exit criteria:

- the compiler can reject ill-typed local code in function bodies with useful
  diagnostics

### Milestone 5: Top-Level Function Inference and Generic Generalization

Purpose:

- infer omitted top-level function signatures with real generic schemes

Files to edit:

- `src/semantic/infer.cpp`
- `src/semantic/analysis.cpp`
- `src/semantic/symbols.cpp`
- `src/semantic/inference_test.cpp`

Tasks:

- build a top-level function dependency graph
- compute strongly connected components
- infer SCCs together before generalization
- generalize top-level functions into rank-1 schemes
- instantiate inferred function schemes at call sites
- require annotations for polymorphic recursion
- apply the chosen value restriction policy to `let` and `var`

Tests:

- infer monomorphic top-level functions
- infer generic identity-like functions
- infer generic higher-use functions within rank-1 limits
- reject unsupported polymorphic recursion with explicit diagnostics
- verify value restriction behavior

Exit criteria:

- omitted top-level function signatures can infer to stable semantic schemes

### Milestone 6: Nominal Types, Constructors, and Fields

Purpose:

- make user-defined types usable in expression and pattern typing

Files to edit:

- `src/semantic/types.cpp`
- `src/semantic/resolution.cpp`
- `src/semantic/infer.cpp`
- `src/semantic/type_check_test.cpp`

Tasks:

- lower `type_decl` definitions into semantic nominal forms
- support aliases, structs, and sum types
- add constructor symbols for sum variants
- add field lookup for structs
- type-check struct literals and field access
- type-check constructor calls and constructor-based patterns

Tests:

- struct field access success and failure
- struct literal shape mismatch
- sum constructor arity mismatch
- constructor resolution from local and qualified contexts

Exit criteria:

- user-defined nominal types participate fully in phase-3 type checking

### Milestone 7: Pattern Typing

Purpose:

- type-check the full core pattern surface

Files to add:

- `src/semantic/patterns.h`
- `src/semantic/patterns.cpp`
- `src/semantic/pattern_test.cpp`

Files to edit:

- `src/semantic/infer.cpp`

Tasks:

- type-check binding, wildcard, and literal patterns
- type-check tuple, struct, and array patterns
- type-check constructor, option, and result patterns
- type-check `ref` and `or` patterns
- bind pattern locals into branch scopes
- support `if let`, `while let`, `let ... else`, `for`, and `match`

Tests:

- pattern-bound variable typing
- constructor pattern mismatch
- invalid field pattern
- inconsistent `or` pattern bindings

Exit criteria:

- patterns are semantically typed rather than only traversed syntactically

### Milestone 8: Exhaustiveness and Usefulness Checking

Purpose:

- deliver the rest of the requested match-analysis behavior for phase 3

Files to add:

- `src/semantic/coverage.h`
- `src/semantic/coverage.cpp`
- `src/semantic/coverage_test.cpp`

Files to edit:

- `src/semantic/patterns.cpp`
- `src/semantic/infer.cpp`

Tasks:

- lower typed patterns into a coverage-oriented representation
- implement usefulness checking for new arms
- implement exhaustiveness checking for completed matches
- produce missing-case diagnostics
- produce unreachable-arm diagnostics
- handle finite-domain cases precisely where practical

Tests:

- missing variant cases for sum types
- missing `true` or `false`
- redundant wildcard after exhaustive coverage
- redundant constructor arm after earlier catch-all

Exit criteria:

- non-exhaustive and unreachable match behavior is enforced in phase 3

### Milestone 9: Generic Bounds and `where` Clauses

Purpose:

- connect generic syntax to semantic obligations

Files to edit:

- `src/semantic/schemes.cpp`
- `src/semantic/constraints.cpp`
- `src/semantic/infer.cpp`
- `src/semantic/type_check_test.cpp`

Tasks:

- validate generic arity and argument forms
- lower parameter bounds into obligations
- lower `where_constraint`s into obligations
- validate named and value generic arguments
- report unsupported or malformed generic uses precisely

Tests:

- generic arity mismatch
- missing bound satisfaction
- malformed associated-type-style constraints

Exit criteria:

- generic bounds participate in semantic analysis instead of only path checks

### Milestone 10: Traits, Impls, and Associated Types

Purpose:

- complete the major semantic structures promised in phase 3

Files to add:

- `src/semantic/traits.h`
- `src/semantic/traits.cpp`
- `src/semantic/traits_test.cpp`

Files to edit:

- `src/semantic/symbols.cpp`
- `src/semantic/types.cpp`
- `src/semantic/infer.cpp`

Tasks:

- build trait symbol tables and member signatures
- validate `requires` relationships
- validate impl headers and target types
- validate associated type declarations and definitions
- detect duplicate or conflicting impls
- support method and associated-type lookup through semantic tables

Tests:

- malformed trait item shapes
- impl target mismatch
- duplicate impl detection
- associated type omission or wrong definition

Exit criteria:

- trait and impl semantics are validated as real phase-3 work, not deferred

### Milestone 11: Concepts and Contracts

Purpose:

- finish the remaining phase-3 semantic surface

Files to add:

- `src/semantic/contracts.h`
- `src/semantic/contracts.cpp`

Files to edit:

- `src/semantic/analysis.cpp`
- `src/semantic/infer.cpp`
- `src/semantic/traits.cpp`

Tasks:

- validate concept declarations structurally
- type-check concept constraints
- type-check contract condition expressions as `bool`
- validate purity-related requirements as far as the semantic model supports
- keep deeper proof obligations conservative until later phases

Tests:

- non-boolean contract condition
- malformed concept constraint
- basic concept-bound validation failures

Exit criteria:

- contracts and concepts are semantically validated within phase 3

## Testing Strategy

End-to-end driver tests should remain in `src/cli_test.cpp`, but most new
phase-3 work should be covered by focused semantic unit tests.

Recommended testing split:

- `src/cli_test.cpp`: high-level compile-session regressions
- `src/driver_stress_test.cpp`: broad parser-driver smoke coverage
- `src/semantic/*_test.cpp`: targeted semantic behavior and diagnostics

The semantic test harness should be able to:

- parse one or more in-memory source strings
- run semantic analysis directly
- inspect inferred signatures and node types when needed
- assert on rendered diagnostics when behavior matters to users

## Suggested Build Changes

`src/BUILD.bazel` should gain a semantic library dependency.

Likely shape:

- add `//src/semantic:semantic`
- make `//src:cli` depend on `//src/semantic:semantic`
- add semantic unit-test targets under `src/semantic/BUILD.bazel`

No change is required yet for the public `//src:kira` binary entry point.

## Diagnostics Guidance

Phase 3 diagnostics should follow the language reference philosophy:

- say what was expected
- say what was found
- explain why the constraint exists when useful
- include a likely next action when possible

Specific diagnostics that should be first-class regression targets:

- unknown name
- unresolved qualified path
- duplicate declaration in semantic scope
- type mismatch
- not callable
- wrong argument count
- missing annotation for unsupported inference cases
- invalid pattern for scrutinee type
- non-exhaustive match
- unreachable match arm
- missing trait bound
- conflicting impl

## Deferred Questions

These items may need follow-up design notes during implementation, but they
should not block the first phase-3 milestones:

- exact value-restriction rule for generalized `let`
- whether field and method lookup share one candidate-selection path or two
- whether exported semantic signatures should be serialized in metadata during
  phase 3 or deferred until phase 4
- how aggressively to validate advanced refinement and dependent-type features
  before a fuller solver exists

## Recommended Execution Order

If implementation must proceed in the smallest safe order, use this sequence:

1. semantic extraction
2. symbol and scope infrastructure
3. types, schemes, and unification
4. core expression and statement checking
5. top-level generic inference
6. nominal types, constructors, and fields
7. pattern typing
8. exhaustiveness and usefulness
9. generic bounds and `where`
10. traits, impls, and associated types
11. concepts and contracts

This order keeps the architecture clean while still landing user-visible phase-3
capability early.
