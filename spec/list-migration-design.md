# Moving `list[T]` out of the compiler

**Status:** All five phases implemented. `list[T]` is an ordinary Kira type
over `std.mem`; what is left is the consequences the flip exposed (todo items
17-20).

| Phase | What it makes possible | Status |
|---|---|---|
| 1 | `index`/`index_set` traits — `v[i]`, `v[i] = x` on any type | **Done**, `index_mut` included. Selection is by key type, so `index[usize]` and `index[range[usize]]` coexist; `&v[i]` (a read-borrow) still has no trait — todo 17 |
| 2 | `for` through `into_iterator` | **Done**, both consuming and borrowing (`for x in &v`) |
| 3 | `from_array` construction, and the settled `list` default for literals | **Done** |
| 4 | Flip `list[T]` onto `vector[T]`'s storage | **Done.** `list[T]` is the Kira struct; the builtin entries and the dead backend code behind them are gone, and the cost is measured (below). What the flip exposed is todo 17-19 |
| 5 | A user collection with all four, as proof | **Done.** The proof type *became* `list[T]` in phase 4, which is the strongest form of it |

`list[T]` (`src/std/list.kira`) is now a growable, heap-owning sequence
written entirely in Kira over `std.mem` and the `machine` layer, with no
compiler support beyond what any user struct gets — reached through
`std.traits`'s `index`/`index_mut`/`index_set`/`from_array` and `std.iter`'s
`into_iterator`. `let v: list[int32] = [1, 2, 3]`, `v[0]`, `v[1] = x`,
`v[0..2]`, `for x in v` all go through traits a user collection can
implement. It was written first as `vector[T]` and renamed in phase 4; the
sections below are the plan that was carried out, kept as the record of why
each piece is where it is.

The point was not to delete code for its own sake. Every capability the
compiler reserved for `list[T]` — literal construction, iteration, indexing —
was a capability no user-written collection could have. Each phase removes
one of those reservations by turning it into a language feature any type can
opt into. The migration of `list[T]` is the acceptance test for that feature,
not the goal in itself.

## What the compiler knew about `list` *(all four removed)*

Four distinct pieces of knowledge, in four places — what a Kira struct could
not replicate. Each row's location is where the privilege lived before the
phase that removed it.

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

## Phase 0 — scope-exit `drop` (prerequisite, todo item 6) *(done, for the case this needs)*

**Was not optional, and was not part of this plan's own scope.** `list[T]`
before this landed leaked nothing the arena wasn't already leaking.
`vector[T]` under `KIRA_ALLOCATOR=system` holds a `malloc`ed block that only
an explicit `.free()` returns. Flipping `list[T]` onto that storage *before*
drop glue existed would have turned every `[1, 2, 3]` in every existing
program into a leak — a strict regression, silently.

So: phases 1–3 were independent of drop and could land in any order. **Phase
4 must not land until todo item 6 does** — the sequencing this document
separated them for.

What phase 4 needs from item 6, specifically, was narrower than the full
item: reverse-order drop of locals at scope exit, implicit field-wise drop
for aggregates, and not dropping a moved-from binding. Unwinding-through-drop
and the prelude `drop(x)` function were never prerequisites. All three of the
narrow requirements now hold (`src/semantic/check.cpp`'s `resolve_drop_plans`
+ `src/hir/drop_schedule.{h,cpp}` + `src/hir/lower.cpp`, verified end-to-end
both backends by `src/testdata/std_test/scope_exit_drop.kira`) — for the
shape phase 4 actually exercises: a plain `let`/`var` local of struct type,
in a function/block whose own tail carries no value (`vector[T]`'s methods
are exactly this shape). See `todo.md` item 6 for the gaps left open beyond
that (a value-producing tail with live locals to clean up, pattern-bound
bindings, sum-type field drop) — none of them block phase 4.

## Phase 1 — an `index` trait with an operator hook *(done)*

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
    def at(self, i: I) -> self.output

