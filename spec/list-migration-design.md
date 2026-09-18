# Moving `list[T]` out of the compiler

**Status:** Plan. Nothing here is implemented.

`vector[T]` (`src/std/list.kira:83`) is a growable, heap-owning sequence
written entirely in Kira over `std.mem` and the `machine` layer, with no
compiler support beyond what any user struct gets. It is the type `list[T]`
should *be*. This document is the plan for making it so.

The point is not to delete code for its own sake. Every capability the
compiler currently reserves for `list[T]` — literal construction, iteration,
indexing — is a capability no user-written collection can have. Each phase
below removes one of those reservations by turning it into a language feature
any type can opt into. The migration of `list[T]` is the acceptance test for
that feature, not the goal in itself.

## What the compiler currently knows about `list`

Four distinct pieces of knowledge, in four places. They are what a Kira struct
cannot replicate today.

| # | Privilege | Where it lives |
|---|---|---|
| 1 | `list` is a type constructor of arity 1 | `k_builtin_generic_arities`, `src/semantic/types.cpp:38` |
| 2 | `.len()`, `.push()`, `.cell()`, `.mutable_cell()` resolve with no declaration | `k_builtin_methods`, `src/semantic/check.cpp:203`; `builtin_method_owner` at :290, `builtin_method_result` at :9153 |
| 3 | `[1, 2, 3]` in a `list`-expecting position *is* a list | `infer_array`'s `expected_is_list` path, `src/semantic/check.cpp:14154` |
| 4 | `xs[i]`, `&xs[i]`, `for x in xs` read a known 3-slot header | `resolve_container_view` — `src/llvm_codegen/codegen.cpp:2613` and `src/bytecode_compiler/compile.cpp:2662`; `lower_for_stmt`'s `lower_indexed_loop` fallback, `src/hir/lower.cpp:3171` |

Plus the runtime half: `list_reserve_slot` (`src/runtime/layout.h`, wired at
`src/llvm_codegen/codegen.cpp:4434`) implements growth in C++, against the
shared bump arena, with no `realloc` and no `free`.

Privilege 2 is already almost gone — `extend[T] list[T]` in `src/std/list.kira`
adds `is_empty`/`first`/`last` as ordinary Kira. The remaining four names are
the ones that need the representation.

## Phase 0 — scope-exit `drop` (prerequisite, todo item 6)

**Not optional, and not part of this plan's own scope.** `list[T]` today
leaks nothing the arena wasn't already leaking. `vector[T]` under
`KIRA_ALLOCATOR=system` holds a `malloc`ed block that only an explicit
`.free()` returns. Flipping `list[T]` onto that storage *before* drop glue
exists would turn every `[1, 2, 3]` in every existing program into a leak —
a strict regression, silently.

So: phases 1–3 are independent of drop and can land in any order. **Phase 4
must not land until todo item 6 does.** The sequencing is the whole reason
this document separates them.

What phase 4 needs from item 6, specifically, is narrower than the full item:
reverse-order drop of locals at scope exit, implicit field-wise drop for
aggregates, and not dropping a moved-from binding. Unwinding-through-drop and
the prelude `drop(x)` function are not prerequisites.

## Phase 1 — an `index` trait with an operator hook

Indexing is the only core operator with no user-facing trait. `+`, `-`, `*`,
`/`, `%`, `==`, `!=` all dispatch to impls through `operator_dispatches`
(`src/semantic/types.h:743`), recorded by `require_operand_trait`
(`src/semantic/check.cpp:~7315`) and emitted by `lower_binary`
(`src/hir/lower.cpp:1063`). `a[i]` has no equivalent: it is a builtin operator
over `resolve_container_view`'s four known shapes and nothing else.

**Design.** Three traits, because reading, writing and borrowing an element
are genuinely three operations and collapsing them produces the wrong
diagnostics:

```kira
pub trait index[I]:
    type output
    def at(self, i: I) -> Self.output

pub trait index_mut[I]: requires index[I]
    def at_mut(mut self, i: I) -> mut Self.output

pub trait index_set[I]:
    type output
    def set_at(mut self, i: I, value: Self.output) -> unit
```

