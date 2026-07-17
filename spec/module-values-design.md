# Modules as Compile-Time Values — Design

This is the Phase 0 design doc called for by
`spec/module-values-implementation-plan.md`. It fixes the decisions the
implementation codes against: grammar, the semantic model of signatures,
structural satisfaction, functor instantiation identity/memoization, and the
pipeline-ordering rule for compile-time module selection. Written 2026-07-17.

## 1. Strategy (the load-bearing decisions)

1. **Applicative functors, memoized per argument tuple.** `audited[postgres]`
   names *the same* module everywhere it appears. Instantiation is a pure
   function of `(functor, canonical-arg-key)`, cached session-wide. Two files
   that both `use audited[postgres]` see one instantiated module, one `conn`.
2. **Structural signature satisfaction, concept-style.** No `impl`. A module
   satisfies a signature iff it provides each required member with a matching
   type. The signature's abstract types (`type conn`) bind to the module's
   concrete same-named types during the check. A functor body's `DB.conn` is a
   *projection* substituted to the argument module's concrete type at
   instantiation.
3. **Instantiate by substitution, check concretely (v1).** A functor body is
   parsed and its declaration registered once, but *type-checked per
   instantiation* after substituting the module arguments — monomorphization,
   reusing the AST-clone precedent (`src/parser/ast_clone.*`) already used for
   const-generic instances and trait defaults. Errors point at the functor body
   with a note chaining to the instantiation site. Abstract pre-checking of a
   functor body against its bounds (errors with zero instantiations) is Phase 7,
   not v1.
4. **Modules are never values — at runtime or in the checker's expression
   language (v1).** `let m = audited[postgres]` is out of scope. The only
   binding forms are `use ... as` and module parameters. Reflection calls
   (`M.functions()`) evaluate only in the comptime evaluator. Modules never
   enter `type_table`.

## 2. Grammar additions

```ebnf
signature_decl
    = [ visibility ] "signature" IDENT ":" NEWLINE
          INDENT { signature_item } DEDENT ;

signature_item
    = "type" IDENT [ ":" bound ] NEWLINE            (* abstract type *)
    | [ visibility ] func_signature NEWLINE          (* def, no body *)
    | [ visibility ] "static" IDENT ":" type_expr NEWLINE ;  (* required const *)

(* parameterized module: params reuse the type_params grammar *)
sub_module_decl
    = [ visibility ] "module" IDENT [ type_params ] NEWLINE
    | [ visibility ] "module" IDENT [ type_params ] ":" NEWLINE
          INDENT { top_level_item } DEDENT ;

(* argumented use: reuses the named-type type-arg-list grammar *)
use_path
    = module_path [ "[" type_arg_list "]" ] "." use_selector
    | module_path [ "[" type_arg_list "]" ] ;
```

`signature` becomes a reserved keyword (`kw_signature`, token added to the
declaration-keyword block). A signature parameter bound on a module parameter
(`module audited[DB: backend]`) is spelled with the ordinary `type_param`
`: bound` syntax — `backend` resolves later, in the module/type namespace, to a
signature (or trait/concept, all of which live there).

`use audited[postgres] as db` combines the arg list with the existing `as`
alias. Nested instantiation `audited[cached[postgres]]` parses because the
argument list reuses `type_arg_list`, whose entries are `type_expr`.

## 3. Signatures in the semantic model

- New symbol kind `signature_symbol` in the module/type namespace
  (`symbol_namespace::module_type_namespace`), registered from
  `module_symbol_spec`. It resolves as a named-type target for use in bounds,
  but is **not** a type (never enters `type_table`).
- A signature body is checked once: each `def`/`static` member's declared
  type must be well-formed under the signature's abstract types. An abstract
  `type conn` is an opaque type local to the signature, modeled on
  `type_param_kind` — usable by later members of the same signature.

### Structural satisfaction — `satisfies(module, signature)`

Bind each abstract signature type to the module's same-named `pub` type, then
require each required member to exist, be `pub`, and match by interned type
equality under that binding:

- required `type T` → module must export a `pub type T`.
- required `def f(params) -> ret` → module must export a `pub def f` whose
  interned signature equals the required one *after* substituting each abstract
  type with its bound concrete type.
- required `static C: T` → module must export a `pub static C` whose type
  equals `T` under the binding.

The diagnostic lists **every** missing/mismatched member (concept-quality), not
just the first: "module `postgres` does not satisfy signature `backend`:" then a
labelled note per failure with expected-vs-found.

**Deep type-equality (landed).** `module_satisfies_signature` checks not just
member existence/visibility/arity but each `def` parameter/return type and each
`static`'s type under the abstract-type binding: the signature's types are
resolved *in the argument module's own resolve context* (quietly), so an
abstract `type conn` binds to the module's concrete same-named `conn`
automatically, and each pair is compared by interned `type_id` (a knowledge gap
— `k_unknown`/`k_error` on either side — never manufactures a false mismatch).

