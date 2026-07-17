# Modules as Compile-Time Values Implementation Plan

Implements the design from `kira-reference.md` (§Modules as Compile-Time
Values): `signature` declarations, structural signature satisfaction,
parameterized modules (functors) with `use audited[postgres] as db`
instantiation, module reflection (`M.functions()`/`M.types()`/`M.name()`),
and compile-time module selection via `static if`. Written 2026-07-12
against the state of the tree at that date.

Should be implemented by Opus.

## Status (updated 2026-07-17)

- **Phase 0 — done.** `spec/module-values-design.md` written; grammar EBNF
  updated (`signature_decl`, parameterized `sub_module_decl`, argumented
  `use_path`).
- **Phase 1 — done.** `signature` keyword/token, `signature_decl` AST node,
  `type_params` on `sub_module_decl` (`is_functor()`), `instantiation_args` on
  `use_decl`, parser support for all three forms, and parser tests
  (`src/parser/parser_test.cpp`).
- **Phase 2 — done.** `signature_symbol` symbol kind; signatures/functors
  indexed in `program_index` (`signatures`/`functors` maps) and skipped by the
  module graph (`resolution.cpp`, `module_index.cpp`) and by ordinary body
  checking (`check.cpp`); `check_signature_decl` well-formedness; a structural
  `module_satisfies_signature` check driven at every `use m[args]` site
  (`check_functor_instantiation`) with concept-quality diagnostics. Tests in
  `src/semantic/check_test.cpp`. **Deep parameter/return-type equality under
  the abstract-type binding is deferred to Phase 7** — v1 checks member
  existence, visibility, and function arity.
- **Phase 3 — not started (the remaining core).** No functor *materialization*
  yet: a satisfied `use m[args]` reports "elaboration pending" rather than
  cloning + substituting the functor body into a usable synthetic module.
  Everything below from here is future work.

## Where the compiler stands today

Per `spec/todo.md` item 1, this is greenfield — but less greenfield than it
sounds. Foundations that exist and carry real weight:

- **The module graph and symbol machinery.** Cross-file modules,
  string-keyed module paths, per-module symbol tables
  (`semantic_symbol`, namespaced), visibility enforcement, `use`
  resolution with selectors (`src/semantic/module_index.h`,
  `analysis.cpp`/`resolution.cpp`). A `sub_module_decl` AST node already
  models a module with a body.
- **Compile-time evaluation.** `static if` / `static for` /
  `static assert` / `static` bindings parse (`parser.cpp:2258`) and
  evaluate (`src/comptime/eval.*`, wired into the checker at
  `check.cpp:2687`–`2749`, `7623`).
- **Reflection over types** exists (`src/comptime/reflect.cpp`) as a
  read-only traversal of resolved AST declarations — the pattern module
  reflection will copy.
- **Synthetic-item injection precedent.** Splice-synthesized impls are
  injected into coherence checking and method tables (`check.cpp:8151`
  area) — the same mechanism an instantiated functor body needs.

What does not exist at all:

- No `signature` keyword, token, grammar, AST node, or symbol kind.
- No module parameters (`module audited[DB: backend]:`) — `module` is a
  bare file-level path line; the parameterized, body-carrying form is new
  surface.
- `use` takes no type/module arguments (`use audited[postgres] as db`
  does not parse; check whether the existing `use_selector`'s `as` alias
  survives the new form).
- No notion of a module as a value anywhere in `src/comptime/` — no
  module descriptors, no `M.functions()`.

## Strategy

Four decisions that shape everything below (Phase 0 writes them down as
the design doc this feature codes against):

1. **Applicative functors, memoized per argument tuple.** `audited[postgres]`
   names *the same* module everywhere it appears — instantiation is a pure
   function of (functor, args), cached session-wide. This is required by
   the confluence rule (§Compile-Time Semantics: results never depend on
   elaboration order) and keeps type identity sane: two files that both
   `use audited[postgres]` see one `conn`, not two.
2. **Structural signature satisfaction, concept-style.** No `impl`; a
   module satisfies a signature iff it provides each required member with
   a matching type, where the signature's abstract types (`type conn`)
   bind to the module's concrete ones during the check. `DB.conn` in a
   functor body is a *projection* that substitutes to the argument's
   concrete type at instantiation.
