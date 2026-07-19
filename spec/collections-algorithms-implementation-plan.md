# Generic Algorithms and Collections — Implementation Plan

The sequencing `spec/collections-algorithms-design.md` codes against. The
design fixes *what* is built; this fixes *in what order*, *how each phase is
proven*, and *what "done" means*. Written 2026-07-18.

Standing rule for this effort, from the user: **no partial implementation.**
Where a language gap or compiler bug blocks a piece of the design, the gap is
fixed rather than routed around. In particular `iter_mut` is **not** deferred
(reversing the fallback in design §R2), and R3a is fixed rather than left to
a body-side destructuring workaround.

## 0. Verified starting state

Re-measured on `master` @ `6e232dc` (2026-07-18), not inherited from the
design's spike table. Spikes in `scratchpad/spikes`.

| Claim | Status | Evidence |
|---|---|---|
| G0 — methods on generic target types never lower | **open** | `s4` (non-generic impl, generic target): `call to holder::get could not be resolved to a function in this compiled module`. `s3` (generic impl): silent, `no compiled module defines a function named main` |
| G1 — impl-level type params never substituted | **open** | `g1_type`: `let s: str = h.get()` on `impl[T] get_it[T] for holder[T]` is **silently accepted**; identical code on a non-generic target correctly errors `expected str, found int32` |
| R3a — `for (k, v) in it:` | **open, silent** | `r3a` produces no diagnostic at all, only a missing `main`. The `t3` workaround (bind tuple, destructure in body) compiles and runs |
| R3 — generic user structs | **works** | `s1` constructs, infers, field-accesses |
| R2 — no pointer/reference model | **open, and worse than documented** | The design says "the bytecode VM". Both backends fail: `compile.cpp:1136` and `codegen.cpp:1535` emit the same message. Merely reading `*x` through a `&mut int32` fails on LLVM too |

Two corrections to the design follow from this:

- **R2 is not a VM-only risk.** `llvm_codegen` lacks the model as well, so
  the pointer work is not "the VM catches up to LLVM" — it is a genuinely
  new capability in shared lowering plus both backends.
- **The type system is already ready.** `ref_kind` and `ptr_kind` exist in
  `types.h:48-49` and `&T`/`*mut T` check fine. The gap is entirely in HIR
  lowering and codegen, which makes Phase 0 tractable and localized.

Favourable finding: the prerequisites from `spec/generic-inference-design.md`
(G1/G2/G3 *of that document* — return-position inference, explicit type args
on method calls, static dispatch through a type parameter) are **landed**:
`solve_from_expected_type` (`check.cpp:4925`), `method_explicit_generic_args`
(`check.cpp:7518`), `static def` dispatch (`check.cpp:8230`), with
`codegen_stress/039_type_param_static_dispatch.kira` covering the last. So
`collect` ships in its unified form as designed, with no preliminary work.

Note: `CLAUDE.md` and the design both cite `src/cli.cpp`; the driver now
lives at `src/driver/cli.cpp`. Line references into `check.cpp` (14090 lines)
predate commits `62e5b1d`/`24f9fd4` and should be re-located by symbol name,
not trusted as offsets.

## 1. Cross-cutting requirements

These are not a phase; each applies to every phase and is part of that
phase's exit criteria.

**X1 — Lowering must never fail silently.** Three of the five spikes above
report nothing and leave the user with `no compiled module defines a function
named main`. Per design §2.1.2 and `CLAUDE.md`, every lowering bail-out must
name the declaration it could not emit and why. **Landed in Phase 1** — see
that phase for what turning it on revealed.

**X2 — The instantiation-site note (design R1).** When an error is reported
inside a generic instance body, append a note naming the instantiation site
and the substitution. Mandatory before Phase 5 ships `collect`, where an
unsuitable `C` is the common failure and the user never wrote a call naming
`from_iter`.

**X4 — Prelude-name shadowing is undiagnosed.** Found while spiking Phase 0:
declaring `type box = { ... }` silently loses to the prelude `box`, and the
resulting mismatch reports ``expected `box`, found `box` `` — two distinct
`type_id`s (`builtin_generic_kind` with a null decl vs `struct_kind`) that
display identically. This is exactly the diagnostic `CLAUDE.md` forbids. Not
a blocker for any phase here, but it cost real debugging time and will cost
users more; a shadowing note at the declaration, plus a display that
distinguishes the two, is owed. Fix opportunistically, before the containers
of Phase 7 add many more prelude names to collide with.