## 4. Functors and instantiation

- A parameterized `module m[P: bound]` registers as a **functor**: visible by
  name in the module/type namespace (kind `submodule_symbol`, flagged as a
  functor), its body *not* elaborated by ordinary per-module checking.
- **Canonical instantiation key.** `audited[postgres]` →
  `"audited[postgres]"`; nested → `"audited[cached[postgres]]"`. Built from the
  functor's fully-qualified name plus each argument's canonical form (a module's
  qualified path, a type's canonical spelling, or a static value's normalized
  literal). This string is the memoization key and the metadata export key
  (Phase 5); it must be stable across compilation units.
- **Elaboration of `use audited[postgres] as db`:** resolve the functor and
  each argument; check each module argument `satisfies` its signature bound
  (§3); if the key is already memoized, reuse the instantiated module; otherwise
  clone the functor body substituting each parameter — `DB.conn` → postgres's
  `conn` `type_id`, `DB.query(...)` → `postgres.query(...)` — inject the clone
  as a synthetic module keyed by the instantiation key, type-check it, and
  memoize. Alias the result into the importing scope as `db`.
- **Type identity.** A `DB.conn` projection substitutes to the *same interned
  `type_id`* as postgres's `conn`; memoization guarantees one module identity
  per key, so two `use audited[postgres]` imports share one `conn`. Respect the
  `type_table` `std::deque` invariant (memory: type-table-deque-fix).

### Materialization mechanism as implemented (v1)

The clone step deliberately does **no AST substitution**. Instead each module
parameter is bound as an **import alias** to its argument module in the cloned
body's file, and projections resolve through the alias:

- Value projections (`DB.query(...)`) already resolved through a `use ... as`
  alias, so they need nothing new.
- Type projections (`DB.conn`) did *not* resolve through an alias, so
  `resolve_named_type` was extended: a multi-segment path whose head is an
  import alias to a session module rewrites to that module's absolute path and
  resolves there. This is generally useful (it fixes alias-qualified type paths
  everywhere), not functor-specific.

The clone exists only to give each instantiation distinct node identity so
`node_types_` and diagnostics stay per-instantiation. `def`, `type`, `static`
(binding), `impl`, `extend`, and `trait` members are all cloned
(`clone_func_decl`, `clone_type_decl`, `clone_static_decl`, `clone_impl_decl`,
`clone_extend_decl`, `clone_trait_decl`); a non-binding `static` form or an
inline submodule still falls back with a clear diagnostic. Types, traits, and
statics are registered into the synthetic module *before* its functions, so a
function's `-> row` return type, a `bump` reference, or a trait bound resolves
against them.

**Ordering for `impl`/`extend`/coherence.** Because `build_method_table` and
`validate_impl_coherence` run *once*, before per-file checking, a functor-body
`impl`/`extend` must be registered before them or it would never enter the
method table or coherence index. So materialization is split: a pre-pass
(`discover_functor_instantiations`, a sibling of the item-splice pass) walks
every file's `use m[args]` and *registers* each instantiation's cloned members
into `program_index` (including `impls`/`extends`); the method-table and
coherence build then see them; and the cloned bodies are *checked* afterward
(`check_pending_functor_bodies`) so a method call inside a body resolves
against the complete table. Coherence keys need no special-casing: the impl's
target type is projected per instantiation (each `use audited[postgres]` vs
`[sqlite]` resolves its local type to a distinct `type_id`/decl), and
memoization means one instantiation = one registration, so two instantiations
implementing a trait for their own local type never collide. The body is
checked with a temporary `use owner.*` wildcard so a name declared alongside
the functor (a sibling `trait`, `type`) still resolves in the synthetic
module's own scope.
The synthetic module is registered in `program_index` under the *sanitized*
instantiation key (non-identifier characters → `_`, since `import_binding::path`
is split/joined on `.`), and the importing file gets an alias binding
(`db` → synthetic module). `record_use_bindings` and the module-graph/import
passes skip instantiation `use`s so only the checker owns them.

A projection through a module parameter that is a *type* (`DB.conn`, or a
`type row = DB.conn` alias whose body is one) resolves in `resolve_named_type`:
a multi-segment path whose head is a whole-module import alias is rewritten to
the aliased module's absolute path and resolved there. The alias' target module
is found directly from the binding's `path` (`import_source_module` returns null
for a whole-module import, since its members are reached by path, so it cannot
be the only source consulted) — without this the projection resolved silently
to `k_unknown`, which a check-only test cannot distinguish from a concrete type
but which starves lowering of a usable type.