3. **Instantiate by substitution, check concretely (v1).** A functor body
   is parsed and name-resolved once, but *type-checked per instantiation*
   after substituting the module arguments — monomorphization-style, using
   the trait-default-cloning / splice-injection precedent. Errors point at
   the functor body with a note chaining to the instantiation site.
   Checking the body *abstractly* against the signature (catching errors
   before any instantiation exists) is a later hardening phase, not v1 —
   the same sequencing the trait system used.
4. **Modules are never values at runtime — and never values in the
   checker's expression language either (v1).** `let m = audited[postgres]`
   is out of scope; the spec says "you rarely bind one to a variable" and
   the only binding forms implemented are `use ... as` and module
   parameters. Reflection calls (`M.functions()`) evaluate in the comptime
   evaluator only. This keeps modules out of `type_table` entirely.

Every phase ends with regression tests (parser, `analysis_test`/
`resolution_test`/`check_test`, stress corpora) and a warning-free build;
diagnostics follow the compiler-is-a-teacher standard.

---

## Phase 0 — `spec/module-values-design.md`

Small but load-bearing: the four strategy decisions above, the grammar
additions (EBNF for `signature`, parameterized `module`, argumented `use`),
signature-matching rules (member kinds, visibility, abstract-type binding),
instantiation identity/memoization, and — most importantly — the **pipeline
ordering rule** for Phase 6: which compile-time expressions may gate a
`use` (see Phase 6 for why this is the hard constraint). Update
`kira-grammar.ebnf` alongside.

## Phase 1 — Grammar and AST

- `signature` keyword + `signature_decl` AST node: named, indented body of
  member requirements — `type` (abstract, optionally bounded), `def`
  signatures (no body), `static` constants (name + type). Reuse the
  existing `kw_intrinsic`-style "declaration without body" parsing where
  it fits.
- Parameterized module declarations: `module name[P: bound, ...]:` with an
  indented body — extends `sub_module_decl` (params + the existing body
  shape) rather than inventing a new node. Params reuse `ast::type_param`;
  a signature bound is a `named_type` resolved later to a signature.
- `use path[args] as name`: extend `use_decl` with optional
  `std::vector<ptr<type_expr>>`-style arguments (reusing the type-arg
  grammar so nested instantiations `audited[cached[postgres]]` parse) and
  confirm/add the `as` alias in the selector.
- Error recovery (`error_node` + resynchronization at DEDENT/keywords) and
  parser tests for every new form, malformed variants included.

## Phase 2 — Signatures in the semantic model

- New symbol kind for signatures in the types/traits namespace; register
  in module-scope symbol building (`symbols.cpp`, `module_index.cpp`);
  name resolution for signature references in bounds.
- Check signature bodies: member signatures well-formed; an abstract
  `type conn` is usable by later members of the same signature (an opaque
  type local to the signature — model on `type_param_kind`).
- **Structural satisfaction check** `satisfies(module, signature)`:
  bind each abstract type to the module's same-named type, then require
  each `def`/`static` member to exist, be visible (`pub`), and match by
  interned type equality under that binding. Produce concept-quality
  diagnostics: list *every* missing/mismatched member with expected vs
  found, not just the first.
- Testable standalone via an internal assertion form or by wiring the
  Phase 3 bound check first — either way, `check_test.cpp` coverage for
  satisfaction and each failure mode.

## Phase 3 — Parameterized modules and instantiation

The core phase.

- Registration: a parameterized module enters the module graph as a
  *functor* — visible by name, not elaborated, its body skipped by normal
  per-module checking.