**X3 — Regression test per behavior change.** Parser/semantic changes get a
`check_test`/`parser_test` case; lowering changes get a `codegen_stress`
case run on **both** backends.

## 2. Phases

### Phase 0 — Reference and pointer model

**Scope corrected 2026-07-18 after measurement — this is substantially
smaller than estimated above.** Both backends already represent every
aggregate (struct, sum, tuple, array, `fn`, opaque) as a single pointer to
a flat block of uniform 8-byte slots (`runtime/layout.h`), and both already
compile `&`/`&mut` on one as a no-op passthrough
(`is_heap_pointer_value`). A reference therefore needs no representation of
its own for the aggregate case, which is the case `iter_mut` over a
container actually needs. The "VM pointer representation" decision the risk
section warned about is only required for *scalar* references.

**0a — `&mut` to an aggregate. DONE.** This did not need a new model at
all; it needed one bug fixed. `runtime::layout`'s entry points did not
strip `&`/`&mut` before answering layout questions, and the checker records
the unstripped `&mut cell` on an assignment target's `object` type — so
`struct_field_offset` was asked to find a field on a `ref_kind`, answered
`nullopt`, and both backends reported "field `.value` is only assignable on
struct values yet". Every mutable-reference parameter was uncompilable.
Fixed centrally in `runtime/layout.cpp` (a `strip_refs` applied at all ten
public entry points, so the two backends cannot drift on it) rather than at
the four backend call sites. Covered by
`codegen_stress/041_mutate_through_mut_ref.kira`; full suite green.

**0b — scalar references (`&mut int32`). DONE.** The genuinely new
capability, needed because `iter_mut` over `list[int32]` yields `&mut
int32`. Representation chosen: **a pointer value is the address of an
8-byte slot**, held in `slot_value.u`. This works because slots are
uniformly 8 bytes, and because `frame::registers` is a `std::vector`
whose *buffer* is stable across `frames.push_back` — so a slot address
outlives frame-vector reallocation. Needs three VM opcodes (address-of-
local, load-through-pointer, store-through-pointer) with the width taken
from the statically known referent type, plus the corresponding
`load`/`store`/`getelementptr` in `llvm_codegen`.

Three opcodes were added (`op_addr_local`/`op_addr_slot`/`op_addr_indexed`)
and no load/store opcode was needed: reading through a pointer *is* a
zero-offset `op_load_slot`, which already existed. On the LLVM side the
one substantive change was `storage_llvm_type`, which stripped `&T` to `T`
before mapping — so a `&mut int32` parameter was typed `i32` and silently
became pass-by-value, losing the callee's writes. Every reference is now an
opaque `ptr`, uniformly (an aggregate referent was already one, so nothing
regressed).

Both backends had to be taught that `&mut xs[i]` shares `xs[i]`'s own
address computation rather than recomputing it — computing it separately
addressed `header + i * stride` on a `list`, i.e. bytes inside the
`{len, cap, data}` header. Both are now routed through one
`compile_element_location`/`compile_element_address`, so address-of is
bounds-checked exactly like a read and cannot drift from it.

**Testing gap found and closed.** That `list` bug produced the *same wrong
answer on both backends*, so `codegen_stress_test`'s cross-tier agreement
check passed it silently. Agreement is blind to a bug two tiers share,
which is what they do whenever they share a wrong assumption about
`runtime/layout.h`. Corpus files may now declare `# expect: <n>`, checked
in addition to agreement; opt-in, so existing files are unaffected.
**Any phase below that changes layout or addressing should declare
`# expect:`** — agreement alone will not catch it.

- X1 was scheduled here but landed in Phase 1, where it was first needed.

**Exit:** `m1`/`m2` (`return *x`, `*x = *x + 1`) run correctly on both
backends (**done**); a `codegen_stress` case mutates a caller's local
through a `&mut` parameter (**done** — 041, 042); `--run` and `--compile`
agree (**done**). Full suite green (24/24).

