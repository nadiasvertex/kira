# Local Parameter-Usage Inference — Design Notes

Status: implemented (`src/semantic/check.cpp`, `param_usage_inferrer` +
`checker::param_types_for`), covered by `check_test.cpp`. This document
records the design decisions behind it, since they weren't obvious from
the roadmap alone and the first attempt at this got one of them wrong.

## What this is for

`spec/kira-reference.md` documents unannotated parameters as a real
language feature, not just "the checker gives up gracefully":

> You can also leave the types off. When you do, the compiler infers them
> **from the function's body** — never from how the function is called —
> and gives each parameter the most general type the body allows.

```kira
def double(x):
    return x * 2        # x can be any number; double works for all of them
```

Before this work, every unannotated, non-`self` parameter was simply
`k_unknown_type` (compatible with everything, per `types.h`'s design) —
correct in the sense of never producing a false positive, but not actually
implementing "the most general type the body allows." The roadmap's
Missing list calls this out directly: "Full Hindley-Milner-style inference
for unannotated parameters (they are currently treated as
unconstrained)."

## Scope decision: local body-usage only, not whole-program HM

Before writing any code, the choice was between:

- **Local body-only inference**: a fresh type variable per unannotated
  parameter, solved by unifying against usage inside that function's own
  body. No cross-function argument propagation, no call-graph ordering.
- **Whole-program call-site-driven inference**: also pull argument types
  in from every call site, requiring topological ordering over the call
  graph (or a fixed-point iteration) and cycle handling for mutual
  recursion — the "full HM" the roadmap names.

We chose local-only, and not only for engineering-cost reasons: pulling
argument types in from call sites is what `kira-reference.md` explicitly
rules out. "Inference never crosses a call. A function's types come from
its own body... the compiler asks for an annotation rather than guessing
from callers." Whole-program call-site-driven inference isn't deferred
future work the way, say, generic instantiation is — it's a shape of
inference the language's design forbids outright, because a parameter's
type must stay a fact about that one function body, not something that
shifts depending on who calls it. Building it later would mean changing
the language, not finishing the checker.

Local-only inference is also strictly additive (a parameter with no
recognizable in-body constraint stays exactly as it was — `k_unknown_type`
— so there is no regression surface), and it doesn't require inventing
call-graph machinery the checker has no other use for yet.

## The mistake this design corrects: literals are not anchors

The first implementation unified an unannotated parameter against
whatever a bare literal defaults to (`infer_literal`'s own fallback:
`int_lit` → `int32`, `float_lit` → `float64`, ...). That broke the exact
example from the reference: `double(x): return x * 2` inferred `x: int32`
and then rejected `double(3.5)` — directly contradicting "works for every
numeric type, chosen at each call."

The fix: **a bare literal is never a concrete anchor** anywhere in the
walker. `walk_expr`'s `literal_expr` case returns `k_unknown_type`
unconditionally rather than a defaulted concrete type (there is no
`literal_type` helper anymore — it was deleted, not left dead). This is
correct, not just conservative: `infer_arithmetic` already treats an
unknown/type-parameter operand as fully permissive with no diagnostic
(`check.cpp` — see the `is_unknown(lhs) || is_unknown(rhs)` early return in
`infer_arithmetic`), which *is* the generic behavior the reference
describes. So leaving a literal-only-constrained parameter at
`k_unknown_type` isn't a fallback for a case we can't handle — it's the
actual right answer.

## What is still a legitimate anchor

A parameter is pinned to one concrete type only when the body leaves no
other possibility — i.e. the pin is a *fact*, not a guess:

- **An already-concretely-typed sibling parameter.** `def add(x, y:
  int32): return x + y` forces `x: int32`, because Kira never converts
  numbers implicitly (per the existing `type_mismatch` diagnostic's own
  conversion-hint text) — there is no other type `x` could be for this
  body to check.
- **A same-module callee's concretely-annotated parameter.** `def
  relay(x): return sink(x)` where `sink(y: int32)` forces `relay`'s `x:
  int32`, for the same reason.

Operators still *link* both operands' type variables together (`x + y`
requires both sides to end up the same type) — that's a structural fact
independent of any anchor, and is harmless: linking two open variables
that never meet a concrete anchor just keeps them equal, still fully
generic.

## Why the walker is a separate, bounded pass rather than reusing `infer_expr`

`infer_expr`/`check_body_node` are the existing, diagnostic-producing,
top-down-expected-type checker functions. They're deeply coupled to live
per-function checker state (`scopes_`, `self_type_`, `return_type_`,
`file_id_` for diagnostic attribution) that's saved/restored around each
function check. Inference for function `F` can be triggered indirectly —
e.g. while checking a call to `F` from inside a *different* function `G`
that's currently live — so reusing `infer_expr` would mean carefully
saving and restoring all of that state just to peek at `F`, and risking
spurious diagnostics or double-reporting if anything leaked through.

Instead, `param_usage_inferrer` is a small, self-contained, read-only
walker: it takes only a `type_table&` and the callee's owning
`module_members*`, reads immutable AST, and never emits diagnostics or
touches checker state. This makes it safe to invoke at arbitrary points
(mid-call-check, recursively) with no save/restore choreography. The cost
is that it's deliberately narrow — it only recognizes a bounded set of
shapes (arithmetic/comparison/logical operators, `let`/`var`/assign,
`if`/`while`/`for`/`match`, and calls to same-module functions with a
simple builtin-scalar-annotated target parameter) and silently skips
anything else, rather than trying to cover every expression form
`infer_expr` does.

## Caching: one shared answer, not two independent guesses

Before this change, an unannotated parameter's type was computed
independently in two places — `check_function` (checking the function's
own body) and `signature_params`/`fn_type_of` (checking call sites
elsewhere) — each defaulting to `k_unknown_type` on its own. `param_types_for(decl, owner)` is now the single source of truth, cached
per-declaration (`inferred_param_types_`, mirroring the existing
`static_types_`/`statics_in_progress_` pattern for `static` initializers),
so both call paths agree. A `param_inference_in_progress_` guard prevents
re-entrant inference of the same declaration; in practice the walker never
recurses into `param_types_for` for a callee (it only reads a callee's raw
annotation, never its inferred types), so this guard is currently
defensive rather than load-bearing, but it costs nothing to keep for when
that changes.

## What's still a known gap: no per-call-site re-checking

Because the checker checks each function body exactly once (not once per
instantiation), an argument that's genuinely incompatible with an
*unconstrained* parameter's body — e.g. calling `double` with a type that
has no `mul` impl — is not caught. Catching that requires re-checking the
function body per call-site instantiation (true monomorphized checking,
the way explicit `[T: bound]` generics would also need it, since they
aren't re-verified per instantiation today either). We considered this
directly and chose not to build it now:

- **Diagnostic locality.** Kira's core value ("the compiler is a
  teacher") means an error should point at the declaration, not scatter
  across call sites the way C++ template errors do. Monomorphized
  checking risks exactly that regression without careful diagnostic-chain
  design.
- **Compile-time cost.** Re-checking a generic body at every call site is
  O(calls) instead of O(declarations), cutting against the roadmap's
  "script-friendly" unoptimized-compile goal.
- **Architecture fit.** The checker's per-function state (`scopes_`,
  `self_type_`, `return_type_`) is singleton, save/restored state, not
  parameterized by "which substitution." Making it re-entrant per
  instantiation is a real rework, and generic functions calling other
  generic functions need instantiation-graph memoization to avoid
  combinatorial blowup.
- **Sequencing.** The roadmap already plans monomorphization at LLVM
  lowering time (phase 5). A second, earlier monomorphization mechanism
  just for semantic checking risks committing to a design that gets
  discarded once the typed-IR phase settles how instantiation works
  end-to-end.

This is recorded as explicit future work, not something to approximate.
