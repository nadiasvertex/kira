# Higher-Kinded Traits Implementation Plan

Implements the design from `kira-reference.md` (§Higher-Kinded Traits): traits
over type constructors — `trait functor[F[_]]`, `trait monad[M[_]] requires
functor[M]`, `impl monad for option` — with methods whose signatures apply the
constructor parameter (`F[A]`, `fn(A) -> M[B]`). Written 2026-07-12 against
the state of the tree at that date.

Should be implemented by Opus.

## Where the compiler stands today

Already done:

- **Surface syntax.** `F[_]` parameters parse: `ast::type_param` carries
  `is_higher_kinded` (`src/parser/ast.h:518`); trait decls, `requires`
  clauses, impl blocks, and trait methods with their own generic params
  (`def map[A, B](fa: F[A], f: fn(A) -> B) -> F[B]`) all parse.
- **A trait/impl semantic model exists for kind-`*` types**: session-wide
  impl coherence keyed per (trait, type-declaration) string
  (`check.cpp:3289`, `validate_impl_coherence` at `check.cpp:7873`), method
  tables, trait-default cloning per impl, `requires` handling,
  `bound_trait_ref` (traits referenced by name + args, not interned as
  types — `types.h:64`).
- **Generic instantiation machinery** for ordinary generics
  (`type_param_kind`, `struct_kind`/`sum_kind` "possibly instantiated",
  `builtin_generic_kind` for `list`/`option`/...).