### Phase 1 — G0 and Phase 2 — G1. DONE.

Landed together, because they are one mechanism: an impl on a generic target
has no single compiled form (G0) *because* its signature means something
different per receiver (G1), and both are fixed by instantiating the method
per concrete receiver type. `check_impl_generic_method_call` (`check.cpp`)
rigid-unifies the impl's target pattern against the receiver, then routes
through the existing `find_or_check_generic_instance` — the same engine the
higher-kinded and const-generic paths already use, so instances register as
`const_generic_instance`s and `lower_module` emits them with no lowering
change at all.

Three things this needed that the plan didn't anticipate:

- **The impl's parameters are not the method's.** `T` in `impl[T] get_it[T]
  for holder[T]` is in scope over `def get(self) -> T` while appearing on
  neither `decl.type_params` nor the method's scope. `signature_params`/
  `signature_return_type` now take the enclosing impl (`generic_param_
  bindings`), and the solved bindings are passed as `fixed_type_params` —
  *not* as the solution's `type_slots`, which `push_type_params` only ever
  consults for the declaration's own parameters.
- **Coherence rejected legal programs.** `type_key_of` is declaration-level
  by necessity (`type_has_trait` must find a blanket impl through any
  instantiation), but it was also used as the *conflict* test — so
  `impl get_it for boxed[int32]` and `impl get_it for boxed[int64]` were
  reported as duplicates, which the reference explicitly permits ("no two can
  apply to the same concrete type"). Split into a declaration-level key plus
  `impl_targets_overlap`, which treats two distinct fully-concrete targets as
  disjoint and everything else as overlapping — conservative on purpose, so a
  blanket impl still conflicts with a concrete one.
- **Method lookup ignored which impl matched.** `find_method` keyed on the
  target *declaration* only, so a `boxed[int64]` receiver could resolve to
  the `boxed[int32]` impl's body and be compiled under the wrong types. It
  now takes the receiver's `type_id` and filters by
  `target_pattern_matches`, preferring a concrete impl over a blanket one.
  This was latent, and only reachable once the coherence fix above allowed
  two such impls to coexist.

**Exit:** `s3` and `s4` both return 7 (**done**); `g1_type` reports
`expected str, found int32` (**done**). Covered by
`codegen_stress/043_impl_on_generic_target.kira` (`# expect: 4477`, both
backends, two instantiations each of a generic and a non-generic impl so a
name collision would show as a wrong sum) and three `check_test` cases. Full
suite green (24/24).

Not done, deferred to where it is actually needed: routing
`try_resolve_iterator` through the same helper (Phase 5 exercises it) and
`for x in nums.iter().map(f)` binding a concrete `x` (Phase 5 builds the
pipeline it names).

**X1 landed here rather than in Phase 0**, and immediately paid for itself:
it is what turned `s3`'s "no compiled module defines `main`" into a labelled
error naming the expression. A lowering failure is now a real diagnostic
whenever code was requested (`--run`/`--compile`) — see
`driver/lowering_stage.cpp` for why it stays quiet otherwise. Turning it on
surfaced roughly a dozen pre-existing silent lowering gaps in the semantic
corpora (string interpolation without `std.fmt`, default parameter values,
`filter` on a list, ...); all were already known limitations, none are new.

### Phase 3 — R3a: tuple patterns in `for`. DONE.

Planned as a narrow pattern-lowering gap. It was two gaps, in two phases,
and the second was the more serious one.

**Lowering** (as planned). Every loop lowerer binds exactly one name per
iteration, so `lower_for_stmt` now reduces a pattern head to that shape: the
element binds to one synthetic local, and `destructure_loop_element` emits
the pattern's bindings at the top of the body via the same `lower_pattern`
machinery a destructuring `let` already used. The five loop lowerers were
not touched. Bindings are emitted *ahead* of the guard-wrapped body, so
`for (k, v) in xs if k > 0` can name them. The synthetic local's name
carries `next_symbol_` because the backends' `locals_` maps keep the first
registration for a name — nested destructuring loops would otherwise have
the inner loop read the outer element.

**Checking** (not planned). `for k, v in pairs` — the parenthesis-free
spelling, which the grammar allows — bound every name to `k_unknown_type`.
That unifies with everything, so `let s: str = v` inside the body was
accepted *silently*; only lowering objected, and only because it refused
the head outright. This is the "compiles cleanly is not behaves correctly"
failure mode from CLAUDE.md, and it was live in both `for` statements and
comprehension clauses — where a comment already claimed positional
destructuring the code did not do. Both now route through one
`check_loop_head_patterns`, which binds each name to its tuple component
and reports a wrong count or a non-tuple element, pointing at the loop
variables rather than the statement span.

Comprehension clauses (`for (k, v) in pairs => k * v`) were carried along
in both spellings; they reach the same loop lowerers.

**Exit met:** `044_for_destructuring.kira` (`# expect: 4955`) covers both
spellings, a guard over destructured names, a nested pattern, nested
destructuring loops, and both comprehension forms — passing on both
backends. Split across several `def`s because the bytecode compiler does
not reuse registers and one body exceeded the 256 its u8 operands address.
Three `check_test` cases cover the silent-typing bug and the two new
diagnostics; each was confirmed to fail with its mechanism reverted.

### Phase 4 — UFCS

The fourth, strictly-last arm in `infer_method_call`, reusing
`receiver_fills_first_param`/`check_receiver_call`. Receiver adaptation per
design §3.2, with `&mut T` against a non-`mut` binding a hard error.
Eligibility per §3.3, no opt-in marker. All three diagnostics of §3.4.

**Exit:** a free `pub def` is callable as a method; a method always wins
over a UFCS candidate; ambiguity is a hard error naming every candidate;
the full existing test suite is green (strict-last means it must be).

### Phase 5 — `std.iter` protocol and `std.algo` catalog

Over the **existing builtin `list`** — no pointers needed beyond Phase 0,
no denaturalization yet. Traits per §5.1 including `from_iter`/`collect`.
All three iteration families including `iter_mut` (unblocked by Phase 0).
Adapters per §5.3, catalog per §6.1. X2 lands before `collect`.

Also here: the R5 checks — instance-cache key includes nested type
arguments, symbol-name hashing past a length threshold, `generic_depth_`
≥ 16 with a teaching diagnostic, and an eight-stage `codegen_stress` case
with **measured** compile time.

**Exit:** `scores.values().filter(x => x > 10).map(x => x * 2).sum()` runs
on both backends; `let names: list[str] = it.collect()` infers; `some
iterator[T]` names a pipeline in return position (R4).

### Phase 6 — The `machine` substrate

`in_machine_fn_` gating per §4.1, the fourteen intrinsics per §4.2, the two
safe slice bridges, `uninit[T, N]` as an opaque builtin with its five
accessors written as ordinary `machine def`s.

**Exit:** a `list`-shaped struct over `*mut T` allocates, grows, reads,
writes, and drops correctly on both backends; dereferencing outside a
`machine def` produces the §4.1 diagnostic.

### Phase 7 — Containers

In order: `deque`, `bitset` (plus the bit-operator traits in `std.traits`),
`ordered_map`/`ordered_set` (sorted-vector first), `unordered_map`/
`unordered_set`. Then `list` denaturalization last, per §6.2's three steps —
additive, close-the-silent-hole, denaturalize — never collapsed into one.
Denaturalization additionally needs array-literal lowering, `for` over
`list` routed through `into_iterator`, and an `index` trait with an operator
hook.

### Phase 8 — `str` and retroactive intrinsic migration

`str_builder`, `chars`/`bytes`/`char_indices`, lazy `split`/`lines`. Then
§7: migrate the six failing-(a) `rt_str_*` intrinsics to Kira, **benchmarked
against the intrinsics before removal**.

## 3. Risk posture

R2 is retired by Phase 0 and is where schedule risk concentrates — treat any
estimate for it as soft until the VM pointer representation is chosen. R1 is
mitigated, not retired, by X2; real call-site bound checking stays out of
scope. R5 is checked inside Phase 5 rather than discovered in Phase 7. R6 is
accepted, with `#[no_ufcs]` held in reserve.