- `use audited[postgres] as db` elaboration: resolve the functor and each
  argument (a concrete module, an instantiation, a type, or a static
  value per the param kinds); check each module argument `satisfies` its
  signature bound (Phase 2); then **clone the functor body substituting
  params** — `DB.query(...)` → `postgres.query(...)`, `DB.conn` →
  postgres's `conn` — and inject the result as a synthetic module keyed
  `audited[postgres]`, memoized session-wide (Strategy #1) so repeated
  instantiations share one identity. Aliased into scope as `db`.
- Instantiation-time checking (Strategy #3) with error notes chaining to
  the `use` site. Cycle detection: a functor whose body instantiates
  itself (directly or through another functor) gets a cycle diagnostic,
  reusing the `statics_in_progress_`-style in-progress set
  (`check.cpp:943`).
- Nested instantiation (`audited[cached[postgres]]`), functors over types
  and static values (not just modules), and `pub use` re-export of an
  instantiation each get stress cases.

## Phase 4 — Module reflection

- Extend `src/comptime/reflect.cpp` from type reflection to module
  reflection: `M.name()`, `M.functions()`, `M.types()` returning
  descriptor values (name, signature info) in the evaluator's value model,
  built by read-only traversal of the resolved module's symbols.
  Visibility-aware per spec: outside the module, `pub` members only;
  inside, everything.
- Works uniformly on ordinary modules, instantiated functors, and module
  *parameters inside a functor body* (where members are known only at
  instantiation — reflection there evaluates per-instantiation, which
  Strategy #3's checking model already provides).
- The spec's command-table example (`static COMMANDS = map(for f in
  cli_commands.functions() => ...)`) becomes an end-to-end fixture:
  reflection → `static for` → reified static, executed on both backends.

## Phase 5 — Metadata and separate compilation

- `src/module_metadata.proto`: serialize signatures, functor declarations
  (parameter kinds + bounds), and instantiation keys so a package can
  export a functor and a dependent package can instantiate it. Memoization
  keys must be stable across compilation units (the canonical instantiation
  key string from Phase 3).
- Round-trip tests through the CLI driver's metadata path
  (`--metadata-dir`).

## Phase 6 — Compile-time module selection (`static if` around `use`)

Deliberately last among the semantic phases because it bends the pipeline:
today the module graph and `use` resolution run *before* type checking and
compile-time evaluation; a `static if BUILD.test: use fake_io as io` makes
the graph depend on evaluation.

- Scope the condition language per the Phase 0 design doc: conditions on
  imports may reference only *early-evaluable* statics — literals, build
  flags (`BUILD.*`), and statics whose initializers need no name
  resolution beyond the prelude. That fragment can be evaluated in a
  pre-resolution pass without circularity, keeps confluence trivially, and
  covers the spec's motivating example. Richer conditions diagnose with an
  explanation of the restriction.
- Implement as a small pre-resolution evaluation step in the driver
  pipeline (`src/driver/lowering_stage.cpp` siblings): fold the taken
  branch's items into the file's item list, drop the untaken branch
  (unparsed-beyond-syntax, per `static if` semantics), then build the
  module graph as today.
- Both-branches fixtures (`fake_io`/`real_io` satisfying one signature)
  under a build flag the CLI can set.

## Phase 7 — Hardening and polish

- Abstract pre-checking of functor bodies against their signature bounds
  (catch errors with zero instantiations — the deferred half of
  Strategy #3), reusing whatever the HKT plan built for checking bodies
  under abstract parameters if it landed first.
- Interactions each with a test or an explicit deferral diagnostic:
  functors + `deriving`; impls declared inside functor bodies (coherence
  keys must include the instantiation key, or orphan-rule violations slip
  through — this one is a *test now, in Phase 3*, not deferred);
  reflection over functors themselves; `project.kira` using signatures.
- Spec examples verbatim as fixtures; a runnable `demo/` showing the
  `backend`/`audited` pipeline end-to-end.

---

## Sequencing and risk

Dependency chain: 0 → 1 → 2 → 3 → (4, 5, 6 in any order) → 7. The feature
is real after Phase 3; Phases 4–6 each unlock one spec section
independently. No coupling to the concurrency plan; mild coupling to the
HKT plan (both check bodies under abstract parameters — share the
approach) and to the dependent-types plan only via general
`resolve_type_args` churn.

Highest-risk items:

1. **Pipeline ordering** (Phase 6, decided in Phase 0) — the module graph
   becoming evaluation-dependent is the one place this feature can
   destabilize the whole pipeline. The early-evaluable fragment keeps it
   a bounded pre-pass; resist any "just evaluate a bit more" widening.
2. **Type identity across instantiations** (Phase 3) — `DB.conn`
   projections must substitute to the *same interned `type_id`* as the
   argument module's type, and memoization must guarantee one identity
   per instantiation key, or two imports of `audited[postgres]` produce
   incompatible `conn`s. Property-test; respect the `type_table`
   `std::deque` invariant (memory: type-table-deque-fix).
3. **Coherence inside functors** (Phases 3/7) — impls in instantiated
   bodies enter session-wide coherence once per instantiation; the
   coherence key must incorporate the instantiation or duplicate-impl
   errors appear (or worse, don't). Test in Phase 3.