**Codegen (wired).** The checker records each cloned `def` as a
`checked_types::functor_instance` (`{decl, owner_module = synthetic module
name}`, owning the clones in `synthesized_functor_nodes`). After the driver
lowers the real source files, `hir::lower_functor_modules` groups those clones
by `owner_module` into one standalone `hir_module` each — named exactly as a
`db.f(...)` call site records its callee's owner — and appends them to the
module set, so ordinary cross-module dispatch links the two. This mirrors the
multi-file idiom where every module is its own top-level file: a materialized
functor instantiation is simply an in-memory module with no source file, and
both backends (bytecode VM and LLVM/AOT) run it unchanged. An instantiated
functor now runs end-to-end, not just type-checks. `def`s and `impl`/`extend`
methods lower to runtime code — a method as `target::method` (via
`functor_instance::impl_target`), the same key its `receiver.method()` call
records, kept distinct across instantiations by the unique synthetic
`owner_module`. `type` members are compile-time, and a scalar `static` is
inlined at each reference (`static_const_values`), so neither needs a
`functor_instance`.
- **Cycle detection.** A functor whose instantiation (directly or transitively)
  requires instantiating itself gets a cycle diagnostic, reusing the
  in-progress-set pattern (`statics_in_progress_`-style, `check.cpp`).
- **Coherence.** An impl declared inside a functor body enters session-wide
  coherence once per instantiation; its coherence key must incorporate the
  instantiation key, or duplicate-impl errors appear (or, worse, orphan
  violations slip through). Tested in Phase 3.

## 5. Reflection (Phase 4) — landed

`src/comptime/reflect.cpp` extends from type reflection to module reflection:
`M.name()`, `M.functions()`, `M.types()`, `M.function_count()`, and
`M.type_count()` return descriptor values built by a read-only traversal of the
resolved module's registered surface. Each function/type descriptor is a
`{name, is_pub}` struct — visibility is *exposed as a field* rather than
pre-filtered, since the evaluator has no caller-module context with which to
apply the "pub from outside, all from inside" rule itself; a consumer filters on
`is_pub`. `checker::register_comptime_modules` registers every module's surface
after functor instantiations are materialized, so a synthetic functor module is
reflected exactly like a hand-written one (by its synthetic name). The checker
recognizes an `M.<call>()` reflection call in `infer_field_call`
(`names_reflectable_module`) and types it (str/int/list) so it type-checks and
reaches compile-time evaluation instead of being rejected as an undefined value.

**v1 limits.** Modules are registered under their fully-qualified name and their
leaf segment (`sample.math` is reachable as `math`); reflecting an instantiated
functor through its `use ... as db` *alias* (`db.functions()`) is not wired yet
— use the module name. Two submodules sharing a leaf name collide (last wins).

### Metadata (Phase 5) — landed

Each `use m[args]` a file requests is exported to its `.kmeta` record as a
`FunctorInstantiation` message (`src/module_metadata.proto`, schema bumped to
v2): the functor path, each argument's source spelling, the `as` alias, and a
canonical instantiation key `functor.path[arg, arg]`. Built syntactically in
`build_module_metadata` (the parser-level metadata pass has no resolved types),
so the key is stable across compilation units for the same functor/arguments.

## 6. Pipeline-ordering rule (Phase 6) — the hard constraint (landed)

Today the module graph and `use` resolution run *before* type checking and
compile-time evaluation. `static if BUILD.test: use fake_io as io` makes the
module graph depend on evaluation — a potential circularity.

**Rule:** a `static if` that gates a `use` may reference only *early-evaluable*
conditions. This fragment is evaluated in a dedicated pre-resolution pass
(`src/driver/static_if_stage.cpp`, `fold_static_if_imports`, a sibling of the
driver's lowering stage) that folds the taken branch's items into the file's
item list and drops the untaken branch before the module graph is built.
Richer conditions get a diagnostic that explains the restriction. This keeps
confluence trivial and the graph's evaluation-dependence a bounded pre-pass —
resist any "just evaluate a bit more" widening.

**As implemented (v1).** The pre-pass only touches a `static if` whose branch
contains a `use` (every other `static if` keeps its ordinary check-time
branch-selection). The condition is evaluated with a *standalone, resolution-
free* `comptime::evaluator` — no globals registered — so only literal
expressions fold (`static if true:`); the taken branch is folded recursively so
a nested import-gating `static if` inside it is handled too. `BUILD.*` build
flags and resolution-dependent statics are named in the rule above but have no
evaluator support yet, so they currently hit the restriction diagnostic — the
natural extension is to teach the standalone evaluator about `BUILD.*`.

## 7. Sequencing

`0 → 1 → 2 → 3 → (4, 5, 6 in any order) → 7`. **All phases landed as of
2026-07-17** — the feature was real after Phase 3, and reflection (4), metadata
(5), `static if` gating (6), and satisfies hardening (7, deep type-equality)
are now complete too. The highest-risk items proved tractable: coherence inside
functors falls out of per-instantiation target-type projection plus memoization
(§4), and pipeline ordering (§6) is a bounded, literal-only pre-pass.
