I found several substantive design problems, alongside documentation drift. The highest priorities are dependent-type checking, the machine safety boundary, and cancellation/lifetime rules.

This is a review of the specification as written. I have not verified these findings against compiler execution; where the text describes an implementation bug, I distinguish that from a design contradiction.

1. Unsupported dependent expressions silently disable type checking.
   Chapter 33 says expressions such as m * n, n / 2, and function calls in type arguments become unknown, “unify with everything,” and produce no diagnostic. That conflicts with the same chapter’s promise never to accept an unproven obligation. Array lengths and state parameters cannot safely become wildcards merely because their expressions exceed the solver’s capabilities.

   Recommendation: preserve an opaque symbolic expression, evaluate it when concrete, or reject an unresolved obligation. Reserve error-recovery types for programs that already have an error. Dependent types, line 46 (spec/specification/03-advanced/33-dependent-and-refinement-types.md:46)
2. The specified value-parameter compatibility test is mathematically incorrect.
   The claimed exact test uses coefficient GCD and a sign check. Those conditions are insufficient over nonnegative integers: 2*m + 3*n = 1 passes both tests but has no solution.

   More fundamentally, satisfiability is appropriate for solving fresh inference variables, but not for establishing equality between fixed, unknown parameters. Two independently supplied lengths cannot be treated as equal because some assignment would make them equal.

   Recommendation: distinguish flexible inference variables from rigid parameters; solve the former and require proof of equality for the latter. Chapter 63 already describes that distinction, contradicting chapter 33’s statement that values are not solved and propagated. Compatibility rules (spec/specification/03-advanced/33-dependent-and-refinement-types.md:48), inference engine (spec/specification/05-implementation/63-type-checking-and-inference.md:21)
3. machine does not establish a safe calling boundary.
   Ordinary code may call read_slot, drop_first, and as_slice, even though callers must establish initialization and ownership conditions. Putting their bodies inside machine cannot make arbitrary calls safe: reading the same owning slot twice or dropping uninitialized slots remains invalid.

   Recommendation: distinguish “contains unsafe implementation” from “requires caller-provided safety guarantees.” The latter needs an explicitly unsafe calling context; safe wrappers must validate or encode those guarantees. Machine functions (spec/specification/03-advanced/38-machine-layer.md:42)
4. on(io)’s concurrency exemption contradicts the scheduler.
   Chapter 30 exempts on(io) from send/share checks because it stays on one thread. Chapter 69 permits multiple I/O workers through CINDER_IO_WORKERS and describes replacement workers during blocking waits. The documented exemption therefore rests on a guarantee the runtime does not provide.

   Recommendation: make thread affinity an executor capability with enforceable semantics, or apply transfer checks to on(io) too. Runtime configuration must not invalidate a compiled program’s safety assumptions. Exemption (spec/specification/02-intermediate/30-data-race-freedom.md:53), worker model (spec/specification/05-implementation/69-task-scheduler.md:9)
5. race cancellation leaves borrowed-data lifetimes unresolved.
   Cancellation is cooperative, but race cancels losers without waiting for them. The specification does not explain what keeps a losing task’s borrowed inputs alive after the race returns, or when those inputs become available for mutation again.

   This is a missing safety rule, not sufficient evidence by itself of a compiler bug.

   Recommendation: join losers before releasing their borrows, restrict detached losers to owned inputs, or return a cleanup object that retains the loans until shutdown completes. Race behavior (spec/specification/02-intermediate/28-crew-par-race.md:76), scheduler exception (spec/specification/05-implementation/69-task-scheduler.md:55)
6. Panic cleanup has incompatible guarantees.
   Chapter 17 promises every live destructor runs during a panic. Chapter 69 explicitly says panicking tasks skip their drops. Because an unawaited task panic can leave the program running, skipped lock guards and resources can affect surviving code.

   Recommendation: choose and specify abort versus unwind behavior, including panics during destruction. If task panics are recoverable at the process level, cleanup must have a coherent guarantee. Drop guarantee (spec/specification/02-intermediate/17-shared-ownership-and-drop.md:84), actual scheduler description (spec/specification/05-implementation/69-task-scheduler.md:27)
7. Several iterator APIs cannot satisfy the ownership rules as declared.
   peek returns option[T] repeatedly without consuming the stored element, but has no copy bound. values converts &T to owned T without such a bound. scan yields its state while also needing to retain it for the next step.

   Separately, the list chapter explicitly documents that v.at(i) bypasses the ownership restriction applied to v[i] and returns a bitwise copy of a non-copy element.

   Recommendation: make peek return a borrow; require copy for values; provide explicit cloning alternatives; redesign scan to separate retained state from yielded output. Enforce ownership on the called API, not only indexing syntax. Adapters (spec/specification/04-stdlib/algorithms/49-lazy-adapters.md:22), indexing loophole (spec/specification/04-stdlib/collections/43-list.md:33)
