# Affine capability core

Status: design proposal. This document defines a candidate core, not implemented Cinder syntax
or a replacement for the current normative specification. Safety claims below are obligations
for a future formalization, not established metatheorems.

## Intent and choices

Function parameters use copy semantics by default. `Copy` means semantic independence, not necessarily duplication of every byte. Primitive values are copied directly; copying mutable composite data must produce independent mutable storage. Immutable strings and frozen subobjects may share backing storage when lifetime management is preserved. Composite values require a checked copy operation that satisfies these conditions. A function can instead request a capability such as `own[T]`, `shared_read[T]`, `inspect[T]`, or `edit[T]`.

Capability names are verbs describing what the holder may do. Proposed conversion annotations are `&x` for a loan, `&>x` for ownership transfer, and `&!x` for freezing. Unadorned `x` uses the parameter capability and conversion rules below.

Local inspection is different: a scoped `inspect` loan permits reading while temporarily
suspending mutation. It does not produce a persistent `read` value. This distinction is
deliberate: interpreting every local read as permanent freezing would prevent ordinary local
construction and update. All notation here is schematic.

Shareability means permission to duplicate a handle, not permission to outlive its storage.
Persistent shared handles retain their storage; scoped references retain a lifetime obligation.
Thread transfer additionally requires that the representation and external resources support it.

## Core objects

Use abstract region identities `r`, loan identities `l`, types `T`, and immutable value
identities `v`. Initially each region is a whole allocation; field-disjoint borrowing is an
extension. Regions owned exclusively form an ownership forest. Cyclic mutable ownership,
interior mutation, raw pointers, and concurrent mutation are outside this core.

```text
value types A ::= scalar | product(A...) | sum(A...) | shared_read(r,T)
                | own(r,T) | inspect(l,r,T) | edit(l,r,T)
                | closure(environment, signature) | proof(P)

region state S ::= unique | frozen | dead

operations ::= new | load | store | move | freeze | clone
             | begin_inspect | begin_edit | end_loan | call | drop
```

An `own(r,T)` carries exclusive authority to read, write, transfer, freeze, or destroy the region. It is affine. `shared_read(r,T)` denotes permanently immutable storage and is duplicable.
`inspect` is a duplicable scoped observation loan; `edit` is an affine exclusive loan.
Neither kind of loan owns storage. Loads of nonduplicable contents produce observations or
require an explicit move; reading must not silently duplicate ownership.

Parameters default to copy semantics. Literal immutable values need no mutable allocation first. Explicit local builders may start with `own` authority.

## Checking judgment

```text
Sigma ; Gamma ; Delta ; Loans ; Facts |- e : A
    => Sigma' ; Delta' ; Loans' ; Facts'
```

`Sigma` records region states and representation types. `Gamma` contains duplicable values
and immutable identities. `Delta` contains affine handles and abstract capability tokens.
`Loans` records active loans, their origins, and their carriers. `Facts` contains propositions
about immutable identities or particular versions of mutable storage.

Sequential checking threads the output state into the next expression. Contexts are not
silently copied when checking subexpressions. Moving an affine handle removes its old binding
and transfers its authority to the result. Dropping it invokes its defined cleanup rather than
merely forgetting a live resource.

## Primitive rules

| Operation | Preconditions | Result |
|---|---|---|
| `new(T)` | Valid initializer | Fresh unique region and `own(r,T)` |
| `load(h)` | Live read authority; no conflicting edit loan | Scalar copy or scoped observation |
| `store(h,x)` | `own` with no loans, or active `edit` authority | Updated storage version |
| `move(h)` | Affine handle available; owner has no active loans | Transfer handle; source unavailable |
| `begin_inspect(h,l)` | Read authority; no exclusive loan | Scoped observation; suspend conflicting owner actions |
| `begin_edit(h,l)` | Exclusive authority; no competing loans | Scoped edit; suspend originating authority |
| `end_loan(l)` | No live carrier of `l` | Restore suspended authority |
| `freeze(h)` | Unique owner; no loans; recursively freezable contents | Consume owner, mark region frozen, return `shared_read` |
| `clone(h)` | A checked cloning operation for `T` | Fresh storage, distinct identity |
| `drop(h)` | Owner has no loans, or shared handle is being released | Cleanup or release; final storage reclamation when safe |

An edit loan may create nested observation or edit loans only by suspending its own conflicting
authority until those loans end. This is reborrowing, using the same rules as an owner.