- `xs[i]` in value position → `index.at`.
- `xs[i] = v` → `index_set.set_at`.
- `&xs[i]` / `&mut xs[i]` → `index.at`/`index_mut.at_mut`, with the existing
  view-borrow exclusivity rules applying to the returned reference exactly as
  they do to a method call returning `mut T` today.

Generic in `I` so `xs[0..3]` (range indexing, already general-stride) becomes
`index[range]` with `output = slice[T]` rather than a second mechanism.

**Compiler work.**

1. `checked_types` gains `index_dispatches` — same shape as
   `operator_dispatches`, keyed on the `ast::index_expr`. A *new* map rather
   than a reuse: an index expression has a receiver and a subscript, not two
   operands, and the place/value distinction has no analogue in `lower_binary`.
2. `infer_index` (`src/semantic/check.cpp`) gains a user-type arm: when the
   receiver is `struct_kind`/`sum_kind`/`opaque_kind`, require the trait,
   instantiate the impl method for this receiver (the
   `instantiate_impl_method_for` path the arithmetic operators already use),
   and record the dispatch. Builtin receivers keep their current path
   untouched — this phase adds a capability, it does not move `list`.
3. `lower_index` emits an `hir_call` when a dispatch is recorded, and the
   existing `hir_index` otherwise. **No backend change in this phase**, which
   is the reason to do it first: both tiers already compile method calls.
4. Assignment: `check_assign`'s index-place arm routes to `index_set`.

**Diagnostic.** A missing impl must say what to write, in the house style:

```
error: `grid` cannot be indexed with `[...]`
      |     ^^^^^^^ this is a `grid[int32]`, which has no `index` impl
help: Indexing is a trait, not a builtin. Add an impl that says what
      `grid[int32][usize]` means:

          impl[T] index[usize] for grid[T]:
              type output = T
              def at(self, i: usize) -> T:
                  ...
```

**Tests.** A `codegen_stress` file with `# expect:` on a user type whose
`at` deliberately returns `i * 2 + 1` — a value no accidental fall-through to
the builtin indexer could produce. Plus a `semantic_check_test` for the
missing-impl diagnostic, and one confirming `xs[i] = v` on a type with `index`
but no `index_set` is refused *naming `index_set`*, not "cannot mutate".

## Phase 2 — `for` through `into_iterator`

`into_iterator[T]` already exists (`src/std/iter.kira:46`) and is unused by
the compiler. `for` currently reaches a user type only by duck-typing on a
`next` method (`try_resolve_iterator`, `src/semantic/check.cpp:13541`,
recorded in `for_iterator_dispatches`, `src/semantic/types.h:486`); a
*collection* — which is not itself an iterator — has no route in at all.

**Design.** `for x in e` resolves in this order, first match winning:

1. The existing dedicated shapes (range, `option`, `generator`, `str`, and
   the builtin containers). Unchanged.
2. `e`'s type implements `into_iterator[T]` → insert a call to
   `into_iter(self)` and loop over the result via the existing iterator path.
3. `e`'s type has a `next(mut self) -> option[T]` → today's duck-typed path,
   kept so an iterator is directly iterable.
4. Otherwise: the current fallback becomes an error rather than
   `lower_indexed_loop`, once phase 4 has removed the builtin `list` that
   needs it.

Ordering 2 before 3 matters: a type could plausibly be both, and "hand me
your iterator" is the more specific answer.

**Compiler work.** `try_resolve_iterator` gains a sibling
`try_resolve_into_iterator`; `for_iterator_dispatch` gains an optional
"adapter call to insert first" field; `lower_iterator_loop` emits that call
into the loop preamble and otherwise proceeds unchanged. The desugaring stays
in lowering — no new HIR node, matching how the duck-typed path was done.

**Library work.** `vector[T]` gets `vector_iter[T]` and
`impl[T] into_iterator[T] for vector[T]`. This is where the first real proof
lands: `for x in v` over a Kira-written collection, both tiers, same output.

**Test.** A `# expect:` corpus file summing a `vector` through `for`, and a
negative test that a struct with neither impl gets a diagnostic naming
`into_iterator` rather than a lowering failure.

## Phase 3 — literal construction through a named constructor

`[1, 2, 3]` becomes a `list[T]` because `infer_array` says so
(`src/semantic/check.cpp:14154`). The array *value* is built, then the
`expected_is_list` branch retypes it.

**Design.** A collection opts into literal syntax by implementing:

```kira
pub trait from_array[T]:
    def from_array[n: usize](items: array[T, n]) -> Self
```

Const-generic monomorphization already compiles a `def f[n: usize]` once per
constant, so the `n` half of this costs nothing new.

`[a, b, c]` in a position expecting type `C`:
- `C` is `array`/`slice` → today's behavior, unchanged.
- `C` implements `from_array[T]` → build the array literal, then call
  `C.from_array(that)`.
- `C` is the builtin `list` → today's behavior, until phase 4.

**Compiler work.** `checked_types` gains `array_literal_conversions`
(literal → resolved constructor). `infer_array`'s `expected_is_list` branch
generalizes. Lowering wraps the existing array-literal HIR in a call. Again,
no backend change.

**Open question, to settle before implementing.** An array literal in a
position with *no* expectation (`let xs = [1, 2, 3]`) currently produces
`array[int32, 3]`. After the migration, `let xs = [1, 2, 3]` most likely
wants a `list`. Two candidate answers: (a) a designated default collection
named in the prelude, or (b) keep it an `array` and require
`let xs: list[int32] = [1, 2, 3]`. (b) is more honest and worse to write; (a)
is a hidden default of exactly the kind the rest of the language avoids.
**Recommendation: (b)**, plus a diagnostic on the first `.push` to an array
that says which annotation to add. Decide this before writing code, not
during.

## Phase 4 — flip `list[T]`

Only after phases 1–3 and todo item 6.

1. Rename `vector[T]` → `list[T]` in `src/std/list.kira`, with
   `impl index`/`index_set`/`into_iterator`/`from_array`, and `drop`.
2. Delete `list` from `k_builtin_generic_arities`
   (`src/semantic/types.cpp:39`) and its four entries from
   `k_builtin_methods` (`src/semantic/check.cpp:205-216`).
3. Delete the `list` arm of `resolve_container_view` in **both** backends.
   `slice`, `str` and `array` keep theirs — they are views and language
   primitives, not library types.
4. Delete `list_reserve_slot` from the runtime and its LLVM declaration.
5. Sweep every stdlib module written against builtin `list` — `std.algo` in
   particular, whose sorts index and swap in hot loops.

**The thing that will actually hurt, and the honest answer.** Every `list`
operation becomes a real Kira call where it used to be an inlined opcode
sequence. `std.algo`'s introsort does `xs[i]`/`xs[j] = ...` in its innermost
loop; on the bytecode VM each becomes a call frame. Expect a measurable
regression there.

Do not pre-emptively design an inliner for this. Measure first: check in a
benchmark *before* phase 4 (sort 100k `int32` on both tiers, both allocator
modes) so the number is a fact rather than a fear. If it is unacceptable, the
proportionate fix is inlining trivial trait-method bodies in HIR, which
benefits every user collection and not just `list`. The migration's value is
that `list` stops being special; buying its performance back with a *second*
special case would spend the whole point.

## Phase 5 — what the migration buys, made visible

The deliverable that proves it worked is a user-written collection in the
test corpus — a `ring[T]` or `grid[T]` — constructed from a literal,
iterated with `for`, indexed with `[]`, and dropped at scope exit, with a
`# expect:` value. Every one of those is impossible today. If that file
compiles and runs identically on both tiers, `list` genuinely is not special
any more.

## Ordering summary

```
Phase 1 (index trait)      ─┐
Phase 2 (into_iterator)    ─┼─ independent, any order, no backend changes
Phase 3 (from_array)       ─┘

todo item 6 (drop glue)    ─── independent, required before ↓

Phase 4 (flip list)        ─── needs all four above
Phase 5 (proof)            ─── needs phase 4
```

Phases 1–3 each make the language strictly more capable on their own and are
worth landing whether or not phase 4 ever happens. That is deliberate: if the
performance measurement in phase 4 comes back bad enough to stop the
migration, nothing in phases 1–3 was wasted.

## See also

- [`43-list.md`](specification/04-stdlib/collections/43-list.md) — what
  `list[T]` and `vector[T]` are today.
- [`38-machine-layer.md`](specification/03-advanced/38-machine-layer.md) —
  the substrate `vector[T]` is built on.
- [`todo.md`](todo.md) item 6 — drop glue, the phase 4 prerequisite.