pub trait index_mut[I] requires index[I]:
    def at_mut(mut self, i: I) -> mut cell[self.output]

pub trait index_set[I]:
    type output
    def set_at(mut self, i: I, value: self.output) -> unit
```

*(As shipped, in `src/std/traits.index.kira`.* `at_mut` returns
`mut cell[self.output]` rather than a bare `mut self.output`: `mut` is only
accepted in type position before `slice[T]` or `cell[T]`, so a bare
`mut self.output` does not parse. `cell` is the language's existing spelling
for a mutable view of one element — it is what the builtin
`list.mutable_cell()` already hands back — so this is the right shape rather
than a detour around the parser.)*

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

**Tests.** `src/testdata/std_test/index_trait.kira` — a `ramp` with no
storage at all, whose elements are computed, so a `v[i]` that silently fell
through to some builtin container's direct addressing could not produce the
printed values. Plus `reject_index_without_impl.kira` and
`reject_index_write_without_index_set.kira` in `semantic_check_test` for the
two diagnostics.

It lives in `std_test`, not `codegen_stress`, because `codegen_stress` is
compiled *without the stdlib injected* and these traits are stdlib
declarations. `std_test` compares exact expected output, which is the same
strength of check as `# expect:` and not a differential one.

**What actually shipped, beyond the plan.**

- `index_mut` is **declared but not wired**. `&mut v[i]` on a user type still
  has no route; only reads and whole-element writes do. The borrow form needs
  the returned `cell` threaded through the existing view-exclusivity rules,
  which is a larger change than the other two and was not needed by
  `vector[T]`.
- Resolving the element type needed more than `resolve_operator_return_type`.
  An impl's `output` is filed under the target type *as written*, so a
  generic `impl[T] index[usize] for holder[T]` files it under `holder[T]`
  with `T` abstract and a lookup against `holder[int32]` misses. Worse, the
  spelling a user reaches for first is a literal `-> T`, not `-> self.output`,
  which is not an associated type at all. `resolve_index_output` resolves the
  declaration's own return type with the impl block's parameters in scope and
  then substitutes the receiver's arguments — the two-step
  `check_impl_generic_method_call` already used for parameter types.

## Phase 2 — `for` through `into_iterator` *(done, with one gap)*

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

**Test.** `src/testdata/std_test/into_iterator_loop.kira` — exact output, at
two element widths, and the *order* of the elements is printed too, so an
adapter called once per iteration rather than once per loop (which would
restart the iterator and yield the first element forever) fails rather than
passing.

**The gap this exposed, and it matters for phase 4.** `into_iter(self)` takes
the collection **by value**, so `for x in v` *consumes* `v`: the move checker
correctly refuses any later use, including `v.free()`. That is right for this
trait and wrong for a collection — `for x in xs` over a `list` must not
consume `xs`, and today it does not. So phase 2 is only half of what phase 4
needs; the other half is a **borrowing** route (`for x in &v`, through an
`iter(&self)`-shaped conversion), recorded as todo item 20. Flipping `list[T]`
before that exists would break every `for` loop over a list that uses the list
again afterwards, which is most of them.

## Phase 3 — literal construction through a named constructor *(done)*

`[1, 2, 3]` becomes a `list[T]` because `infer_array` says so
(`src/semantic/check.cpp:14154`). The array *value* is built, then the
`expected_is_list` branch retypes it.

**Design.** A collection opts into literal syntax by implementing:

```kira
pub trait from_array[T]:
    static def from_array[n: usize](items: array[T, n]) -> self
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

**Settled: an unannotated literal is a `list`.** `let xs = [1, 2, 3]`
produces `list[int32]`, not `array[int32, 3]`. This is a change to inference
that lands with phase 3, independently of whether `list` is still a builtin
at the time.

This is what [06-collections-list-array.md](specification/01-core/06-collections-list-array.md)
already says the language means — *"For general-purpose code, `list` is the
default choice; `array` is for when the size is fixed and known at compile
time."* Inference disagreeing with that is the bug; a fixed size is the
special case and should be the thing you have to ask for.

**One rule, not two.** No expectation → `list`, for every literal form,
including the fill form: `let zeros = [0.0; 4]` is a `list[float64]` of four
zeros. An `array` is spelled by saying so:

```kira
let xs = [1, 2, 3]                        # list[int32]
let zeros = [0.0; 4]                      # list[float64]
let rgb: array[uint8, 3] = [255, 0, 0]    # array, because it was asked for
```

Resisting a second rule for the fill form is deliberate. "A literal with a
constant repeat count is an array, otherwise a list" is a distinction nothing
else in the language draws, and it would make `[0; 4]` and `[0, 0, 0, 0]`
different types — which is exactly the kind of thing a reader has to keep in
their head rather than derive.

**Blast radius, as measured after the fact: nearly none.** The element form
(`[1, 2, 3]`) *already* inferred as a `list` when nothing was expected — only
the fill form `[0; n]` produced an `array`, so the "one rule" change was the
one-line removal of that inconsistency. No test in the tree changed behavior.
The ~110 unannotated literals counted beforehand were almost all already
lists.

**Two real compiler bugs this phase surfaced**, both pre-existing in shape
and both now fixed (todo items 17 and 18):

1. **A `static def` with generic parameters was never monomorphized.**
   `check_call_against_decl` gated instantiation on `is_free_function`, and a
   `static def` in an `extend`/`impl` block is not in `owner->functions`. A
   generic one reached lowering as its uncompiled template and both backends
   reported "call to `trio::from_array` could not be resolved to a function
   in this compiled module" — a codegen failure for correct code. This is
   entirely independent of `from_array`; any `static def f[T]` on a type hit
   it.
2. **A silent wrong-value bug on the literal's element width.** The element
   type was read from the impl method's first parameter *without* the impl
   block's type parameters in scope, so it came back abstract, and an
   abstract element is laid out at 8 bytes. The caller then wrote a
   `[10, 20, 30]` of `int32` eight bytes apart while the callee read it four
   bytes apart, and the literal came back as `10, 0, 20`. It was found by
   running the thing, not by a type error. The `int64` case was correct
   throughout — which is exactly why the test covers both widths.

**Why this ordering is safe.** Phase 3 changes what an unannotated literal
*infers to*; it does not change what a `list` is made of. At phase 3 `list`
is still the builtin, so `let xs = [1, 2, 3]` produces the same arena-backed
3-slot value it always did — no new leak, nothing waiting on drop glue. The
storage change is phase 4's alone.

**Still to do here.** The type-mismatch a fill literal now produces where an
`array` was expected is the ordinary one (``expected `array[int32, 4]`, found
`list[int32]` ``). It should say *why* the literal is a list and name the
annotation that restores the old meaning; it does not yet.

## Phase 4 — flip `list[T]`

**Done.** Steps 1-4 landed; 5 is incomplete (see below).

1. ~~Rename `vector[T]` → `list[T]`~~ **Done** — `src/std/list.kira`, with
   `impl index`/`index_mut`/`index_set`/`into_iterator`/`from_array`, and
   `drop`.
2. ~~Delete `list` from `k_builtin_generic_arities` and its four entries
   from `k_builtin_methods`~~ **Done.**
3. ~~Delete the `list` arm of `resolve_container_view` in **both**
   backends~~ **Done** (2026-09-23), along with every other builtin-`list`
   path: `is_list_type`, `compile_list_init`, the `hir_list_push` and
   `hir_mutable_cell` nodes and the `push`/`cell`/`mutable_cell` lowering
   interceptions that produced them (each fired only for a call with no
   resolved callee, and `list` now declares all three), and `op_list_push`.
   `slice`, `str` and `array` keep their arms — they are views and language
   primitives, not library types.
4. ~~Delete `list_reserve_slot` from the runtime and its LLVM declaration~~
   **Done** (2026-09-23).
5. Sweep every stdlib module written against builtin `list` — `std.algo` in
   particular, whose sorts index and swap in hot loops. **Partly done:**
   `std.algo` is clean and `src/testdata/std_test/algo_sort.kira` runs on
   both tiers. `std.io` (todo 18) and `std.iter`'s borrowing iterators
   (todo 17) are not.

Three things had to arrive with this phase, none of them anticipated above:

- **`range[T]` became an ordinary struct** (`std.traits`). `v[a..b]` on a
  library collection dispatches to `impl index[range[usize]]`, whose `at`
  *receives* the range and reads its bounds — impossible for a type that
  was only ever desugared away. `for i in a..b` and range-indexing an
  `array`/`slice`/`str` still lower their bounds directly and build no
  value, so nothing that worked before pays for it.
- **`index_mut` carries its own `output_mut`** rather than
  `cell[index.output]`. A mutable borrow is not uniformly "a cell around
  what a read yields": borrowing one element gives `cell_mut[T]`, an
  address to write through; borrowing a range gives `slice_mut[T]`, which
  is already a mutable view and wants no cell around it.
- **A pre-existing `&`/`&mut`/`*` codegen bug, found and fixed.**
  `list[T]`'s `index[range[usize]]` impl (`&mut self.data[i.start]` over
  its raw `*mut T` storage) was the first thing to ever take `&p[i]`/`*p`
  of a raw pointer whose element type is compound (heap-boxed on both
  backends: a struct, sum, tuple, or array). Both backends' `compile_unary`
  short-circuit `&x`/`*r` to a no-op whenever the referent is already
  heap-boxed — correct for an *ordinary* Kira reference (`&T`/`&mut T`),
  where a compound value's own representation already **is** its address,
  but wrong for a *raw* pointer, where `&p[i]` means "the offset address"
  (`spec/specification/03-advanced/38-machine-layer.md`) regardless of the
  pointee's representation. The shortcut conflated the two, reading a
  slot's *contents* (for compound `T`, the boxed pointer stored there)
  back as if it were the slot's own address, and vice versa for `*p`. This
  reliably crashed the bytecode VM (SIGBUS) and silently corrupted memory
  on LLVM for any `list[T]` with a compound `T` reached through
  `index[range[usize]]` — `std.algo`'s `sort_by`/`sort_by_key` over a
  `list[(int32, int32)]`, say. Fixed by gating both
  shortcuts on the referent's type, not just its heap-kind-ness
  (`src/bytecode_compiler/compile.cpp`'s and `src/llvm_codegen/codegen.cpp`'s
  `is_raw_pointer_type`). Regression:
  `src/testdata/codegen_stress/088_raw_pointer_addr_of_compound_element.kira`
  (`# expect:`, not backend agreement — both tiers had made the identical
  mistake).

