# Generic Argument Inference — Design

This fixes the decisions behind three related changes to generic
instantiation: solving type parameters from the expected type, honoring
explicit type arguments on method calls, and dispatching a static member
through a solved type parameter. Together they are what a unified
`collect` needs; each is independently useful.

They share one substrate — `solve_generic_params` and
`find_or_check_generic_instance` in `src/semantic/check.cpp` — which is why
they are designed together even though they land separately.

## 1. Motivation

`spec/collections-algorithms-design.md` §5.1 defers `from_iter` as a trait,
on the grounds that a generic `collect[C: from_iter]` needs call-site bound
enforcement and return-type-driven inference and Kira has neither. The first
half of that is wrong: bound enforcement affects the *quality of the error*
when `C` is unsuitable, not whether `collect` computes the right answer. The
second half is right and is the actual blocker, along with two smaller gaps
that only surface once you try to write the call.

The three gaps:

- **G1 — return-position inference.** `C` in
  `def collect[I, C](self: I) -> C` appears only in the return type.
  `solve_from_argument_types` (`check.cpp:4677`) unifies parameters against
  arguments and nothing else, so `C` is never bound and
  `report_unsolved_type_param` fires.
- **G2 — explicit type arguments on method calls.** `instantiate_hk_method`
  (`check.cpp:4877`) passes `explicit_generic_args{}`, so brackets on a
  method call are **silently discarded**. This is a live correctness bug,
  not a missing feature: `b.take[int64](5)` parses cleanly, compiles
  cleanly, and ignores the `int64`.
- **G3 — static dispatch through a type parameter.** `collect`'s body must
  call `C.from_iter(self)` with `C` a solved type parameter. `is_static`
  exists on impl members (`ast.h:1727`), but no resolution path accepts a
  qualified base that *is* a type parameter.

## 2. G1 — the expected type as a solution source

### 2.1 The hint is a fallback, never a constraint

`solve_generic_params` (`check.cpp:4560`) documents three answer sources in
decreasing order of explicitness. The expected type becomes a fourth, and it
goes **last**:

1. an explicit type argument — the call says so outright;
2. `unify_rigid` against the argument types;
3. an enclosing instance's fixed bindings;
4. **the expected type at the call site.**

A parameter already bound by 1–3 is never revisited. The hint fills only
what remains open.

This ordering is not a stylistic preference, and it is the single decision
in this document most expensive to reverse. `k_unknown_type` deliberately
unifies with everything (CLAUDE.md; it is what keeps one gap in knowledge
from cascading). That means the type system has no way to distinguish a hint
the checker is confident in from a hint that is a placeholder standing in
for "not known yet." If a hint could *override* an argument-derived binding,
then `let x: int64 = take(5i32)` would silently re-solve `T := int64` and
retype the call instead of reporting a mismatch — and the same mechanism
would let any unknown-typed context quietly rewrite a well-understood call.
Fallback-only leaves every existing diagnostic exactly as it is: annotations
still get checked against what inference found, because inference still runs
first and independently.

The corollary is that adding the hint can only turn errors into successes,
never successes into different successes. That is what makes G1 safe to land
without auditing existing call sites.

### 2.2 Mechanism

In `instantiate_generic_function`, after `solve_from_argument_types`:

```cpp
if (decl.return_type != nullptr && !types_.is_unknown(expected)) {
  unify_rigid(resolve_type(*decl.return_type, template_ctx), expected,
              type_bindings);
}
```

`unify_rigid` already declines to overwrite a bound name, so "arguments
first" falls out of ordering alone rather than needing a second mechanism.

The plumbing is the bulk of the work: `expected` must reach
`instantiate_generic_function`, which today takes no hint. It threads from
`infer_call(call, expected)` (`check.cpp:7815`) through both the
free-function and receiver-call paths.

Refinements are stripped before the hint is used, matching `infer_expr`'s
existing rule (`check.cpp:9360`): an expected refined type is an obligation
to be proved, not a shape to infer *as*. A `-> T` solved against an expected
`positive` binds `T := int32`, and the refinement is then checked the way it
would be for any other call.

When the hint is absent or unknown, nothing binds and
`report_unsolved_type_param` fires as it does today. `let xs = it.collect()`
with no annotation genuinely cannot be inferred, and saying so is correct.
That diagnostic should name the expected-type route in its help text, since
"annotate the binding" is now an actionable fix.

### 2.3 Hint propagation: everywhere it can reach

Principle of least surprise governs. A user who learns that `collect()`
infers from context will not accept that it works in `let` but not in
`return`, and every unreached position is a place they must write a
turbofish for reasons they cannot see. The hint therefore propagates to
every position where the checker knows an expected type:

| Position | Today | Action |
| --- | --- | --- |
| Annotated `let` / `var` initializer | hint present | reaches G1 free |
| Call argument | hint present | reaches G1 free |
| `return` expression | declared result known | thread it |
| Assignment RHS | target type known | thread it |
| Struct literal field | field type known | thread it |
| Array / list element | element type known once fixed | thread it |
| Match arm result | joined against other arms | thread the join type |
| Lambda body with declared result | `check.cpp:9025` | thread it |
| Unannotated `let` | no hint exists | error, correctly |

Each row is a small independent change against the same `infer_expr(expr,
expected)` signature — the pipe already exists; these are call sites passing
`k_unknown_type` that have a better answer available. Landing them
individually is fine. Landing only some of them and calling G1 done is not:
the resulting behavior is arbitrary from the outside.

## 3. G2 — explicit type arguments on method calls

### 3.1 The bug

Brackets on a method call are parsed and discarded. Fixing it means
`instantiate_hk_method` accepts an `explicit_generic_args` and forwards it
to `solve_generic_params`, which already knows what to do with one.

The grammar already sanctions the syntax: `postfix_suffix` carries a
`"." IDENT "[" type_arg_list "]"` production for exactly this form
(`kira-grammar.ebnf`). The parser builds it correctly and the checker
discards it, which is what makes this a defect rather than a feature
request.

`explicit_generic_callee` (`check.cpp:4463`) is already general over its
base expression — it returns the base and the bracket contents without
caring what the base is. Only its caller `infer_explicit_generic_call`
(`check.cpp:7749`) restricts to `ident_expr`. The receiver path reuses it
unchanged.

Because the brackets are currently ignored rather than rejected, this is a
behavior change: code that passes a bracket argument that disagrees with
what the arguments imply changes meaning from "silently use the arguments"
to "use the brackets, or report a conflict." That surface is small — the
construct is undocumented and useless today — but it needs a regression test
pinning the new behavior, and it should be called out in release notes
rather than filed as a pure bug fix.

### 3.2 Prerequisite: generic type arguments must be expressible

`explicit_type_argument` (`check.cpp:4495`) handles `ident_expr` and
`module_path_expr` and returns `nullopt` for everything else. So `int32` is
expressible as a type argument and **`list[int32]` is not** — which is
exactly the argument `collect` needs. This is a prerequisite for G2 being
useful, not a separate nicety.

Here too the grammar is ahead of the implementation: `type_arg` already
admits any `type_expr`, and `named_type` already admits a nested
`"[" type_arg_list "]"`, so `list[int32]` is a well-formed single type
argument by the grammar's own rules. Only the checker's expression-to-type
mapping is narrower.

The fix extends the switch to `index_expr`, mapping it to a generic
application and recursing on the bracket contents, so an explicit type
argument reaches the same `resolve_type` path a written annotation does.
Nesting follows for free.

### 3.3 Arity and non-generic bases

Two cases the current code never had to answer:

- **Too many bracket arguments for the declaration's parameters** — an
  error naming the declaration and its parameter count. Not silently
  truncated.
- **Brackets on a non-generic method** — indexing of the call's *result* is
  not what the shape means, so this is an error explaining that the method
  takes no compile-time parameters, with a help line distinguishing it from
  indexing.

## 4. Bracket disambiguation, stated normatively

Brackets carry three meanings: indexing, const-generic value argument, and
type argument. Today the checker resolves between them correctly but as an
implementation accident distributed across `explicit_generic_callee`,
`lookup_value`, and `solve_generic_params`. Extending the type-argument form
to nested generics is the point at which that needs to be a written rule.

**The rule, in order:**

1. If the base resolves in `value_namespace` to a local binding, parameter,
   or static, the brackets are **indexing**. A value shadowing a generic
   name means the user meant the value (`check.cpp:7753` already implements
   this).
2. Otherwise, if the base resolves in `module_type_namespace` or names a
   generic callable, the brackets are a **compile-time argument list**.
3. Within a compile-time argument list, each argument's meaning is fixed by
   the *declaration's* parameter at that position, never by the argument's
   own syntax: a `[n: usize]` parameter takes a value argument, a bare `[T]`
   parameter takes a type argument. `solve_generic_params` already
   dispatches this way.
4. Anything else is an error, not a fallback to indexing.

Rule 3 is what makes nested generic type arguments safe. `f[list[int32]]`
and `f[3]` do not need to be distinguished by looking at the brackets — the
declaration of `f` already says which is expected, and a mismatch is a
diagnosable error rather than an ambiguity.

### 4.1 Why not angle brackets

Moving type arguments to `<>` was considered, on the reasoning that
indexing should get `[]` to itself. It is rejected.

The collision `[]` creates is *decidable by name lookup*, because Kira
keeps types and values in separate namespaces (`symbols.h:18`:
`module_type_namespace` vs `value_namespace`). Rule 1 above is a single
lookup and it is already implemented. The collision `<>` creates is with
the comparison operators — `a < b, c > (d)` is simultaneously a valid
expression and a valid instantiation — and no namespace lookup resolves it,
because the ambiguity lives in the operators rather than the names. It
requires unbounded lookahead or a disambiguating token. This is why Rust
carries `::<>` and C++ carries the `template` keyword; both adopted those
after the fact, at permanent cost to their surface syntax.