Not done — the actual work (per `spec/todo.md` item 3: "parses, zero
semantic model of type constructors"):

- `is_higher_kinded` is never read outside the parser.
- The type table (`types.h:38`) has no representation for an *unapplied*
  type constructor (`option` as a thing you pass to `monad`) nor for an
  application of a parameter (`F[A]` where `F` is itself a parameter).
- No kind checking anywhere: using `F` bare where a type is needed, or
  applying a kind-`*` param (`T[int32]`), is not diagnosed.
- No inference from `option[int32]` back to `F = option, A = int32`.

## Strategy

Four decisions that bound the feature (state them in the code comments and
diagnostics; they are the difference between a tractable feature and a
research project):

1. **Kinds are arities, nothing more.** A type parameter's kind is `*`
   (ordinary) or an n-ary constructor (`F[_]` is 1-ary, `F[_, _]` 2-ary).
   No curried kinds, no kind polymorphism, no kind inference across
   declarations — the kind is exactly what the declaration syntax says.
2. **Only nominal constructors inhabit higher kinds.** The argument you can
   pass for `F[_]` is a *named* declaration of matching arity: a user
   struct/sum/opaque generic or a prelude generic (`option`, `list`,
   `result` partially?—no: see 3). No lambdas at the type level, no
   partially-applied constructors, no generic aliases as constructor
   arguments in v1. Each restriction gets its own teacher-quality
   diagnostic ("`result` takes 2 type arguments but `F[_]` requires a
   1-argument constructor; wrap it in a 1-parameter type alias— not yet
   supported — or a dedicated struct").
3. **Unification is rigid pattern-matching only.** `F[A] ~ option[int32]`
   solves `F = option, A = int32` by matching the outermost nominal
   constructor. `F[A] ~ int32` fails. Two flexible constructors never unify
   with each other's applications. This sidesteps higher-order unification
   (undecidable in general) entirely; it is what Rust-adjacent designs and
   the spec's examples actually need.
4. **HKTs erase at monomorphization.** By HIR, every `F` is a concrete
   constructor and every `F[A]` a concrete type — backends, layout, and
   both runtimes are untouched. This is a semantic-analysis-only feature
   plus module-metadata plumbing.

Every phase ends with regression tests (`check_test.cpp`,
`semantic_stress/` corpus) and a warning-free build; diagnostics follow the
compiler-is-a-teacher standard (name the kind expected, the kind found, why,
and the fix).

---

## Phase 1 — Kinds in the semantic model

All in `src/semantic/`.

- Add a `kind` (arity: 0 for `*`, n for n-ary constructors) to the
  checker's view of type parameters: thread `ast::type_param::
  is_higher_kinded` (and the underscore count) through scope/symbol
  building (`scopes.cpp`/`symbols.cpp`) and into `type_param_kind` entries
  in the type table.
- **Kind-check every type application** in `resolve_type`/
  `resolve_type_args` (`check.cpp:1524`): applying a kind-`*` name,
  applying an n-ary constructor to m ≠ n args, or using a higher-kinded
  param bare in type position are all errors now (today they fall through
  as `unknown`). Builtin generics get their true arities (`option` 1,
  `result` 2, `list` 1, `array` 2, ...).
- Diagnostics + negative stress cases for each kind error. This phase is
  standalone-shippable: it strictly improves errors for ordinary code.

## Phase 2 — Type constructors and parameter applications in the type table

- Two new `type_kind`s (`types.h:38`):
  - `ctor_ref_kind` — an unapplied nominal constructor (declaration ref +
    arity). This is what `option` means as an argument to `monad[option]`
    or in `impl monad for option`.
  - `param_app_kind` — an application whose head is a type parameter:
    `F[A]`, `M[B]`. Holds head param `type_id` + arg `type_id`s.
- **Substitution normalizes**: substituting `F := option` into
  `param_app_kind(F, [A])` must produce the ordinary interned
  `option[A]` — the same `type_id` ordinary code gets, or id-equality
  silently splits (same canonical-form discipline as the dependent-types
  plan; respect the `type_table` `std::deque` invariant — entries must not
  move).
- Extend `bound_trait_ref` args and `resolve_type_args` to accept a bare
  generic name as a `ctor_ref_kind` argument *only where the target param
  is higher-kinded* (kind-directed resolution — the Phase 1 arity data
  decides whether `option` here means "the constructor" or is a
  missing-arguments error).

## Phase 3 — Trait declarations over constructor parameters

- Check `trait functor[F[_]]` bodies: method signatures may apply `F`;
  method-local generic params (`[A, B]`) interact with the trait's `F`
  correctly; trait-default bodies type-check under an abstract `F` (only
  operations justified by the trait's own surface and `requires` bounds).
- `requires functor[M]` where `M` is higher-kinded: the implied-bounds
  path that already exists for kind-`*` traits extends to constructor
  params — a `monad[M]` context makes `functor[M]`'s methods available.
- Associated types inside HK traits: **defer** (diagnose "not yet
  supported") unless it falls out free — the spec's examples don't use
  them, and they multiply the unification surface.

## Phase 4 — Impls for constructors

- `impl monad for option`: the impl target resolves to a `ctor_ref_kind`;
  arity must match the trait param's kind (diagnose `impl functor for
  result` per Strategy #2).
- **Coherence**: the existing string-keyed per-declaration scheme
  (`check.cpp:3289`) extends naturally — the coherence key for a
  constructor-targeted impl is the constructor's declaration, same as
  today's rule "one impl per (trait, type declaration)". Orphan rule
  unchanged (own the trait or the constructor).
- **Signature conformance**: check each impl method against the trait
  signature under `F := <ctor>`, using Phase 2 substitution-normalization
  (`F[A]` becomes `option[A]` and must equal the impl's written
  `option[A]` by id). Trait defaults clone per-impl exactly as the
  existing `derived_trait_impls_` path does for kind-`*` traits
  (`check.cpp:8151`).

## Phase 5 — Inference, calls, and monomorphization end-to-end

- **Rigid pattern unification** (Strategy #3) in the checker's existing
  inference: calling `bind(ma, f)` with `ma: option[int32]` against
  `bind[A, B](ma: M[A], f: fn(A) -> M[B])` solves `M = option, A = int32`
  from the outermost constructor, then `B` flows from `f` as usual. Method
  syntax (`ma.bind(f)`) resolves through the constructor's impl the same
  way kind-`*` method lookup works today.
- Generic *functions* with their own higher-kinded params
  (`def lift[F[_]: functor, A](...)`) — same machinery, bound-checked
  against the trait/concept.
- **Monomorphization**: instantiation records concrete constructor
  bindings so HIR lowering (`hir/lower.cpp`, `hir/link.cpp`) sees only
  fully-applied types (Strategy #4). Add codegen stress cases proving a
  `functor`/`monad` program runs identically under `interpret` and `aot` —
  the backends should need zero changes; the test is the proof.
- Unsolvable-inference diagnostics: when the pattern match fails, say what
  shape was expected (`M[A]` — "a 1-argument constructor applied to one
  type") and what was found.

## Phase 6 — Surface completeness and polish

- Module metadata (`src/module_metadata.proto`): kinds on exported type
  params, constructor-targeted impls — so HK traits survive separate
  compilation; round-trip tests.
- Interactions, each with at least a stress test or an explicit
  "deferred" diagnostic: HK params in `concept` definitions; `some Trait`
  existentials over HK traits (likely defer); `box[trait]` objects for HK
  traits (defer — object safety for HKTs is a rabbit hole); `deriving`
  unaffected (assert).
- The spec's own examples (`functor`/`monad` for `option`, `pure`/`bind`)
  become a semantic stress fixture verbatim, plus a runnable end-to-end
  demo under `demo/`.

---

## Sequencing and risk

Dependency chain: 1 → 2 → 3 → 4 → 5 → 6. Phases 1–2 are the foundation;
Phase 5 is where the feature becomes usable; nothing here blocks — or is
blocked by — the concurrency plan, and only canonical-form discipline is
shared with the dependent-types plan (they touch the same
`resolve_type_args` seam, so land whichever goes second with care).

Highest-risk items:

1. **Substitution normalization** (Phase 2) — if `F[A]`[F:=option] doesn't
   intern to the exact `type_id` of `option[A]`, id-equality splits and
   every downstream comparison silently breaks. Property-test it across
   all type shapes (nested applications, tuples of applications, fn types
   over applications).
2. **Inference scope discipline** (Phase 5) — rigid pattern matching must
   stay rigid. Any "just this one flexible case" slides toward
   higher-order unification. Extensions (alias constructors, partial
   application) are design-doc revisions, not patches.
3. **Trait-default bodies under abstract `F`** (Phase 3) — checking a body
   where `F` is opaque exercises inference paths built for concrete types;
   expect `k_unknown_type`-unification interactions and keep the "one gap
   doesn't cascade" property intact.