8. fn mixes incompatible callable models.
   Chapter 16 presents fn(A) -> B as a concrete callable hidden behind monomorphization, with heterogeneous storage requiring unique[fn(...)]. But std.test stores different callbacks in a homogeneous list[test_case], whose run field is plain fn().

   Recommendation: specify separately—or explicitly unify—function pointers, generic callable parameters, opaque closure returns, and erased stored closures. Include calling rights: read-only, mutating, and consuming. Closure model (spec/specification/02-intermediate/16-closures-and-capture.md:39), test callbacks (spec/specification/04-stdlib/testing/61-std-test.md:63)
9. Implicit generic inference contradicts itself within one chapter.
   Functions supposedly infer their most general types from their bodies, never from callers. A few paragraphs later, implicit generics are checked only when instantiated with concrete call arguments, and no bounds are inferred. Those are different language models.

   Recommendation: describe omitted parameter types as implicit generic parameters with deferred checking, if that is intended. Preserve the separate guarantee that explicitly bounded generics are checked against their declarations. Functions (spec/specification/01-core/04-functions.md:20)
10. Opaque return types have an unnecessary collection restriction.
    Chapter 24 says every call site gets a distinct some trait type, so opaque values cannot inhabit a homogeneous collection. That is not a necessary consequence of static dispatch.

    Recommendation: give an opaque return type identity per declaration and generic substitution. Then [make_iter(), make_iter()] can contain values of the same hidden type, while results from unrelated factories can remain distinct. Opaque identity (spec/specification/02-intermediate/24-trait-objects.md:27)
11. Compile-time confluence conflicts with observable splice order.
    Chapter 32 says emitted definitions cannot be observed and elaboration order cannot affect meaning. Chapter 31 says later splices can see implementations emitted by earlier splices. Trait queries and method selection can therefore observe the evolving program.

    Recommendation: define explicit elaboration phases and the snapshot each query observes. Deterministic source ordering is a possible design, but it is weaker than order-independent elaboration. Confluence (spec/specification/03-advanced/32-compile-time-semantics.md:40), splice visibility (spec/specification/03-advanced/31-compile-time-execution.md:160)
12. The string ownership model is incomplete.
    str is a copyable borrowed view, yet interpolation, concatenation, replacement, and case conversion allocate new storage and return str. The specification does not clearly identify the owner of those buffers or when they are reclaimed.

    Recommendation: define an owned string type, a managed shared-string representation, or an explicit arena lifetime model. Also specify whether byte slicing at a non-scalar boundary panics or can produce invalid UTF-8; current text permits ambiguity. String representation and operations (spec/specification/04-stdlib/strings-and-formatting/52-std-string.md:9), invalid string slicing (spec/specification/04-stdlib/io-and-platform/57-std-fs-path.md:91)
13. Receiver ownership needs one authoritative rule.
    Ownership describes self/mut self as borrowed receivers for heap-represented types. The iterator chapter says next(mut self) moves the iterator; the I/O chapter likewise says each operation consumes its reader or writer. Neither operation returns the consumed receiver, so repeated use would be impossible under that description.

    Recommendation: specify borrowed, mutably borrowed, and consuming receivers explicitly, independently of runtime representation. Ownership (spec/specification/02-intermediate/14-ownership-and-borrowing.md:132), iterator receiver (spec/specification/04-stdlib/algorithms/48-iterator-protocol.md:16), I/O receiver (spec/specification/04-stdlib/io-and-platform/54-std-io.md:72)

There are also worthwhile design improvements:

- Make default arguments definition-scoped but evaluated per call. Evaluation timing does not require helper names to be visible to the caller. The current rule unnecessarily exposes implementation details of public functions. Defaults (spec/specification/01-core/04-functions.md:79)
- Specify the safety consequences of disabling contracts. Preconditions feed proofs that can remove bounds checks. --no-contract-checks therefore needs a precise account of what happens when an assumed contract is false. Contracts (spec/specification/03-advanced/34-contracts.md:39)
- Make iteration depend on types rather than spelling. Currently for x in &v and let r = &v; for x in r have different borrowing/element rules. Extracting an expression into a local should preserve behavior. Iteration dispatch (spec/specification/04-stdlib/algorithms/48-iterator-protocol.md:41)
- Give static assert a strictly compile-time meaning. Silently turning an assertion involving runtime input into a runtime check weakens what its spelling promises. Ordinary assert already provides that behavior. Static assertions (spec/specification/03-advanced/31-compile-time-execution.md:40)
- Specify numeric edge cases directly. “C++ conversion semantics” leaves Cinder’s behavior insufficiently defined for out-of-range floats, NaNs,

Finally, the specification needs an editorial consistency pass. For example, Meta Queries is marked Implemented but says “Nothing in this chapter exists yet”; Sorting says sort does not exist immediately before describing its implementation; and the string search example reports offset 9 where "  Hello, Cinder  ".find(" Cinder") should report 8. Meta Queries (spec/specification/04-stdlib/type-traits/60-meta-queries.md:73), Sorting (spec/specification/04-stdlib/algorithms/51-sorting-and-searching.md:21), string example (spec/specification/04-stdlib/strings-and-formatting/52-std-string.md:86)
