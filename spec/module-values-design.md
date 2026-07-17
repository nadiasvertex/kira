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
- **Cycle detection.** A functor whose instantiation (directly or transitively)
  requires instantiating itself gets a cycle diagnostic, reusing the
  in-progress-set pattern (`statics_in_progress_`-style, `check.cpp`).
- **Coherence.** An impl declared inside a functor body enters session-wide
  coherence once per instantiation; its coherence key must incorporate the
  instantiation key, or duplicate-impl errors appear (or, worse, orphan
  violations slip through). Tested in Phase 3.

## 5. Reflection (Phase 4)

`src/comptime/reflect.cpp` extends from type reflection to module reflection:
`M.name()`, `M.functions()`, `M.types()` return descriptor values built by a
read-only traversal of the resolved module's symbols. Visibility-aware: `pub`
members only from outside, all from inside. Works uniformly on ordinary modules,
instantiated functors, and module parameters inside a functor body (reflection
there evaluates per-instantiation, which the Strategy-3 checking model already
provides).

## 6. Pipeline-ordering rule (Phase 6) — the hard constraint

Today the module graph and `use` resolution run *before* type checking and
compile-time evaluation. `static if BUILD.test: use fake_io as io` makes the
module graph depend on evaluation — a potential circularity.

**Rule:** a `static if` that gates a `use` may reference only *early-evaluable*
conditions — literals, build flags (`BUILD.*`), and statics whose initializers
need no name resolution beyond the prelude. This fragment is evaluated in a
dedicated pre-resolution pass (a sibling of the driver's lowering stage) that
folds the taken branch's items into the file's item list and drops the untaken
branch before the module graph is built. Richer conditions get a diagnostic that
explains the restriction. This keeps confluence trivial and the graph's
evaluation-dependence a bounded pre-pass — resist any "just evaluate a bit more"
widening.

## 7. Sequencing

`0 → 1 → 2 → 3 → (4, 5, 6 in any order) → 7`. The feature is real after Phase 3.
Highest risks: pipeline ordering (§6), type identity across instantiations (§4),
and coherence inside functors (§4).
