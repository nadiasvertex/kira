# Generic Algorithms and Collections — Design

This fixes the decisions that `spec/collections-algorithms-implementation-plan.md`
codes against: the call-syntax rule that makes pipelines chain, the primitive
substrate every collection is built on, the iterator protocol, the algorithm
catalog, and the representation of each container. Written 2026-07-18.

The target, in one line:

```kira
let total = scores.values().filter(x => x > 10).map(x => x * 2).sum()
```

lazy, allocation-free until a terminal runs, uniform across every collection,
and written entirely in Kira.

## 1. Strategy (the load-bearing decisions)

1. **UFCS, not methods.** `x.f(a)` falls back to `f(x, a)` when no method `f`
   exists on `x`'s type. Every algorithm is an ordinary `pub def` free generic
   function; chaining is a resolution rule, not a type-level feature. This is
   what lets one `filter` serve `list`, `str`, `deque`, every map and set, and
   every user type, without a trait hierarchy to enroll in and without
   `extend` blocks per container. The pipeline operator `|>` is **dropped**:
   with UFCS it is a second spelling of the same desugaring and a second
   precedence table to get wrong.
2. **A minimal primitive substrate, and nothing above it.** Fourteen
   intrinsics plus one opaque builtin type. `list`, `deque`, `bitset`, every
   map and set, and eventually `str`'s interior are ordinary Kira source over
   that floor. An intrinsic is justified **only** when it (a) cannot be
   expressed on the primitive set, (b) requires OS or platform access, or (c)
   has a measured performance cliff. This rule is retroactive — see §7.
