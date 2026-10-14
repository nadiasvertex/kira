# 64. Proof Solving

**Status:** Implemented

How the reference compiler proves facts about values it does not know: the solver's fragment, how source predicates reach it, how facts accumulate along a function, and what a proof changes in the compiled program.

## Role

Compile time has two machineries ([Compile-Time Semantics](../03-advanced/32-compile-time-semantics.md)). Evaluation runs code on known values; see [Compile-Time Execution in the Compiler](65-compile-time-execution-in-the-compiler.md). Reasoning proves facts about unknown values. The reasoning engine discharges refinement predicates, dependent-type obligations, `pre`/`post`/`invariant` conditions, and index bounds. It decides one question: whether a set of facts entails a goal.

## The solver

The solver (`src/semantic/reason.h`, `reason.cpp`) works in quantifier-free linear integer arithmetic over uninterpreted atoms.

- A **constraint** is a linear polynomial related to zero by `==`, `!=`, or `>=`. Other comparisons normalize to these; `>` becomes `>= 1` because the domain is the integers.
- An **atom** is anything the solver does not interpret: a name, a field projection such as `x.len`, or a call such as `len(v)`. Two occurrences of the same atom are the same unknown.
- A **goal** is in disjunctive normal form. Disjunction only comes from a predicate the user wrote with `or`.
- The decision procedure is Fourier-Motzkin elimination over the rationals. If the facts plus the negated goal have no rational solution, the goal is **proved**. If the facts plus the goal have none, it is **refuted**. Otherwise the answer is **unknown**.

Both definite answers are sound, because no rational solution implies no integer solution. The procedure is incomplete in the other direction, which is where `unknown` comes from. The solver always terminates: it has no search, timeout, or configuration. Obligations with more than a fixed number of atoms, or that grow too many intermediate constraints, are answered `unknown` rather than risk exponential time.

Each constraint keeps the source spelling it came from, so a failed proof can show what was known and what was needed in the user's own terms.

## From source to constraints

`predicates.cpp` translates source expressions into solver terms. A predicate becomes a goal when it must be proved and a fact set when it may be assumed. The parts outside the linear fragment are handled differently in each case: an unrepresentable goal cannot be proved, and an unrepresentable fact is dropped. Translation applies a substitution, because a predicate is written about `self` and the declaring type's value parameters. For example, `index[n]`'s `self < n`, applied to argument `i` in a function with its own `n`, becomes the goal `i < n`. One polynomial builder (`linear_poly`) serves both the proof engine and the value solver.

## Facts along a function

The narrowing engine (`narrowing.cpp`) tracks which facts hold at each point in a function body. Facts come from parameter refinements and domains (a `usize` is `>= 0`), earlier `pre` conditions, `let` bindings with known values, and branch conditions. A fact is visible exactly in the region its source dominates: a branch's condition holds only inside that branch, and the fact set shrinks back when the region ends. When a value is assigned, facts about it are dropped. The engine holds no checker state and emits no diagnostics; it returns the pieces a diagnostic needs and the checker words the message.

## Outcomes

- **Proved.** The obligation is discharged. A narrowing coercion to a refined type is accepted. An index proved in bounds has its run-time check removed. A contract proved on every call emits no check at all.
- **Refuted.** The code is wrong on every execution that reaches it. This is always an error.
- **Unknown.** For a refinement, the checker reports what it needed and what it knew, and points at the two remedies: add a fact the solver can use, or convert at run time with `try_from`. For a contract, lowering emits a run-time check that panics on violation, unless contract checks are turned off with `--no-contract-checks` ([Contracts](../03-advanced/34-contracts.md)).

## `try_from` proofs

`T.try_from(x)` for a refined `T` is a run-time proof. `try_from_proof.cpp` rewrites it into ordinary code that binds `x`, tests the refinement's predicate with `self` and the value parameters replaced by the actual value and constants, and produces `@some(x)` or `@none`. The checker types the rewritten tree like any other code. Inside the `@some` arm the predicate is a fact, so later obligations can use it. When a value parameter is still open (inside a generic over `n`), the check is generated per instance.

## See also

- [Dependent and Refinement Types](../03-advanced/33-dependent-and-refinement-types.md)
- [Contracts](../03-advanced/34-contracts.md)