Adopting `<>` would trade a collision the compiler already resolves in one
lookup for one that has permanently scarred two languages with far larger
budgets, at a migration cost of 471 generic-bracket occurrences across 417
`.kira` files, 199 in `kira-reference.md`, and effectively all of
`kira-grammar.ebnf`.

The legitimate complaint behind the proposal — that bracket meaning is
undocumented and accidental — is addressed by §4 being normative and by
`kira-grammar.ebnf` and `kira-reference.md` stating the rule.

## 5. G3 — static dispatch through a type parameter

### 5.1 Shape

```kira
pub trait from_iter[T]:
    static def from_iter[I: iterator[T]](it: I) -> self

pub def collect[I, C](self: I) -> C:
    return C.from_iter(self)
```

`C` is solved by §2, then `C.from_iter` must resolve to the static member of
the impl for the type `C` was solved to.

### 5.2 Resolution

Name resolution gains one case: a qualified path whose base names an
in-scope entry of `type_parameter_namespace` resolves by substituting the
active binding from `type_param_slots_` and then resolving the member
against the resulting concrete type. Inside an uninstantiated template no
binding exists, so the path resolves to `k_unknown_type` and is checked only
in instances — the same discipline `find_or_check_generic_instance` already
applies to the rest of a generic body.

`from_iter` needs no object safety. Dispatch is static and monomorphic; the
trait is never boxed and never becomes a `box[from_iter]`.

### 5.3 Instance naming

`instantiate_hk_method` builds names like `option::pure$int32` from a
`target_type_name`, because that is how `hir::lower_impl_associated_
functions` names an impl member. A static reached through a type parameter
has no such name until substitution.

It takes the **solved type's interned name** — the same string
`bind_generic_type` records in the solution suffix — so `C.from_iter` inside
`collect[list_iter[int32], list[int32]]` names `list[int32]::from_iter`.
This keeps lowering's naming scheme untouched: it sees an ordinary impl
member on a concrete type and never learns a type parameter was involved.

### 5.4 The instantiation-site note is required here, not optional

`spec/collections-algorithms-design.md` §8 R1 proposes appending an
instantiation-site note when an error is reported inside a generic instance
body, and treats it as a cheap mitigation for unenforced bounds. For
`collect` it is not optional, and it should land **with** G3 rather than
after it.

With `C: from_iter` unenforced, an unsuitable `C` fails as "no static
`from_iter` on `int32`" pointing into `std/iter.kira`. For `sum` that is
tolerable — it is a rare mistake. For `collect` it is the *common* failure,
and the user never wrote a call naming `from_iter` at all: they annotated a
binding. Without the note the diagnostic is unactionable.

```
error: no static `from_iter` on type `int32`
  --> std/iter.kira:210:12   (in `collect[list_iter[int32], int32]`)
   = note: instantiated from pipeline.kira:12:9 — `let n: int32 = xs.iter().collect()`
   = note: `C` was solved to `int32` from the type of `n`
   = help: `collect` builds a collection; `int32` is not one. Did you mean
           `let n: list[int32]`, or `.sum()` to total the elements?
```

The second note — naming *where the solution came from* — is new, and G1
makes it necessary. Once a type parameter can be solved from context rather
than from something the user wrote, the error must say which context, or the
binding appears to come from nowhere.

## 6. Bounds

`C: from_iter` remains decoration after all three gaps land, per R1. This is
acceptable and is not a blocker: an unsuitable `C` fails inside the instance
body with the note of §5.4 pointing at the user's line. Enforcement makes
the message better; it is not what makes `collect` correct.

Stated plainly so the standard library documentation can say so: `collect`
is inferred, not bounds-checked. A `C` that is not a collection is a
compile-time error at the point of use, phrased indirectly.

## 7. Sequencing

Three commits, in order. Each is independently shippable and independently
useful.

1. **G1** — return-position unification plus the §2.3 propagation table.
   Unblocks every function generic in its return type, which is a standing
   limitation unrelated to `collect`.
2. **G2** — `explicit_type_argument` extended to generic applications
   (§3.2), then method-call brackets honored (§3.1), then §3.3's errors.
   Fixes a live silent-wrong-answer bug.
3. **G3** — type-parameter static dispatch (§5.2/§5.3) and the
   instantiation-site note (§5.4). Only after this does `collect` exist.

Regression tests accompany each, per CLAUDE.md. G2 specifically needs a test
pinning that brackets are now honored, since the old behavior was silent.

## 8. Unrelated defect found while spiking

`impl box_of:` on a non-trait type reports "`box_of` is a type, not a trait"
with the help text *"Write `impl box_of:` to add inherent methods to a type
instead"* — advising verbatim the input it just rejected. It should say
`extend box_of:`.