3. **Reuse the specced `machine` layer.** Raw pointers, `transmute`, and
   `uninit[T, N]` are already designed in `kira-reference.md` ("The `machine`
   Layer"). This work gives that design semantics rather than inventing a
   competing one. `machine` acquires exactly one meaning: it gates raw-pointer
   *operations* and primitive intrinsics.
4. **Three iteration families up front.** `iter()` yields `&T`, `iter_mut()`
   yields `&mut T`, `into_iter()` yields `T`. Maps yield tuples. Deferring
   `iter_mut` would mean rebuilding every adapter later.
5. **Adapters are generic structs with generic trait impls.** No new
   declaration form. `impl[I, T, U] iterator[U] for map_iter[I, T, U]` already
   parses and registers; the missing piece is one substitution rule in the
   checker (§2).
6. **Everything is monomorphic.** There are no trait objects in Kira today
   (`box[Trait]` is specced with zero implementation). A pipeline's type is
   the full nest of its adapter types, erased at compile time. `some
   iterator[T]` in return position is the escape valve for naming one.

## 2. The one crux: impl-level type parameter substitution

Everything in this design rests on a generic adapter's `next` returning a
concrete type. Today it does not, and the reason is narrow enough to state
exactly.

`impl_decl` carries `type_params` (`src/parser/ast.h:1698`), the grammar
admits `"impl" [ type_params ] named_type "for" type_expr`
(`kira-grammar.ebnf:291`), `check_impl_decl` calls `push_type_params`, and
`build_method_table` keys `methods_` on the target `type_decl` node. So

```kira
impl[I, T, U] iterator[U] for map_iter[I, T, U]:
    def next(mut self) -> option[U]: ...
```

**registers correctly right now.** But:

> **Gap G1.** Impl-level type parameters are never substituted at the call
> site. `check_generic_instance_method_call` (`check.cpp:7085`) gates on
> `is_generic_template(*method.decl)` — the *method's* own `[T]`, not its
> impl's. A `next` whose `U` comes from the impl header returns `U`
> unsubstituted, which degrades to `k_unknown_type`. The identical hole exists
> in `try_resolve_iterator` (`check.cpp:9280`), which reads
> `signature_return_type` with no receiver substitution at all.

Left unfixed, `for x in nums.iter().map(f):` binds `x: unknown` and then
type-checks anything at all — precisely the silent-acceptance hazard this
work exists to remove.

### 2.1 Gap G0, found by spike — strictly larger than G1

Spikes run 2026-07-18 (`scratchpad/spikes/s1`–`s5`) establish that G1 is not
the whole story, and not even the first thing to fix:

| Spike | Shape | Result |
|---|---|---|
| `s1` | generic struct `pair[A, B]`, no impl | **works** (returns 7) |
| `s5` | `impl get_it for holder` — non-generic target | **works** (returns 7) |
| `s4` | `impl get_it for holder[int32]` — non-generic impl, **generic target** | fails to lower |
| `s3` | `impl[T] get_it[T] for holder[T]` — generic impl | fails to lower |
| `g1` | `impl[I,T,U] iterator[U] for map_iter[I,T,U]` in a `for` loop | fails to lower |

> **Gap G0.** Methods in an `impl` whose **target type is generic** are never
> lowered, on either backend. The determining factor is the genericity of the
> target type, not whether the impl carries type parameters — `s4` has no impl
> type params and still fails. `s4` reports it plainly:
> `call to `holder::get` could not be resolved to a function in this compiled
> module`. `s3`/`g1` fail more obscurely, with `main` itself failing to lower
> and the user seeing only `no compiled module defines a function named main`.

This is why `codegen_stress/037` passes while every adapter in this design
would not: `counter` is a non-generic type. The failure is in shared lowering
rather than either backend — LLVM and the VM report the same two errors.

Two consequences for the plan:

1. **G0 precedes G1 and is the true blocker.** Substituting impl-level type
   parameters (G1) is worth nothing until methods on generic targets are
   emitted at all. Phase 1 must instantiate an impl's methods per concrete
   target type — the same monomorphization the existing
   `instantiate_generic_function` performs for free functions, applied to impl
   members keyed on the target's type arguments.
2. **The silent-failure mode is itself a defect.** A program that passes
   semantic analysis and then produces no `main` is exactly the "compiler as
   teacher" failure this repository's conventions forbid. Whatever the fix
   costs, lowering must report *which* declaration it could not emit and why,
   rather than leaving the driver to discover a missing `main`.

Two risks are also partly retired: `s1` shows generic user structs construct
and field-access correctly (R3 is milder than feared), and `s5` confirms the
non-generic impl path is sound, so G0 is a gap in generic instantiation rather
than in method dispatch generally.

**The fix.** When a resolved method's owning impl has `type_params`, build a
substitution by rigid-unifying the impl's `for_type` pattern
(`map_iter[I, T, U]`) against the concrete receiver type
(`map_iter[list_iter[int32], int32, str]`), then apply it to the method's
parameter and return types. `unify_rigid` (`check.cpp:6667`) already performs
exactly this shape of match. Concretely:

- Generalize the `check_generic_instance_method_call` trigger to
  `is_generic_template(*method.decl) || impl has type_params`, seeding
  `bindings` from receiver unification before argument solving.
- Give `method_entry` (`check.cpp:147`) a back-pointer to its `impl_decl`; it
  currently carries `decl`, `owner`, `from_trait`, `is_extension`, and
  `fixed_type_params`, none of which reach the impl header.
- Route `try_resolve_iterator` through the same receiver-unification helper so
  the structural `for`-loop path sees the concrete element type.

No grammar change. No `extend` change. No inherent generic impls.

## 3. UFCS

### 3.1 Resolution

For `recv.f(a₁..aₙ)` with `recv: R`, the existing lookup ladder in
`infer_method_call` (`check.cpp:7228`) runs unchanged — inherent method,
trait-impl method or monomorphized trait default, `extend` method, builtin
method — and UFCS is appended as a fourth arm, **strictly last**:

1. Collect free `func_decl`s named `f` visible in scope (current module,
   `use`d modules, prelude) that are UFCS-eligible.
2. Filter to those whose first parameter accepts `R` under receiver
   adaptation (§3.2).
3. Exactly one → rewrite as `f(recv, a₁..aₙ)` and delegate to the ordinary
   free-call path (`check_call_args_against` → `solve_from_argument_types` →
   `instantiate_generic_function`), recording the callee so HIR lowering emits
   a plain call. Zero → enriched not-found error. More than one → ambiguity
   error.

Being strictly last is the safety property that makes this shippable: UFCS can
never shadow a method, so adding a free function to the standard library can
never silently change the meaning of code that already compiles.

Note that half of this already exists. `receiver_fills_first_param`
(`check.cpp:7129`) and `check_receiver_call` (`check.cpp:7156`) implement
"the receiver fills the first declared parameter, and the callee's type
parameters are solved by rigid unification starting from the receiver" — UFCS
semantics, merely restricted to methods found in an `impl`/`extend` block.
The fourth arm reuses that machinery rather than adding any.

### 3.2 Receiver adaptation

First match wins:

| First parameter declared | Accepts |
|---|---|
| `T` by value | `R`; or `&R`/`&mut R` (auto-deref) |
| `&T` | `R` when `recv` is an lvalue (auto-ref); `&R`; `&mut R` (reborrow) |
| `&mut T` | `R` when `recv` is a **mutable** lvalue; `&mut R` |
| unconstrained generic `I` | anything — unification binds `I := R₀` |

`&mut T` against a non-`mut` binding is a **hard error**, never a silently
skipped candidate. Otherwise `v.iter_mut()` on a `let` binding collapses into
a baffling "no method" message.

### 3.3 Eligibility

> A free function is UFCS-eligible if it is `pub`, has at least one parameter,
> and its first parameter is not named `self`.

No opt-in marker. Requiring one would make `std.algo` special in a way user
code could not mirror, and the whole point is that algorithms are ordinary
functions. `#[no_ufcs]` is held in reserve as an opt-out if the candidate pool
proves noisy in practice; it is not built in the first phase.

### 3.4 Diagnostics

Three messages carry this feature. Per `CLAUDE.md`, each says what was
expected, what was found, and how to fix it.

**No candidate.** Suggestions are drawn from (methods on `R₀`) ∪
(UFCS-eligible names in scope), ranked by edit distance. The pool *grows*
under UFCS, so typo detection gets better, not worse:

```
error: no method `fliter` on type `list[int32]`
  --> pipeline.kira:12:15
   |
12 |     nums.fliter(x => x > 0)
   |          ^^^^^^ method not found
   = note: `list[int32]` provides len, push, pop, iter, iter_mut, into_iter
   = help: did you mean `filter`? `std.algo.filter` is callable as a method
           on any `iterator[T]` via `it.filter(pred)`.
```

**Name exists, receiver does not fit.** The highest-value message here,
because this is what a forgotten `.iter()` looks like:

```
error: `sum` cannot be called on `list[int32]`
  --> pipeline.kira:14:10
   |
14 |     nums.sum()
   |          ^^^ no `sum` accepts a `list[int32]` as its first argument
   = note: `std.algo.sum[I, T](it: I) -> T` requires `I: iterator[T]`
   = help: `list[T]` is not itself an iterator. Call `.iter()` first:
           `nums.iter().sum()`
```

**Ambiguous.** Every candidate is listed with module path and signature, and
the fix is the qualified form `std.algo.filter(nums, p)`. There is no
"most specific wins" tie-break: ambiguity is the user's to resolve.

## 4. The primitive substrate

### 4.1 What `machine` means

`is_machine` is presently a parse-only flag (`parser.cpp:2344`). It acquires
exactly one job:

> **`machine` gates primitive intrinsics and raw-pointer operations.** Inside
> a `machine def`, `*T` and `*mut T` may be dereferenced, offset, compared,
> and passed to the primitives below. Outside, a raw pointer is an opaque
> value: storable in a struct field and passable, but not dereferenceable.

Non-`machine` code may freely **call** `machine` functions. This is what the
reference already specifies — "the unsafety is contained entirely inside
them" — and it is what allows `list[T]` to hold a `*mut T` and still present a
fully safe public API. The function, not a block, is the unit of unsafety;
that is deliberately coarser than Rust and it fits the compiler's teaching
posture, since the diagnostic is always "this needs a `machine def`" rather
than "wrap this in a block".

Raw pointer *types* are not gated, only operations. Gating the type would
force every collection struct into `machine` and destroy the containment
property that makes the whole design safe.

Checker work is one predicate, `in_machine_fn_`, set on entry to a `func_decl`
with `is_machine` and consulted by unary `*`, pointer arithmetic, and the
intrinsic call path:

```
error: cannot dereference a raw pointer here
   = note: `*mut int32` may only be read or written inside a `machine def`
   = help: mark this function `machine def load(p: *int32) -> int32:`, or use
           a safe wrapper such as `slice_from_raw(p, len)`.
```

### 4.2 The fourteen

New module `std.machine`, declared in the two-layer style of `std.string`:

```kira
module std.machine

# --- compile-time layout queries (const-evaluated; no runtime call) ---
intrinsic def size_of[T]() -> usize
intrinsic def align_of[T]() -> usize

# --- heap: the gap uninit[T, N] does not cover ---
#: Returns a null pointer on failure. `align` must be a power of two.
machine intrinsic def alloc_raw(size: usize, align: usize) -> *mut byte
machine intrinsic def realloc_raw(p: *mut byte, old_size: usize,
                                  new_size: usize, align: usize) -> *mut byte
machine intrinsic def free_raw(p: *mut byte, size: usize, align: usize) -> unit

# --- pointer access ---
machine intrinsic def ptr_read[T](p: *T) -> T
machine intrinsic def ptr_write[T](p: *mut T, value: T) -> unit
machine intrinsic def ptr_offset[T](p: *T, n: usize) -> *T
machine intrinsic def ptr_offset_mut[T](p: *mut T, n: usize) -> *mut T
machine intrinsic def ptr_copy[T](dst: *mut T, src: *T, count: usize) -> unit
machine intrinsic def ptr_null[T]() -> *mut T
machine intrinsic def ptr_is_null[T](p: *T) -> bool

# --- escape hatch ---
machine intrinsic def transmute[A, B](value: A) -> B

# --- drop glue: unavoidable for any owning container ---
machine intrinsic def drop_in_place[T](p: *mut T) -> unit
```

plus the two safe bridges that are the only way pointers become usable values
in ordinary code:

```kira
machine def slice_from_raw[T](p: *T, len: usize) -> slice[T]
machine def slice_mut_from_raw[T](p: *mut T, len: usize) -> slice_mut[T]
```

Deliberate omissions, each because the primitive set already reaches it: no
memset (a loop over `ptr_write`; LLVM recognizes the idiom and the VM does not
care), no popcount (SWAR in Kira — see §6.3), no SIMD.

`free_raw` takes size and align — sized deallocation — so the VM can back the
allocator with a bump or pool arena carrying no header word, while the AOT
runtime maps it to `free`/`_aligned_free` by ignoring the extra arguments.

### 4.3 `uninit[T, N]`

Retained exactly as `kira-reference.md` specs it, and **not** made redundant
by the allocator: it is inline, fixed-capacity storage, the thing that lets
`small_list[T, N]` avoid heap traffic entirely for the small case. Implement
it as an opaque builtin type laid out as `align_of[T]`-aligned
`N * size_of[T]` bytes.

Its five accessors — `slot_ptr`, `write_slot`, `read_slot`, `drop_first`,
`as_slice` — are then *derivable* from the fourteen and become ordinary
`machine def`s in `std.machine`, not new intrinsics. `drop_first` is a loop
over `drop_in_place`. Only the opaque type itself is compiler work.

## 5. The iterator protocol

### 5.1 Traits

`std.iter` grows from ten lines to:

```kira
module std.iter

pub trait iterator[T]:
    def next(mut self) -> option[T]

    #: Lower bound on remaining elements. Adapters refine this; the default
    #: is the always-correct answer.
    def size_hint(self) -> usize:
        return 0

pub trait double_ended[T] requires iterator[T]:
    def next_back(mut self) -> option[T]

pub trait exact_size[T] requires iterator[T]:
    def exact_len(self) -> usize

pub trait into_iterator[T]:
    type iter
    def into_iter(self) -> self.iter
```

Notes on each choice:

- `size_hint` is a **defaulted method, not a trait**. Trait defaults already
  monomorphize correctly, it only affects `collect` pre-sizing, and a
  conservative wrong answer is harmless.
- `double_ended` earns its place immediately: it is what makes `rev`, `last`,
  and `rfind` non-quadratic.
- `exact_len`, not `len` — under UFCS a trait method named `len` would collide
  with the free `len` function on every container.
- **`from_iter` is a trait, and `collect` is unified.** An earlier revision
  of this document deferred it, on the grounds that `collect[C: from_iter]`
  needs both call-site bound enforcement and return-type-driven inference and
  Kira has neither. The first half was wrong: unenforced bounds affect the
  *quality of the error* when `C` is unsuitable, not whether `collect`
  computes the right answer (§8, R1, and see below). The real blockers were
  return-position inference and two smaller gaps, all three of which are
  designed in `spec/generic-inference-design.md` and sequenced ahead of this
  work. `collect` therefore ships as one function:

  ```kira
  pub trait from_iter[T]:
      static def from_iter[I: iterator[T]](it: I) -> self

  pub def collect[I, C](self: I) -> C:
      return C.from_iter(self)
  ```

  This snippet did not parse when it was written. `static def` was accepted
  only at module level: inside a `trait` body the parser read `static` as the
  start of a static *constant* (`static NAME: type`) and reported ``expected
  a name but found `def` ``, and inside an `extend` block it reported
  ``expected an extend member (function) but found `static` ``.
  `kira-grammar.ebnf` lists `static` among the function modifiers with no
  such restriction, so the parser was the side that disagreed with the
  intent. Fixed 2026-07-19: every construct accepting both forms now looks
  past the run of modifiers for a `def` before committing
  (`parser::at_static_func_decl`), which is the disambiguation module scope
  already performed — by hand, unrolled to three tokens, in two copies.
  `trait`, `impl`, and `extend` bodies now take `static def` on the same
  terms. `codegen_stress/051_static_trait_method.kira` covers the whole
  `collect` shape, not just the parse.

  It infers from the expected type — `let names: list[str] = it.collect()` —
  and takes an explicit argument where no expected type exists:
  `it.collect[list[str]]()`. Named collectors are not shipped; they would be
  a second way to spell the same thing.

  The one honest caveat, which the standard library documentation should
  state plainly: **`collect` is inferred, not bounds-checked.** A `C` that is
  not a collection is a compile-time error at the point of use, but phrased
  indirectly — it surfaces as a missing `from_iter` inside `std.iter`, with
  the instantiation-site note of R1 pointing back at the user's line. That
  note is a hard requirement for this feature rather than the nice-to-have
  R1 treats it as, because for `collect` an unsuitable `C` is the *common*
  failure and the user never wrote a call naming `from_iter` at all.
- `into_iterator` exists but `for` does not require it. `for` already resolves
  structurally through `try_resolve_iterator`; `element_type_of` is extended
  so that a receiver which is not itself an iterator desugars through
  `into_iter`. Keeping the structural path primary preserves every existing
  test.

### 5.2 The three families

Free functions, made chainable by UFCS:

```kira
pub def iter[T](self: &list[T]) -> list_iter[T]
pub def iter_mut[T](self: &mut list[T]) -> list_iter_mut[T]
pub def into_iter[T](self: list[T]) -> list_into_iter[T]
```

Each collection ships three iterator structs. Maps yield tuples: `iter` on
`ordered_map[K, V]` yields `(&K, &V)`, `into_iter` yields `(K, V)`.

### 5.3 How adapters are written

Plain generic structs plus generic trait impls — no new declaration form, and
no `extend` type parameters:

```kira
module std.iter

pub type map_iter[I, T, U] = { inner: I, f: fn(T) -> U }

impl[I, T, U] iterator[U] for map_iter[I, T, U]:
    def next(mut self) -> option[U]:
        match self.inner.next():
            @some(x) => return @some((self.f)(x))
            @none => return @none

pub type filter_iter[I, T] = { inner: I, pred: fn(T) -> bool }

impl[I, T] iterator[T] for filter_iter[I, T]:
    def next(mut self) -> option[T]:
        while let @some(x) = self.inner.next():
            if (self.pred)(x):
                return @some(x)
        return @none
```

Three points of grammar, since an earlier revision of this section got all
three wrong and none of the code in it compiled:

- A match arm is `pattern => result`, not `case pattern:`.
- A match arm's result is a single expression or statement. There is no
  indented block form, which is why `filter_iter` is written as a `while let`
  rather than as a `match` with a conditional arm.
- A struct literal never takes type arguments: it is `map_iter { ... }`, with
  the parameters solved from the field values, not `map_iter[I, T, U] { ... }`.

The adapter *constructors* are free functions, and UFCS turns them into
chaining:

```kira
module std.algo

pub def map[I, T, U](it: I, f: fn(T) -> U) -> map_iter[I, T, U] where I: iterator[T]:
    return map_iter { inner: it, f: f }

pub def filter[I, T](it: I, pred: fn(T) -> bool) -> filter_iter[I, T] where I: iterator[T]:
    return filter_iter { inner: it, pred: pred }
```

**The `where` clause is load-bearing, not documentation.** This is the one
place §6.1's "bounds are advisory only" does not hold, and the earlier claim
that this needed "no further language work" was wrong. Nothing about the
arguments can solve `T`: `it` answers `I`, the lambda is what `T` is needed
*for*, and no other parameter mentions it. The bound is the only link between
`I` and `T` — given `I := counter` and `impl iterator[int32] for counter`, it
answers `T := int32`. Without it, `nums.map(x => x * 2)` reports `cannot tell
what `T` is in this call to `map``, and `sum`, which takes no written argument
at all, is unsolvable outright.

Bounds remain unenforced in the sense R1 describes — an unsatisfied bound is
not diagnosed at the call site — but they are now *consulted*, by
`solve_from_bounds` in `check.cpp`. A bound can turn an unsolvable call into a
solved one and can never re-answer one the arguments already settled.

The second half is argument ordering. A lambda has no type of its own until
one is expected of it, so `x => x * 2` checked in declaration order binds its
parameter to the type *parameter* `T` and reaches lowering as an abstract
type. Lambda arguments are therefore checked last, against a parameter type
substituted with everything the other arguments and the bounds have solved.
That is what allows a bare `x => x * 2` where an annotated
`(x: int32) => x * 2` would otherwise be required.

With those two mechanisms,
`scores.values().filter(p).map(f).sum()` type-checks to a concrete type and
runs on both backends. `codegen_stress/050_bound_driven_inference.kira` is
this exact chain.

## 6. Catalog and containers

### 6.1 Algorithms

`std.algo`, all free generic functions. Bounds are written in `where` clauses
for documentation and for future enforcement; today they are **advisory only**
(§8, R1).

**Lazy adapters.** `map`, `filter`, `filter_map`, `take`, `skip`,
`take_while`, `skip_while`, `step_by`, `zip`, `enumerate`, `chain`,
`flat_map`, `flatten`, `rev` (`I: double_ended[T]`), `peekable`, `scan`,
`inspect`, `unique` (`T: hash + eq`).

**Terminals.** `count`, `fold`, `reduce`, `for_each`, `sum` (`T: add`),
`product` (`T: mul`), `any`, `all`, `find`, `find_map`, `position`, `last`,
`nth`, `min`/`max` (`T: ord`), `min_by`/`max_by`, `min_by_key`/`max_by_key`,
`collect` (§5.1; one function, inferred from the expected type),
`partition`, `group_by`, `eq_by`, `cmp_by`.

**Slice algorithms**, in-place and not iterator-based: `sort` (`T: ord`),
`sort_by`, `sort_by_key`, `sorted`, `stable_sort`, `binary_search`,
`binary_search_by`, `reverse`, `rotate`, `dedup`, `partition_point`,
`lower_bound`, `upper_bound`. This is where `sort` finally acquires a
comparator and a connection to `ord`; today it has neither.

`windows` and `chunks` live here too, as **slice** operations rather than
iterator adapters. A general iterator cannot produce overlapping views without
buffering, and the buffering version has a materially different cost — putting
them on slices keeps the complexity honest.

### 6.2 `list[T]`

`{ data: *mut T, len: usize, cap: usize }`, ×2 growth from an initial 4.
`push` amortized O(1), index O(1), `insert`/`remove` O(n).

Migration off `builtin_method_result` is the riskiest single item in the plan
and must proceed in three separate steps, never one:

- **Additive.** Leave `list` a builtin generic. Add free functions (`iter`,
  `iter_mut`, `into_iter`, and the whole catalog) reachable by UFCS. Nothing
  breaks; `builtin_method_result` is untouched. This alone delivers most of
  the user-visible value.
- **Close the silent hole.** Change the `list` arm of `builtin_method_result`
  to report a real diagnostic for unrecognized names *after* UFCS lookup has
  been tried, instead of returning `k_unknown_type` and accepting silently.
  Then `slice`, `option`, `result`, one arm at a time. Expect fallout in
  existing testdata — finding it is the purpose.
- **Denaturalize.** Remove `list` from `k_builtin_generic_arities`
  (`types.cpp:32`) and define it in Kira as
  `pub type list[T] = { data: *mut T, len: usize, cap: usize }`,
  prelude-imported. This additionally requires array-literal lowering
  (`[1,2,3]` → `list_from_array`), routing `for` over `list` through
  `into_iterator` rather than `element_type_of`'s hardcoded arm, and an
  `index` trait with an operator hook for `v[i]` (no such trait exists today).
  This is the largest single chunk of compiler work in the whole effort and
  belongs last.

### 6.3 `deque[T]`, `bitset`

**`deque[T]`** — ring buffer over a power-of-two-capacity allocation:
`{ data: *mut T, head: usize, len: usize, cap: usize }`, masking instead of
modulo. `push_front`/`push_back`/`pop_front`/`pop_back` amortized O(1), index
O(1). Not contiguous, so no `as_slice`; offer `make_contiguous` instead.

**`bitset`** — `{ words: list[uint64], nbits: usize }`. `set`, `clear`,
`flip`, `test`, `count_ones` (SWAR, no intrinsic), `union`, `intersect`,
`difference`; `iter()` yields set-bit indices by trailing-zero scanning. The
set operations need bit-operator traits (`bit_and`, `bit_or`, `bit_xor`,
`shl`, `shr`), which do not exist — add them to `std.traits` following the
existing `add`/`type output` pattern. Mechanical, and it unblocks user numeric
types at the same time.

### 6.4 `ordered_map[K, V]`, `ordered_set[K]`

**Sorted vector first, B-tree second.** Phase one is
`{ keys: list[K], vals: list[V] }`: lookup O(log n) by binary search,
insert/remove O(n) by memmove. Roughly 150 lines, it exercises `ord` end to
end, and below about a thousand elements it outperforms a B-tree anyway. It
ships the complete API immediately.

Phase two replaces the internals behind an unchanged API with a **B-tree**,
B = 6 (five to eleven keys per node). Chosen over a red-black tree because
node payloads are `array[K, 11]` plus a length, which the existing
const-generic support already handles, whereas a red-black tree needs
option-of-node-pointer chasing and colour invariants that are considerably
harder to get right without a borrow checker.

API: `insert`, `get`, `get_mut`, `remove`, `contains`, `len`, `first`, `last`,
`range(lo, hi)`, `keys()`, `values()`, `values_mut()`, and the three iterator
families yielding tuples in key order. Bound `K: ord`.

### 6.5 `unordered_map[K, V]`, `unordered_set[K]`

**Open addressing, linear probing, Robin Hood with backward-shift deletion.**

```kira
pub type unordered_map[K, V] = {
    ctrl: *mut uint8,   # 0 = empty, otherwise 1 + probe distance
    keys: *mut K,
    vals: *mut V,
    len: usize,
    cap: usize,         # power of two
}
```

Backward-shift deletion rather than tombstones, because tombstone degradation
under repeated insert/remove is the failure mode users actually hit. Power-of-
two capacity with mask indexing, growth at 7/8 load factor. Average O(1),
worst case O(n).

Hashing comes from `K: hash` (which returns `uint64`), but a fibonacci-
multiply finalizer is applied before masking: user `hash` implementations will
be low-quality, and linear probing is unforgiving of clustered hashes.

No SIMD group-scan (Swiss-table style). It would require SIMD intrinsics that
do not exist and would be the only part of the standard library not writable
in portable Kira.

### 6.6 `str`

Representation and the existing `rt_str_*` intrinsics are unchanged here;
their migration is deferred (§7). Added now:

- `str_builder` — `{ buf: list[uint8] }` with `push_str`, `push_char`,
  `build`. This is what `join` targets, and what `str`'s `from_iter` impl
  builds through so `collect[str]` costs one allocation pass.
- `chars()`, `bytes()`, `char_indices()` as iterators, written in Kira over
  `as_bytes()` with UTF-8 decoding. New, and Kira-side from day one — no new
  intrinsics.
- `split(sep)`, `lines()` as lazy iterators, replacing the eager equivalents.

## 7. Retroactive scope: the existing intrinsics

The rule in §1.2 is not forward-only. Measured against it, the current
thirty-three intrinsics divide:

- `rt_str_to_upper`, `rt_str_to_lower`, `rt_str_reverse`, `rt_str_replace`,
  `rt_str_trim`, `rt_str_find` — **fail (a)**: all are expressible on the
  primitive set. They survive only pending a (c) measurement. Scheduled for
  migration, with the Kira implementations benchmarked against the intrinsics
  before the intrinsics are removed.
- `rt_fmt_f64_fixed`, `rt_fmt_f64_sci`, `rt_fmt_f64_general` — **pass (a)**.
  Correct float formatting is Ryū or Grisu, which is not stdlib-in-Kira work
  at this stage. These stay.
- `rt_stdin`/`rt_open`/`rt_read`/`rt_write`, `rt_uname`, `rt_gethostname`, and
  the rest of `std.io` and `std.platform` — **pass (b)**. These stay.

## 8. Risks

**R1 — Generic bounds are unenforced (highest impact).** Every `where T: ord`
in this document is decoration. Bounds resolve to `k_unknown_type` by design
(`check.cpp:3665`, `:3691`) and guide method lookup only; violations surface
as errors *inside* the instantiated body, pointing at standard library source
rather than user source. UFCS makes this worse, because the user never wrote
the call that failed.

The cheap, mandatory mitigation: when an error is reported inside a generic
instance body, append a note naming the instantiation site and the
substitution. `find_or_check_generic_instance` already knows both.

```
error: no method `add` on type `str`
  --> std/algo.kira:88:20   (in `sum[list_iter[str], str]`)
   = note: instantiated from pipeline.kira:12:30 — `names.iter().sum()`
   = help: `sum` requires `T: add`; `str` does not implement `add`.
           Use `.fold(str_builder(), append)` to concatenate strings.
```

That single note converts the worst symptom from incomprehensible to merely
indirect. Real call-site bound checking is a large project and out of scope
here — but this design is what will make its absence painful enough to
prioritize.

**R2 — The bytecode VM has no reference or pointer model at all.** Not "barely
exercised" — absent. Spikes `m1`/`m2` (2026-07-18):

```
m2:  return *x            → operator `*` needs a reference/pointer model
                            this bytecode compiler doesn't have yet
m1:  *x = *x + 1          → assigning to an index target needs a heap/aggregate
                            representation this bytecode compiler doesn't have yet
```

Merely *reading* through a `&mut int32` fails. This is the single largest
finding of the spike phase and it resizes the plan in two places:

- `iter_mut` is undeliverable until the VM gains a pointer model. Fallback
  stands: ship `iter` and `into_iter` first, making the three-family
  requirement two-plus-one, and treat `iter_mut` as gated on this work.
- **More seriously, the entire primitive substrate of §4 is built on `*T` and
  `*mut T` dereferences.** `ptr_read`, `ptr_write`, and every collection above
  them presuppose exactly the model the VM lacks. The `machine` phase is
  therefore not "give `is_machine` semantics and add fourteen intrinsics" — it
  is "build a reference/pointer model in the bytecode compiler," with the
  intrinsics as the easy part on top. It should be re-estimated accordingly
  and sequenced with that understanding, and it is the reason the stdlib-side
  phases are deliberately ordered to deliver value before it lands: the
  iterator protocol and algorithm catalog over the existing builtin `list`
  need no pointers at all.

**R3 — Generic user structs work; impls on them do not.** Partly retired by
spike `s1`: a generic struct constructs, infers its type arguments from field
values, and field-accesses correctly. The residual risk is entirely G0 (§2.1),
which is sharper and better understood than "thinly tested." The deep-nesting
canary (R5) is still worth running once G0 is fixed.

**R3a — Tuple destructuring works in `let`, not in `for` patterns.** Spikes
`t1`/`t2`/`t3`: `let (a, b) = p` works; a tuple-typed trait argument
(`iterator[(int32, int32)]`) works; binding the whole tuple in `for` and
destructuring in the body works (`t3` returns 6). Only `for (k, v) in it:`
fails. Since every map iterator in this design yields tuples, this is
directly on the critical path — but it is a narrow, well-isolated pattern-
lowering gap with a working desugaring already demonstrated, not a structural
problem.

**R4 — No trait objects.** Assumed and correct. Consequences to document: no
heterogeneous iterator storage, and no `box[iterator[T]]` to break type
recursion. `some iterator[T]` in return position is the one ergonomic escape
valve for users who want to name a pipeline's type without writing
`filter_iter[map_iter[list_iter[int32], ...], ...]`, and it should be tested
early.

**R5 — Monomorphization blowup on deep pipelines (most likely to bite).** A
five-stage pipeline is a type like
`take_iter[enumerate_iter[filter_iter[map_iter[list_iter[int32], int32, str], str], str], ...]`.
Four concerns, each with a concrete check:

- *Checker cost.* Each adapter's `next` is re-checked once per distinct inner
  type — linear in distinct pipelines, not exponential, **provided the
  instance cache keys on the full substitution**. Verify the key includes
  nested type arguments rather than a rendered suffix string.
- *Symbol length.* Mangled suffixes are built by string concatenation; a
  depth-8 pipeline yields kilobyte symbol names, which some linkers reject.
  Hash suffixes past a length threshold, keeping the readable prefix.
- *Recursion depth.* Confirm `generic_depth_` is at least 16 and that
  exceeding it produces a teaching diagnostic ("this pipeline is N adapters
  deep") rather than a generic recursion error.
- *Backends.* LLVM will inline the whole chain, which is how these reach zero
  cost, but IR size grows multiplicatively. An eight-stage `codegen_stress`
  case with **measured compile time** is worth having before the standard
  library is written, not after.

**R6 — UFCS namespace pollution.** Every `pub` free function in every imported
module becomes a method candidate, and the prelude auto-imports `std.traits`,
`std.iter`, `std.console`, plus a new `std.algo`. Ambiguity errors will appear
in real code. Mitigated by strict-last precedence (working code can never
break), by ambiguity always being a hard error with a qualified-path fix, and
by `#[no_ufcs]` held in reserve.

**R7 — Closing the silent hole breaks code by design.** Turning
`k_unknown_type` into a diagnosed error on `list` will surface real breakage.
Sequence it after the algorithms exist, so replacements are available before
the hardcoded methods are removed.
