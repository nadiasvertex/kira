# Typed Lowering IR (HIR) — Design Draft

Status: draft, not yet implemented. This document exists to settle the
open questions in `spec/llm-compiler-roadmap.md` phase 4 before any HIR
code is written, and to scope a first milestone that is small enough to
land and test on its own.

**Precondition satisfied:** Decision 1 below assumed lowering would consume
"a `check_program` result in which every expression's `type_id` is
concrete" — but until now, `check_program` returned `void` and discarded
its `type_table` and every resolved expression type the moment it
returned. That's fixed: `check_program`/`validate_semantics` now return a
`checked_types` (`src/semantic/types.h`) — the session's `type_table` plus
a `std::unordered_map<const ast::node *, type_id>` recording every checked
expression's resolved type, populated from the single `infer_expr`
dispatch chokepoint in `src/semantic/check.cpp`. Milestone step 2
("lowering... taking a checked `semantic_session`") should read this as
"taking a `checked_types`" — the AST + `checked_types` pair is the actual
input lowering has to consume; there's no separate "semantic session" type
that also carries expression types.

## Why this document exists

Phase 3 (semantic analysis) is substantially complete, but it made two
deliberate simplifications that phase 4 cannot silently inherit:

- Unannotated parameters and external-module members are typed
  `k_unknown_type` (`src/semantic/types.h:21`), which unifies with
  everything. This is correct for a checker whose job is "don't produce
  false positives," but a typed IR node cannot carry `unknown` into
  codegen — codegen needs a concrete representation.
- Generic instantiation beyond direct type-parameter positions is not
  implemented. The roadmap lists this as still Missing.

Phase 4 says "introduce a typed HIR or MIR" without saying what it does
about either gap. This document picks answers so implementation can
proceed without re-deriving them mid-PR.

## Decisions

### 1. HIR requires fully-resolved types; `unknown` is a build error, not a value