There is no `shared_read -> own` conversion. Even discovering that a frozen handle has one remaining
reference does not thaw it. An independent mutable copy requires `clone` and a fresh region.
Rebinding a variable to fresh storage does not mutate the old frozen value.

Freezing is deep: recursively consume and freeze exclusively owned reachable regions; already
frozen children are allowed. Borrowed mutable children, outstanding mutable aliases, and
opaque resources without a checked freezing contract prevent freezing. A read-only outer
record must not conceal a mutable cell or an operation that mutates its reachable storage.
Reading an external resource does not imply the external world is immutable: such resources
need explicit operation capabilities and cannot automatically become persistent `read` data.

## Capability requests and conversions

A prototype declares each requested parameter capability; otherwise it requests a copy. Callers may write `&`, `&>`, or `&!` to request a loan, transfer, or freeze. An incompatible explicit annotation is an error.

| Requested capability | Available value | Conversion |
|---|---|---|
| `copy[T]` (default) | Copyable value | Copy with semantic independence; mutable composite storage is independent, while immutable backing storage may be shared when lifetime-safe |
| `own[T]` | Copyable value | Semantically independent copy into fresh owned storage; immutable backing storage may be shared when lifetime-safe |
| `own[T]` | `own(r,T)` with `&>` | Transfer; consume source |
| `shared_read[T]` | `own(r,T)` | Copy, then freeze the copy; source remains owned |
| `shared_read[T]` | `own(r,T)` with `&!` | Freeze original; consume source |
| `shared_read[T]` | Existing `shared_read(r,T)` | Duplicate shared handle |
| `inspect[T]` | Owner or shared handle | Begin scoped observation loan |
| `edit[T]` | Owner | Begin exclusive edit loan |

All conversions require their relevant conditions: copying must be supported, freezing must be valid, and loans must not conflict. `&x` uses the loan mode requested by the prototype; `&>x` transfers only; `&!x` freezes owned storage. Thus `publish(x)` may copy and freeze while preserving `x`, while `publish(&!x)` consumes and freezes the original. A composite containing an uncopyable resource cannot use the default copy route; its caller must transfer ownership with `&>` or lend it to a suitable `inspect` or `edit` parameter.

## Calls, lifetimes, and control flow

A function signature specifies argument modes, result capabilities, borrowed result origins,
and abstract token transitions. Its body is checked against that contract once. Call checking
uses the contract without inspecting the implementation.

Parameter conversions follow the capability conversion table. The default copy route never consumes or freezes the caller value.
An explicit scoped observation parameter accepts `inspect` without freezing. Any returned
loan names its originating parameter region in the core signature; the surface language may
infer this information. A loan of a function-local allocation cannot escape.

Loan carriers include aggregates, closures, returned values, and nested loans. Local
last-use analysis ends a loan only after every carrier is dead. When escape or alias analysis
cannot establish this, checking conservatively retains the loan or rejects the operation.
Lifetime inference is not a promise to accept every semantically safe program.

At a branch join, an affine authority is available only if it survives in every reachable
branch. A region is writable afterward only if every branch preserves unique write authority.
If one branch freezes and the other retains an owner, explicitly freeze that other owner at
the join when valid, or reject a requested common representation. Never restore lost authority.
Loops require a stable capability invariant on every back edge; freezing an owner in one
iteration cannot be followed by writing it in the next. Early returns perform the cleanup
required by their exit states. Exceptions and cancellation need equivalent explicit exits
before they are admitted to this core.

An aggregate derives duplication from all its fields. A closure is an aggregate environment
plus checked code: a consuming call takes its environment by ownership, a mutating reusable
call takes an edit loan, and a read-only reusable call takes observation authority. Capturing
a loan preserves its lifetime obligation; capturing ownership preserves affine usage.

## User-defined capabilities

Users may define sealed abstract capabilities with finite operation signatures. A capability
is evidence of authority, not a freely constructible label attached to a value.

```text
capability append(r)                 // affine unless explicitly justified otherwise
append_item : append(r) * item -> append(r)

capability connection(r, state)
send  : connection(r, open) * message -> connection(r, open)
close : connection(r, open) -> connection(r, closed)
```

The declaring module defines private constructors and checked operation bodies. Creating an
`append(r)` token must consume appropriate exclusive authority; the token privately owns
that authority. Each operation preserves or consumes it according to its signature. A module
cannot mint a second token by naming the same region. An append token coexists with frozen
data only when it controls separate storage, not the frozen region itself.

