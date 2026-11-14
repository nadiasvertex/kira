# Ergonomics comparison: ownership designs A, B, and C

This document compares three ownership designs for Cinder on representative library code
and application workflows. A is the [affine capability core](../affine-capability-core.md).
B is scoped value semantics. C adds inferred access requirements and effects, abstract
storage groups, per-outcome summaries, checked authority-backed tokens, and an explicit
managed mutable path. The recommendation is to prototype the strengthened
[C plan](c-plan.md), not to claim equivalence to A or an established Pareto optimum.
More effort should buy lower runtime cost and stronger guarantees while memory safety
remains required throughout. See the [complete A/B/C applications](applications/README.md).

The original 17-signature exercise counted about 8 clauses for A pinned, 5 for A inferred,
3 for B, and 4 for the earlier C. Those counts do not cover the strengthened C contracts
or application restructuring. About thirty probes against the compiler at the time of the
original exercise found five compiler bugs, including three soundness holes, recorded as
items 30–34 in [`spec/todo.md`](../todo.md). These are historical observations, not a fresh
compiler run or evidence that the proposed designs have been implemented.

The library ports and annotation counts are done on paper, and the counts are by hand. The probes are real programs and their results are reproducible. They do not replace P01/P02.

## Design B summary

The full rules, retained safety obligations, implementation order, and validation gates
are extracted into the [standalone B plan](b-plan.md). Its [rules 1–10](b-plan.md#rules)
are the baseline extended by C below; they no longer live as a second copy here.

B uses ordinary owned values and scoped `&T`/`&mut T` borrows, field and partition places,
loan-carrying values, source-inferred result/store-flow summaries, and conditional cleanup.
Paths still require alias resolution, backing identity, and reborrow dependencies.
The store check rejects local escape and caller-side store propagation keeps stored loans
live. These are required design rules, not claims that today's compiler implements B.

B deliberately keeps whole-authority effects and whole-receiver borrowing, unions stored
loans across outcomes, and uses private constructors for ordinary affine typestate.
It has no groups, inferred partial receiver access, checked sealed-token creation, or
managed mutable sharing. Its application workarounds are field helpers, explicit state
threading, and prepare/commit APIs. A token authorizing primitive memory-sensitive access
requires an explicitly trusted implementation in B; private construction alone is not proof.

See the B plan's [A/B machinery mapping](b-plan.md#what-b-removes-and-what-it-retains) and
[precision/trust limits](b-plan.md#deliberate-precision-and-trust-limits) for the complete
extraction of this comparison's original B section.

## Design C rules

C builds on B, but needs more than narrow write sets to accept methods on partly borrowed
objects. The detailed semantics, managed path, and dependency-ordered tasks are in the
[C plan](c-plan.md). These remain proposals; scripts benefit from inference but can still
encounter ownership errors or choose explicit managed APIs.

11. **Access and effects.** Infer required shared and exclusive access separately from writes and ownership transitions, using a bounded fixpoint with conservative widening over cycles. A call acquires exactly its checked access requirements before invocation; the body is checked under those requirements. Narrow writes alone cannot justify borrowing less than a whole `&mut self`. Unknown contracts conservatively require the authority admitted by the signature. Include transitive calls, captures, stored loans, callbacks, and cleanup.
12. **Groups.** A type may declare named groups abstracting its fields and owned storage. Exported access/effect summaries use groups rather than private fields and round up conservatively. A type with no group declaration is one group. Groups retain structural dependencies: replacing a backing pointer can invalidate element storage in a different group. Nested/generic groups require checked substitution. Unsafe group coverage is part of an explicit trusted contract.
13. **Fact invalidation.** A fact depends on the actual identity and state observed to establish it. Infer observer read sets where possible and invalidate on overlapping writes or relevant relocation, destruction, replacement, and cleanup. Epochs or equivalent internal state may be necessary. A positional bounds witness and a claim about the value at that position have different dependencies.
14. **Per-outcome summaries.** Keep origins, store flows, initialization, and authority states per variant where useful. Before discrimination protect all possibly retained loans. Intervening effects update every pending alternative; a later `match` cannot restore stale authority or facts. Discard/join conservatively. Error results do not imply rollback. Stronger semantic postconditions require proof or checked primitive contracts.
15. **Checked authority.** Private constructors suffice for ordinary typestate. A token authorizing memory-sensitive operations must contain or derive from checked authority; safe declaring-module code cannot forge it either. Native minting is explicitly trusted.
16. **Managed sharing.** Offer explicit `managed[T]` handles with retention and runtime-checked scoped access. The initial candidate uses single-threaded reference counting and weak handles; strong cycles retain storage. Access conflicts return an error, and no access-dependent loan can escape. Retention alone does not permit element views across relocation. No implicit managed fallback, freeze, or thread transfer is allowed.
17. **Interfaces and I/O.** Infer complete public summaries by default and permit pinned constraints. Check compatibility against a recorded released interface across all summary components. Restricted I/O is enforced transitively through imports, callbacks, globals, native boundaries, and cleanup; scripts receive broad entry authority explicitly.

Tokens are added as described under [Capability tokens](#capability-tokens).

```cinder
pub type list[T] = { len: usize, cap: usize, data: ptr[T] }
    group shape    = { len, cap, data }
    group elements = data[..]
```

The `group` spelling is a placeholder. `elements` covers the storage that `data` points to, so writes through the raw pointer inside `list`'s methods are assigned to it. With this declaration, `sort` and `set` export "writes `elements`", while `push` and `pop` export "writes `shape` and `elements`".

```cinder
for x in &self.items:
    self.bump(*x)        # C requires inferred access confined to self.total (p22)

let i = xs.argmax()      # i: index[xs], a brand that reads xs.shape
xs.sort()                # writes xs.elements; i survives
total += xs[i]           # no bounds check
xs.push(0)               # writes xs.shape; i is invalidated
```

`xs.push(0)` invalidates `i` even though the index stays valid. Keeping it would need a monotonic fact such as "length only grows", stated as an explicit postcondition. Those should be rare.

Here `index[xs]` witnesses only positional bounds and collection identity. The stronger
claim that `i` still identifies a maximum depends on elements and is invalidated by sort.
The group's pointer/backing relation must also participate in loan invalidation.

### C compared with A

C targets comparable precision with a simpler surface; equivalence and soundness have not
been established. In particular, the current A core says a whole-receiver requirement
still borrows the whole receiver. A write clause alone does not grant partial authority.

| A | C |
|---|---|
| Regions and the ownership forest | Resolved places plus internal backing identity and lifetime/transition checks |
| Declared `writes` footprints | Inferred access requirements, write sets, and transitions (rule 11) |
| Footprints over abstract regions | Groups (rule 12) |
| Per-region storage versions | Dependency-aware fact invalidation, with epochs or equivalent internal state as needed |
| Per-outcome output states | Per-variant summaries (rule 14) |
| Sealed capabilities | Checked authority-backed tokens; private constructors for ordinary typestate |

C does not require exposed region or owner-handle syntax. That does not establish that
its verifier is smaller: access inference, managed sharing, and provenance need evaluation.

### C's risks

- **Inferred public contracts can change.** Store complete summaries and compare against a recorded released interface, including access requirements, origins, store flows, outcomes, transitions, call modes, groups, and guarantees. A previous local build alone is not a compatibility baseline. Pinning is optional; incompatible changes need an explicit version/interface change.
- **Diagnostics for deep writes.** A rejection can come from a write several calls down (p25). The diagnostic must show the call chain that produced the write. Today's diagnostics name only the conflicting variables.
- **Inference cost.** Summaries are computed per package with a fixpoint over cycles. Calls through function values use the declared `fn`-type summary.
- **Coarser exported summaries.** Outside the declaring module, write sets are rounded up to groups. A library author who wants finer precision for callers declares finer groups.
- **Group declarations on unsafe types.** A type whose methods write through raw pointers must declare groups that cover the pointed-to storage. If the declaration is wrong, inference cannot catch it, so it belongs in the trusted code that the type's author already maintains.
- **Access and lifetime soundness.** Write disjointness does not establish read disjointness, authority acquisition, or safety under relocation and destruction. C01–C05 must define these rules before claiming equivalence.
- **Managed runtime and cycles.** The convenient path adds access checks, retention, and cleanup obligations. Strong reference cycles retain storage under the initial candidate; concurrency and suspension require separately validated extensions.

## Method

The library ports and hard-case ports use each design's accepted or proposed spellings. The probes live in [`probes/`](probes/). Run one with `./bazel-bin/src/cinder spec/affine-capability/probes/<file>.cn`. The exit code is `main`'s result. Today's checker roughly approximates B's rejections: it has loans, carried origins, and liveness, but no store check, field paths, or summaries. The probes therefore show B's behavior only where those pieces don't matter, and they show compiler bugs where today's behavior violates the specification. Probes that today's compiler rejects for borrow reasons have a control that removes the conflict and returns the expected value, which shows the rejection comes from the borrow rule.

## Library ports

The original library exercise uses B's spellings plus C's `list` groups. The strengthened
C adds semantic obligations and APIs outside that exercise; its full application cost is
not represented by the signature table.

### list

```cinder
# A
extend[T] list[T]:
    pub def push(mut self, value: T)
    pub def pop(mut self) -> option[T]:
        return remove_last(self)
    pub def get(self, i: usize) -> option[&T]
    pub def slot(mut self, i: usize) -> option[edit[own[T]]]

def replace_at[T](xs: &mut list[T], i: usize, v: T) -> option[T]:
    match xs.slot(i):
        @some(s) => return @some(exchange(*s, v))
        @none    => return @none

# B and C
def replace_at[T](xs: &mut list[T], i: usize, v: T) -> option[T]:
    if i >= xs.len():
        return @none
    return @some(replace(&mut xs[i], v))
```

`get` drops today's `where T: copy` bound in all designs (probe p09 shows today's rejection). A needs the owner-slot type for generic replacement. B and C need `&mut xs[i]` to have type `&mut T`; today it is `cell_mut[T]`, which breaks `replace` (todo 33, probe p10). In C, `replace_at` is inferred to write `xs.elements`, so a caller's facts about `xs.len()` survive it.

### option

```cinder
# A
def map[U](self: own[Self], f: fn(self: own[Self], T) -> U) -> option[U]
# B and C
def map[U](self: Self, f: fn(T) -> U) -> option[U]
```

A's callable type must state the consuming entry so that it accepts any closure. In B and C, a by-value `fn` parameter is invoked at most once by default.

### iter

```cinder
pub trait iterator[T]:
    def next(mut self) -> option[T] from carried(self)      # A
    def next(mut self) -> option[T] carried                  # B and C

pub type map_iter[I, T, U] = { inner: I, f: fn(mut self, T) -> U }   # A
pub type map_iter[I, T, U] = { inner: I, f: fn mut(T) -> U }         # B and C
```

`carried` is a placeholder spelling. Without it, the result of `next` borrows the iterator itself, so two live results are rejected (probe p03). An adapter that calls a stored closure repeatedly needs a reusable call mode in every design. The trait method's write set stays at the default, "writes `self`", which is correct for iterators.

### string

```cinder
pub def split(self, sep: str) -> list[str] from self        # A pinned; inferred elsewhere
pub def replace(self, from: str, to: str) -> shared_str     # all designs
```

The default origin for `split` would keep `sep` borrowed as well. Today's compiler does this (probe p07 shows the same over-approximation with slices). Text producers must return `shared_str` in all designs. `from` is also a parameter name in `std.string`, which collides with A's clause keyword.

## Annotation counts

These are historical hand counts for the earlier designs, retained as a library baseline.
They have not been recomputed for strengthened C and are not application ergonomics results.
A clause is counted only when it differs from the default derived from the signature. The defaults are:

- **Writes:** a signature that receives no exclusive authority writes nothing, and an exclusive parameter may write its target.
- **Origins:** the result may hold every borrowed input.

A group declaration counts as one clause. "C pinned" counts what an author would write to fix every exported contract that differs from the default. These historical C columns predate access contracts and managed sharing; the recommended policy now uses complete released-interface checks in the C plan.

| # | Signature | A pinned | A inferred | B | C inferred | C pinned | Clause |
|---|---|---|---|---|---|---|---|
| 1–5 | list.push, pop, get, set, as_slice | 0 | 0 | 0 | 0 | 1 | C pinned: `set` writes `elements` |
| 6 | list.replace_at, generic | 1 | 1 | 0 | 0 | 1 | A: `edit[own[T]]` slot; C pinned: writes `xs.elements` |
| 7–8 | option.as_ref, is_some | 0 | 0 | 0 | 0 | 0 | |
| 9 | option.map | 2 | 2 | 1 | 1 | 1 | A: `own[Self]` and consuming entry; B and C: by-value `self` |
| 10 | list_iter.next, impl | 1 | 0 | 0 | 0 | 1 | `carried` (inferable from the body) |
| 11 | iterator.next, trait | 1 | 1 | 1 | 1 | 1 | `carried` |
| 12 | list_iter_mut.next, impl over `split_at` | 1 | 0 | 0 | 0 | 1 | `carried` |
| 13–15 | str.trim, split, replace | 1 | 0 | 0 | 0 | 1 | `split`: `from self` |
| 16 | native string intrinsic | 0 | 0 | 0 | 0 | 0 | default suffices |
| 17 | map_iter stored closure | 1 | 1 | 1 | 1 | 1 | reusable call mode |
| | `list` group declaration | — | — | — | 1 | 1 | `shape` and `elements` |
| | **Total** | **8** | **5** | **3** | **4** | **9** | |

A's write footprints for `set` and `replace_at` fall under its default in this count because A's region model already separates the slot from the list's shape.

## Hard cases

C additionally provides per-outcome states and checked memory-sensitive authority. The
rows describe intended behavior, subject to the required access and provenance rules.

| Case | A | B | C | Probe evidence |
|---|---|---|---|---|
| Callback whose result must come from its input | `fn(part: slice[int32]) -> slice[int32] from part` | Same clause | Same clause | p13: today rejects conservatively; the diagnostic names the wrong binding (todo 34) |
| Callback that edits a captured counter while an earlier result is live | `&mut fn(mut self, part) -> slice from part writes {self}` | `&mut fn(part) -> slice from part`; the edit follows from the `&mut` passing mode | Same as B | none |
| Closure returning a duplicate of a captured view | Precise with `from carried(self)` | Same clause | Same clause | none |
| Escaping closure capturing a local borrow | Rejected | Rejected | Rejected | p14: today rejects |
| Generator borrowing its argument | Undefined until GX | Generator is a scoped value carrying its arguments' loans | Same as B | p15: today's checker accepts `xs.push` while the generator is live (todo 31); p15b: the hand-written iterator is rejected |
| Typestate token | `capability connection(r, state)`; authority-backed | `type conn[open: bool]`, affine and consumed and returned; needs private constructors | Private typestate; memory-sensitive tokens additionally require checked backing authority | p16 accepts; p18 rejects reuse after a move; p17: a `bool` index mismatch is accepted (todo 32), while p17b's `usize` index mismatch is rejected |
| Borrow stored through a `&mut` parameter | Rejected by lifetime dependencies | Rejected by the store check (rule 8) | Same as B | p06 and p06b: today accepts and reads a dangling reference (todo 30) |
| By-value argument carrying `&mut` | Effect covered by the reachable-authority bound | Effect covered by rule 7 | Same as B | p04 rejects a read during the loan; p05 writes `n` and returns 2 |
| Method writing one field while another field is borrowed | Whole-receiver loan conflicts; use an explicitly narrower authority contract or field helper | Rejected; pass `&mut self.total` to a free function instead | Intended acceptance through inferred narrow access, not writes alone | p22, p24 rejected in the historical run; p24b control returns 11 |
| Index or length fact surviving an in-place edit | Storage versions | Lost at every `&mut` call | Kept unless the call writes `shape` | Not expressible today: no brand types |
| Write reached through a helper call | Rejected | Rejected | Rejected; the diagnostic shows the call chain | p25 rejected today, without the chain |

## Probe results

| Probe | Historical compiler result | A target | B target | C target |
|---|---|---|---|---|
| p01 push while `&xs[0]` is live | Rejected | Rejected | Rejected | Rejected |
| p02 loop over `self.items`, write `self.total` | Rejected (whole-variable limit) | Accepted | Accepted (needs field paths) | Accepted |
| p03 two live `next` results | Rejected | Rejected without `carried` | Same | Same |
| p04 read `n` while a by-value struct holds `&mut n` | Rejected | Rejected | Rejected | Rejected |
| p05 by-value struct writes `n` | Accepted, 2 | Accepted | Accepted | Accepted |
| p06, p06b borrow of a local stored into an outer list | Accepted, dangling (bug) | Rejected | Rejected | Rejected |
| p07 result borrows one of two arguments | Rejected (over-approximation) | Accepted (inferred) | Accepted (inferred) | Accepted (inferred) |
| p08 view of a temporary | Rejected | Rejected | Rejected | Rejected |
| p09 `get` on a list of lists | Rejected (`copy` bound) | Accepted as `option[&T]` | Same | Same |
| p10 `replace(&mut xs[i], v)` | Type error (bug) | Use the slot form | Accepted once `&mut xs[i]` is `&mut T` | Same as B |
| p11 two live `iter_mut` results | Rejected | Rejected; needs `split_at` | Same | Same |
| p12 move on one branch only | Accepted, 8 | Explicit drop required by the plan revision | Accepted | Accepted |
| p13 callback returns captured storage | Rejected; diagnostic names the wrong binding | Rejected | Rejected | Rejected |
| p14 escaping closure | Rejected | Rejected | Rejected | Rejected |
| p15 push while a generator borrows | Passes the checker (bug) | Undefined until GX | Rejected | Rejected |
| p16–p18 typestate | p16 accepted; p17 accepted (bug); p18 rejected | Sealed capability | Affine value-indexed type | Same as B |
| p19 get-or-push, borrow ends in the `@none` arm | Accepted, 0 | Accepted | Accepted | Accepted |
| p20, p20b callee stores a view into a `&mut` argument, caller then mutates the source | Accepted (bug) | Rejected | Rejected | Rejected |
| p21 view stored only on `@ok`, source mutated on `@err` | Accepted, but only because of the p20 bug | Accepted (per-outcome states) | Rejected (union summary) | Accepted (rule 14) |
| p22 loop over `&self.items` calls `self.bump`, which writes `self.total` | Rejected | Rejected with whole-receiver authority; needs narrower contract/helper | Rejected | Intended acceptance with narrow access |
| p23 loop over `&a.items` passes `&mut a.total` to a free function | Rejected (whole-variable limit); p23b control returns 6 | Accepted | Accepted (field paths) | Accepted |
| p24 view of `a.items` live across `a.bump(10)` | Rejected; p24b control returns 11 | Rejected with whole-receiver authority; needs narrower contract/helper | Rejected | Intended acceptance with narrow access |
| p25 as p24, but `bump` pushes to `items` through a helper | Rejected; diagnostic shows no call chain | Rejected | Rejected | Rejected; diagnostic shows the chain |

The p22/p24 A entries correct the original write-bound-only acceptance claim. C's
corresponding entries are design targets for rule 11, not executed results.
The p16–p18 C entry additionally requires rule 15 when
the token authorizes memory-sensitive operations; plain typestate alone is insufficient.

p19 is the get-or-push case: the historical Cinder probe's per-path liveness handles it,
so it needs no per-outcome feature. p21 cannot show per-outcome precision until todo 30's
store tracking exists. p23 is the workaround B offers for p22; the historical compiler
run rejected it for lack of field paths. This table makes no claim about other compilers'
current acceptance behavior.

## Opt-in features for stronger checking

Per-outcome loan states and capability tokens both have concrete uses in systems code. In C, per-outcome states are part of the inferred summaries. Tokens are an opt-in layer. The intended split is:

- **Scripts:** inferred summaries, drop flags, broad entry-point I/O authority, and explicit managed sharing where ownership would require restructuring. Runtime access/bounds checks and retention have visible costs.
- **Systems code:** owned storage, scoped views, finer groups/access contracts, checked facts and tokens, and restricted I/O. Programmer work can remove checks and retention while preserving safety.

### Per-outcome loan states

Per-outcome states are needed when a callee changes a place the caller keeps, and the change differs by outcome. They are not needed when the state travels in the result value, as with `try_send` returning the message in its error or `try_lock` returning a guard. Programs that need them:

- **Registration and interning that borrow input.** A registry, interner, or arena stores a view only on success (p20, p21). With a union summary, the caller cannot mutate the source after a failed registration.
- **Out-parameter initialization.** A read into an uninitialized buffer initializes the prefix only on success. This supports zero-copy I/O and in-place construction.
- **Facts that hold only on success.** `try_reserve` can guarantee spare capacity, and `try_parse` can give a refined type, only on `@ok`.

### Capability tokens

A capability token is an affine value whose existence proves something: a lock is held, an index is in bounds, interrupts are off, or the holder may do I/O. Programs that need them:

- **Lock guards.** A forged guard is a data race.
- **Branded indices.** An arena's `alloc` returns an index branded with that arena, so indexing needs no bounds check. This fits graph and entity-system code.
- **Critical-section and peripheral tokens.** Only code holding the token issued by a critical section may touch the state it guards. Peripheral singletons follow the same pattern.
- **Capability-restricted I/O.** A library can do file or network I/O only through a token passed to it. Scripts keep unrestricted I/O by default.
- **Protocol typestate.** A connection or file handle changes type as it moves through its protocol (p16–p18).

Tokens need four pieces:

- private constructors for ordinary typestate, plus checked backing authority for memory-sensitive tokens;
- affine value-indexed types, which exist today apart from todo 32;
- root tokens issued by the runtime or passed to `main`;
- brand types that name a specific value, such as an index branded with one arena. These build on Cinder's dependent types. In C, a brand reads its owner's `shape` group, so rule 13 decides when it survives a call.

Strengthened C retains the authority obligation inside safe declaring-module code.
Private construction alone is not proof of lock ownership, index validity, or singleton
uniqueness. Unchecked native authority creation belongs in an explicit trusted boundary.

## Findings

1. **Library annotation counts are only a baseline.** The original hand counts are close, but do not measure application rewrites, copies, managed checks, or strengthened C contracts. Evaluate the complete applications before drawing an ergonomics conclusion.
2. **C is a frontier candidate.** It targets a concise surface and strong checking; equivalence to A and a smaller sound core remain unproved. Internal backing identities, granting dependencies, and state versions may still be necessary.
3. **Partial receiver access matters.** A narrow write bound does not resolve whole-receiver borrowing. C must infer and acquire narrow access; A needs an appropriate authority contract or field helper. Both must reject hidden reads conflicting with exclusive loans.
4. **Contract stability is one cost among several.** Compare complete released interfaces, allow pinning, and measure managed retention/checks, inference complexity, group abstraction, and programmer restructuring.
5. **The historical probe run found three soundness holes:**
   - A borrow of a callee's local can escape through a `&mut` parameter (todo 30, p06, p06b).
   - A callee can store an argument's view into a `&mut` argument without the caller's variable carrying that loan, so the source can be mutated while the view is live (todo 30, p20, p20b).
   - A generator's result does not carry its arguments' loans (todo 31).

   All designs require the store check and store-flow summaries that would close the first two.
6. **All designs need the same library changes:**
   - `option[&T]` from `get`
   - `split_at`-based `iter_mut`
   - a drain primitive or trusted body for front-consuming `into_iter`
   - `shared_str` from text producers
   - a `carried` spelling
   - a resolution of the `from` keyword collision
7. **Typestate and primitive authority differ.** Private affine state-indexed values can express protocols; memory-sensitive tokens also need checked authority. Branded indices require identity plus a current validity witness.
8. **C needs staged validation.** The [C01–C10 plan](c-plan.md#dependency-ordered-implementation-work) orders access/provenance rules, store flows, inference, groups/facts, outcomes, checked tokens, interfaces/I/O, managed sharing, application measurements, and proof/integration gates.

## Not covered

- Async functions and cancellation.
- `crew` and task borrows (today's fixtures cover these).
- Trait objects.
- Proof-kernel facts across abstract types.
- A formal statement of B and C. Rules 6–8 and 11–14 are where the soundness argument would concentrate.
- Surface syntax for groups, brands, and pinned summaries, and how a module opts in to restricted I/O.
- How groups interact with generic type parameters, for example a `list[list[T]]` whose element type has its own groups.

These remain implementation/proof gaps. The C plan adds requirements for them; the
application listings are complete proposed workflows, not evidence that the gaps are closed.