**The thing that will actually hurt, and the honest answer.** Every `list`
operation becomes a real Kira call where it used to be an inlined opcode
sequence. `std.algo`'s introsort does `xs[i]`/`xs[j] = ...` in its innermost
loop; on the bytecode VM each becomes a call frame. Expect a measurable
regression there.

Do not pre-emptively design an inliner for this. Measure first: check in a
benchmark *before* phase 4 (sort 100k `int32` on both tiers, both allocator
modes) so the number is a fact rather than a fear.

**Measured (2026-09-23), after the fact.** The pre-flip number was never
taken, so it was reconstructed: `a46bd51` (the commit before the flip, with
only the LLVM 23 toolchain bump applied) against the flip-plus-cleanup tree,
both built `-c opt`, running `bench/sort_100k_int32.kira` (push-fill,
`sort` over `&mut xs[0..n]`, indexed sortedness check). Mean of 8-15 runs:

| Tier | pre-flip | post-flip | |
|---|---|---|---|
| bytecode VM (net of a `n = 2` compile-and-run baseline) | ~430 ms | ~995 ms | **~2.3x slower** |
| LLVM AOT, `-O0` | 10.8 ms | 14.2 ms | ~1.3x slower |
| LLVM AOT, `-O2` | 8.6 ms | 8.3 ms | no difference |