Users can restrict and package existing authority. They cannot introduce unchecked primitive
permissions, declare a token duplicable while it contains exclusive authority, or prove their
way into write access to frozen storage. Duplicable capabilities must have duplicable private
representations. Native operations require an explicitly trusted boundary.

Protocol tokens are affine by default: abandoning one follows its declared cleanup behavior.
A guarantee that a user explicitly performs `close` would need an additional must-consume
rule; affinity alone cannot establish it.

## A terminating proof kernel

The guarantee is that every submitted proof has a terminating verification procedure and
every accepted proof follows the kernel's rules. It is not a guarantee to prove every true
statement or to discover a proof automatically.

The initial proposition language contains equality of immutable terms, linear integer
inequalities, conjunction, implication, and nominal type/capability witnesses. Integer terms
contain constants, variables, addition, subtraction, and multiplication by a literal.
Type equality uses finite structural comparison and nominal declaration identity; recursive
type unfolding is not an unrestricted proof computation.

Proofs are finite, acyclic certificate trees. Constructors include assumptions, reflexivity,
symmetry, transitivity, congruence, conjunction introduction/elimination, implication
introduction/application, and checked linear-arithmetic certificates. Implication introduction
discharges a scoped assumption; an assumption cannot become an unconditional exported fact.
Linear certificates combine inequalities with nonnegative coefficients and check normalization
using arbitrary-precision integers. They may be incomplete for integer arithmetic.
Explicitly encoded domain bounds justify reasoning about machine integers; arithmetic
operations require no-overflow evidence or use their specified modular semantics.

There is no general recursion, arbitrary Cinder evaluation, user axiom declaration, or
unbounded proof search inside verification. Optional bounded automation produces certificates
checked by the same kernel. Finite verification can still be expensive; implementation resource
limits report failure to check, never successful proof.

Examples of admissible statements:

```text
n >= 0  |- n + 1 > 0
i >= 0, i < n |- i is a valid index into an immutable array of length n
proof(A = B)  |- equality transport between A and B
```

Equality transport preserves region identity and capabilities; it cannot change `read` to
`own`. A nominal capability witness proves a fact about authority but does not duplicate the
authority itself. Resource-bearing tokens remain in `Delta`, not in erased proof data.

Proofs refer to stable identities, not variable spellings. A write creates a new storage
version, so facts about the old contents cannot establish facts about the new contents.
Frozen values support persistent facts. Facts established under an inspection loan can
justify operations within that loan's validity; retaining a proof of the old snapshot never
authorizes an operation on changed storage. Runtime tests can introduce conditional evidence
in a successful branch, which is erased afterward; the runtime test itself is not erased.

## Examples and expected outcomes

```text
x = new_buffer()                 // own(r, buffer)
push(edit x, 1)                  // accepted; temporary exclusive loan
n = length(inspect x)            // accepted; observation ends after call
push(edit x, 2)                  // accepted
publish(x)                      // shared_read parameter: copy, then freeze copy
push(edit x, 3)                  // accepted: x still owns mutable r
y = clone(x)                    // new region s, if buffer supports cloning
push(edit y, 3)                  // accepted; r is unchanged
```

```text
v = inspect x                   // scoped view into mutable x
push(edit x, 1)                  // rejected if v is used afterward
consume_observation(v)           // last carrier use
push(edit x, 1)                  // accepted after the loan ends
```

Required negative cases also include freezing with an active edit loan, storing an edit
token in a duplicable closure, returning a local loan, using an old bounds proof after a
length-changing write, and declaring a capability constructor that duplicates an owner.

## Relationship to Cinder and next validation

This proposal changes ordinary argument passing: immutable sharing becomes a permanent state
transition for mutable inputs. It also separates duplication of a shared handle from deep
copying. These are observable language choices requiring review against Cinder's value
semantics and existing APIs; this document does not silently amend those specifications.

The current borrowing analysis and bounded refinement reasoning are useful implementation
starting points. A future elaborator would make freeze, capability transfer, loans, cleanup,
and proof erasure explicit. Staging and effects remain separate judgments.

Before implementation, formalize a small sequential operational semantics and establish
preservation, progress modulo runtime failures, absence of conflicting access, permanent
freeze, nonescape of dead storage, and sound proof erasure. Then translate representative
Cinder examples to measure which existing special cases disappear. Memory reclamation for
shared data, surface annotations, thread transfer, and cancellation remain design work.