The HIR only accepts a `check_program` result in which every expression's
`type_id` is concrete (not `k_unknown_type`, and not `k_error_type`).
Lowering does not attempt inference — that is out of scope for phase 4
and stays a follow-on (tracked in the roadmap's Missing list). Concretely:

- The lowering entry point takes the semantic session plus the AST and
  returns `std::expected<hir_module, lowering_error>`.
- If any reachable expression's checked type is `k_unknown_type`, lowering
  fails with a diagnostic pointing at the unannotated declaration, rather
  than lowering a placeholder. This keeps the "typed IR" promise literal:
  every `hir_expr` has a real `type_id`.
- Practically, this means phase 4's first milestone only lowers fully
  annotated functions (explicit parameter types, explicit or
  easily-inferred return types via existing block-tail inference). Files
  containing unannotated parameters still parse and type-check today;
  they just don't lower yet. This is consistent with "small, targeted
  changes" — it does not require solving HM inference to ship an IR.

### 2. Generic functions/types lower as templates, not instantiations, for now

The HIR keeps generic parameters symbolic (same `type_param_kind` id
scheme as `type_table` today) rather than monomorphizing. Monomorphization
(producing one HIR body per instantiation) is deferred to phase 5, where
it's needed anyway to emit LLVM IR for a concrete instantiation. This
keeps phase 4 from having to invent a mangling/specialization scheme
before there's a consumer for it.

Consequence: `hir_call` to a generic function keeps its `type_id` generic
argument list attached; there is no "instantiate now" step yet.

### 3. HIR is a separate module, not an AST decoration

New directory `src/hir/`, mirroring the `src/semantic/` layout:

```
src/hir/
  ids.h        // hir_id-style typedefs, mirrors src/semantic/ids.h
  nodes.h      // hir_expr / hir_stmt / hir_item node definitions
  lower.h       // lowering entry point + lowering_error
  lower.cpp
  lower_test.cpp
```

The AST (`src/parser/ast.h`) is left untouched. Lowering consumes
`const ast::*` nodes plus the `semantic_session`/`type_table` and
produces new, owned HIR nodes. This matches "Keep the parser AST and
typed IR separate" in the roadmap's Architectural Guidance, and avoids
retrofitting `type_id` fields onto AST structs that phase 1–3 code
already depends on being purely syntactic.

### 4. Node shape: flat kind enum + owning `unique_ptr` tree, same pattern as `ast::node`

To stay consistent with the existing style (`ast::node` has a `node_kind`
tag and `ptr<T>` children), HIR reuses the same shape rather than
introducing `std::variant`-based nodes:

```cpp
enum class hir_node_kind : uint8_t {
  // expressions
  hir_literal, hir_local_ref, hir_binary, hir_unary, hir_call,
  hir_field, hir_index, hir_tuple, hir_struct_init, hir_cast,
  hir_block, hir_if, hir_match, hir_lambda,
  // statements
  hir_let, hir_assign, hir_expr_stmt, hir_return,
  // items
  hir_function, hir_module,
};

struct hir_node {
  hir_node_kind kind;
  source_span span;       // preserved from the originating AST node
  type_id type = k_unknown_type; // always concrete for expr nodes; see below
};
```

Every `hir_expr : hir_node` carries a concrete `type_id` (Decision 1).
Statements and items reuse `hir_node` for their span but don't have a
meaningful `type` (leave it `k_unknown_type` — it's simply unused for
non-expression kinds, matching how `ast::node` already has fields that
are only meaningful for some `kind`s).

### 5. Source spans: copied, not re-derived

Every HIR node stores the `source_span` of the AST node it was lowered
from (Decision 4). Lowering never synthesizes a span; sugar that expands
into multiple HIR nodes (Decision 6) gives every synthesized node the
span of the sugar form that produced it, so diagnostics on desugared code
still point at what the user wrote. This is the literal reading of
"Preserve source spans through every lowering step" in the roadmap.

### 6. Desugaring order — smallest sugar first, comprehensions last

The roadmap names comprehensions, pattern aliases, and inline control flow
as things phase 4 should lower into simpler forms. These are not equally
hard, so the milestone breakdown (below) does them in order of increasing
HIR surface required:

1. **Inline `if`/`match` used as expressions** — already expression-shaped
   in the AST (`if_expr`, `match_expr`); lowering is close to 1:1, no new
   control-flow shape needed in HIR beyond `hir_if`/`hir_match`.
2. **Pattern aliases** — lower to an ordinary `hir_let` binding the alias
   name to the matched sub-value; needs pattern-to-binding lowering but no
   new HIR node kinds.
3. **`for` comprehensions** (`ast::for_expr` with `clauses`/`guard`/
   `yield_expr`) — lower into nested `hir_call`s against whatever the
   prelude's iterator protocol turns out to be. This is deferred until
   that protocol is decided (see Non-Goals) because guessing it now risks
   redoing this lowering once the real iterator trait shape exists.

`on`/`async`/`await`/`par`/`race`/`crew` (concurrency and actor-style
forms) and `quote`/`splice`/`static` (compile-time metaprogramming) are
explicitly **not** in scope for the first milestone — see Non-Goals.

### 7. Symbol references: reuse `semantic::symbol_id`, don't reinvent identity

`hir_local_ref` and similar nodes store the existing `semantic::symbol_id`
from `src/semantic/ids.h` rather than minting a parallel HIR-local id
scheme. The semantic session already gives every binding a stable id;
duplicating that in HIR would just be two sources of truth for "which
declaration is this."

## Non-Goals for the first milestone

Explicitly out of scope, so review doesn't drift into scope creep:

- Inference for unannotated parameters (still tracked as separate
  follow-on work in the roadmap's Missing list).
- Monomorphization / generic instantiation (Decision 2 — phase 5).
- Concurrency forms (`async`, `await`, `par`, `race`, `crew`, `on`) and
  compile-time forms (`quote`, `splice`, `static`) — these need their
  runtime/compile-time execution model decided first, which is separate
  design work, not a lowering-shape question.
- Borrow/ownership metadata (roadmap: "once the language model needs
  it" — not yet).
- Any LLVM-facing concern (mangling, calling convention, ABI) — phase 5.

## Milestone breakdown

1. `src/hir/ids.h`, `src/hir/nodes.h` — node shapes for literals, idents,
   binary/unary ops, calls, field/index access, blocks, `let`, `if`
   (statement and expression forms), `return`, function items. No
   comprehensions, no pattern aliases yet.
2. `src/hir/lower.cpp` — lowering for the above, taking a checked
   `semantic_session` for one module and failing closed (Decision 1) on
   any unannotated/unknown-typed reachable declaration.
3. Regression tests (`lower_test.cpp`, hand-rolled `expect`/`fail` per
   `src/testing/test_assert.h`) covering: a fully-annotated function
   lowers to the expected HIR shape; a function with an unannotated
   parameter is rejected with a specific diagnostic (not silently
   skipped); spans on lowered nodes match the originating AST spans.
4. Extend to `match`, pattern aliases (Decision 6, items 1–2).
5. Revisit comprehensions once the iterator protocol exists.

Exit criteria for the milestone (narrower than the roadmap's phase-4
exit criteria, which cover the whole phase): a fully-annotated,
non-generic Kira function can be lowered from checked AST to HIR with
spans and concrete types preserved, with `bazelisk test //...` green.

## Open questions to resolve before step 5 (comprehensions)

- What is the iterator protocol `for` comprehensions lower against (a
  trait? a builtin fixed shape)? Not yet decided anywhere in `spec/`.
- Where does `box[fn]` / trait-object dispatch (per the
  `returned-closure-typing` decision) show up in HIR — as a distinct node
  kind, or as an ordinary `hir_call` through a vtable-shaped struct? This
  matters once lambdas/closures lower, likely in a step after the
  milestone above.