`KIRA_ALLOCATOR=system` and `=arena` agree to within noise on every row. The
VM regression is the one the paragraph above predicted: every `xs[i]` and
`xs.push(v)` is now a call frame. The pre-flip side also lacks every other
change between the two commits, so the VM ratio is an upper bound on what
the flip alone costs. The proportionate fix was inlining small function
bodies in HIR, which benefits every user collection and not just `list` —
the migration's value is that `list` stops being special, and buying its
performance back with a *second* special case would spend the whole point.

**Inlined (2026-09-24).** `hir::inline_small_calls` (`src/hir/inline.h`)
runs between lowering and both backends, on by default (`--no-inline` turns
it off). Same benchmark, same build:

| Tier | `--no-inline` | inlined |
|---|---|---|
| bytecode VM (net) | ~984 ms | ~462 ms — within ~7% of pre-flip |
| LLVM AOT, `-O0` | 12.2 ms | 12.4 ms |

`codegen_stress_test` runs the whole corpus a second time with inlining on
and requires the same result as without it, on both tiers
(`100_inline_call_shapes.kira` covers each rewrite shape by value).

## Phase 5 — what the migration buys, made visible *(done)*

The library type is the proof: `src/std/list.kira` carries
`impl[T] index[usize]`, `index[range[usize]]`, `index_mut`, `index_set[usize]`,
`into_iterator[T]`, `from_array[T]` and `drop` for it, and
`src/testdata/std_test/list_owned_storage.kira` builds one from a literal,
indexes it, range-indexes it, writes through the index, and iterates it —
identically on both tiers. Every one of those was impossible before this
work.

Scope-exit `drop` landed (todo item 6, for the plain-`let` shape this
exercises), and phase 4 then made the proof total: the library type is not a
demonstration alongside `list[T]` any more, it *is* `list[T]`.

## Ordering summary

```
Phase 1 (index trait)      ─┐  DONE
Phase 2 (into_iterator)    ─┼─ DONE (consuming only)
Phase 3 (from_array)       ─┘  DONE

todo item 20 (for x in &v) ─── DONE (2026-09-18)
todo item  6 (drop glue)   ─── DONE for the narrow case phase 4 needs
                                (2026-09-18) — reverse-order scope-exit drop,
                                moved-from exclusion, struct field-wise
                                drop all hold for a plain `let`/`var` in a
                                unit-tailed scope, which is what `vector[T]`
                                (a struct, plain locals) exercises. See
                                todo.md item 6 for the gaps left open
                                (value-tailed scopes, match/for/destructuring
                                pattern bindings, sum-type field drop) — none
                                of them block phase 4 below.

Phase 4 (flip list)        ─── DONE (2026-09-19). Brought three unplanned
                                things with it: `range[T]` as an ordinary
                                struct, `index_mut`'s own `output_mut`, and a
                                pre-existing `&`/`*` codegen bug for raw
                                pointers to compound elements (found via
                                `sort_by` on a tuple `list`, fixed both
                                tiers). Dead backend arms removed and the
                                benchmark taken 2026-09-23; tail is todo
                                17-19 (what the flip exposed).
Phase 5 (proof)            ─── DONE; phase 4 turned the proof into `list[T]`
```

Phases 1–3 each make the language strictly more capable on their own and were
worth landing whether or not phase 4 ever happened. That is deliberate — and
the benchmark phase 4 asked for *before* flipping was only taken afterward,
against a rebuilt pre-flip compiler (phase 4, "Measured").

## See also

- [`43-list.md`](specification/04-stdlib/collections/43-list.md) — what
  `list[T]` and `vector[T]` are today.
- [`38-machine-layer.md`](specification/03-advanced/38-machine-layer.md) —
  the substrate `vector[T]` is built on.
- [`todo.md`](todo.md) items 17-20 — what the migration left behind and
  what it exposed.
