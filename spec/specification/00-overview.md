# 0. Overview

**Status:** Implemented

This is the normative specification of the Cinder language and standard library. It states what is true of the language and its implementation, precisely and without tutorial framing. For a guided introduction, see the (forthcoming) tutorial; for the grammar, see `../cinder-grammar.ebnf`; for C++ compiler-implementation conventions, see `../CONVENTIONS.md`.

## Structure

The specification is organized in three layered sections, followed by a standard library section:

- **Core** (`01-core/`) — values, functions, types, pattern matching, built-in collections, and error handling. Most programs are written using only Core material.
- **Intermediate** (`02-intermediate/`) — ownership and borrowing, traits and generics, and async concurrency. Needed for library code and concurrent programs.
- **Advanced** (`03-advanced/`) — compile-time execution, dependent and refinement types, contracts, concepts, modules as compile-time values, and the low-level `machine` layer.
- **Standard Library** (`04-stdlib/`) — the modules that ship with the compiler: collections, algorithms, string/formatting, I/O/platform, type traits/limits/meta queries, and unit testing.

A later section never needs to be understood to use an earlier one. Within a section, each chapter is a single language or library feature, keeping the specification's chapters small and independently addressable — the goal, per the project's structuring intent, is that both a human and an LLM can find a specific rule without loading unrelated context.

Each chapter opens with a status line:

- **Implemented** — works today, end to end.
- **Partial** — part of the chapter's surface works; the chapter's "Implementation status" subsection says exactly which part.
- **Planned** — designed but not yet implemented; the chapter is still written normatively, as the target design, with a note on what's missing.

## Table of contents

### Core (`01-core/`)

1. [Syntax Basics](01-core/01-syntax-basics.md)
2. [Built-in Types](01-core/02-built-in-types.md)
3. [Bindings](01-core/03-bindings.md)
4. [Functions](01-core/04-functions.md)
5. [Lambdas](01-core/05-lambdas.md)
6. [Collections: `list` and `array`](01-core/06-collections-list-array.md)
7. [Strings](01-core/07-strings.md)
8. [Control Flow](01-core/08-control-flow.md) — Partial
9. [Pattern Matching](01-core/09-pattern-matching.md)
10. [Type Declarations](01-core/10-type-declarations.md)
11. [Error Handling](01-core/11-error-handling.md)
12. [Modules and Imports](01-core/12-modules-and-imports.md)
13. [Programs and `main`](01-core/13-programs-and-main.md) — Partial

### Intermediate (`02-intermediate/`)

14. [Ownership and Borrowing](02-intermediate/14-ownership-and-borrowing.md)
15. [Views](02-intermediate/15-views.md) — Partial
16. [Closures and Capture](02-intermediate/16-closures-and-capture.md) — Partial
17. [Shared Ownership and Drop](02-intermediate/17-shared-ownership-and-drop.md) — Partial
18. [Traits](02-intermediate/18-traits.md)
19. [Generics and Inference](02-intermediate/19-generics-and-inference.md)
20. [Operator Overloading](02-intermediate/20-operator-overloading.md)
21. [Coherence and the Orphan Rule](02-intermediate/21-coherence-and-orphan-rule.md)
22. [Extension Methods](02-intermediate/22-extension-methods.md)
23. [Contracts on Trait Methods](02-intermediate/23-contracts-on-trait-methods.md) — Partial
24. [Trait Objects](02-intermediate/24-trait-objects.md) — Partial
25. [The Module System in Depth](02-intermediate/25-module-system-in-depth.md) — Partial
26. [Concurrency: The Execution Model](02-intermediate/26-concurrency-execution-model.md) — Planned
27. [`async`/`await` and Generators](02-intermediate/27-async-await-and-generators.md) — Partial
28. [`crew`, `par`, `race`](02-intermediate/28-crew-par-race.md) — Planned
29. [Channels and Sync Primitives](02-intermediate/29-channels-and-sync-primitives.md) — Planned
30. [Data-Race Freedom](02-intermediate/30-data-race-freedom.md) — Planned

### Advanced (`03-advanced/`)

31. [Compile-Time Execution](03-advanced/31-compile-time-execution.md)
32. [Compile-Time Semantics](03-advanced/32-compile-time-semantics.md)
33. [Dependent and Refinement Types](03-advanced/33-dependent-and-refinement-types.md) — Partial
34. [Contracts](03-advanced/34-contracts.md)
35. [Concepts](03-advanced/35-concepts.md)
36. [Modules as Compile-Time Values](03-advanced/36-modules-as-compile-time-values.md)
37. [Higher-Kinded Traits](03-advanced/37-higher-kinded-traits.md)
38. [The `machine` Layer](03-advanced/38-machine-layer.md) — Partial
39. [Tail-Call Optimization](03-advanced/39-tail-call-optimization.md)
40. [Advanced Concurrency: Execution Graphs](03-advanced/40-execution-graphs.md) — Planned
41. [Advanced Cancellation](03-advanced/41-advanced-cancellation.md) — Planned

### Standard Library (`04-stdlib/`)

42. [The Prelude](04-stdlib/42-prelude.md)

**Collections** (`04-stdlib/collections/`)

43. [`list[T]`](04-stdlib/collections/43-list.md)
44. [`deque[T]` and `bitset`](04-stdlib/collections/44-deque-and-bitset.md) — Planned
45. [`ordered_map[K, V]` and `ordered_set[K]`](04-stdlib/collections/45-ordered-map-and-set.md) — Planned
46. [`unordered_map[K, V]` and `unordered_set[K]`](04-stdlib/collections/46-unordered-map-and-set.md) — Planned
47. [`small_list[T, N]`](04-stdlib/collections/47-small-list.md) — Planned

**Algorithms** (`04-stdlib/algorithms/`)

48. [The Iterator Protocol](04-stdlib/algorithms/48-iterator-protocol.md)
49. [Lazy Adapters](04-stdlib/algorithms/49-lazy-adapters.md)
50. [Aggregation](04-stdlib/algorithms/50-aggregation.md)
51. [Sorting and Searching](04-stdlib/algorithms/51-sorting-and-searching.md)

**Strings and Formatting** (`04-stdlib/strings-and-formatting/`)

52. [`std.string`](04-stdlib/strings-and-formatting/52-std-string.md)
53. [`std.format`](04-stdlib/strings-and-formatting/53-std-format.md)

**I/O and Platform** (`04-stdlib/io-and-platform/`)

54. [`std.io`](04-stdlib/io-and-platform/54-std-io.md) — Partial
55. [`std.console`](04-stdlib/io-and-platform/55-std-console.md)
56. [`std.platform`](04-stdlib/io-and-platform/56-std-platform.md)
57. [`std.fs.path`](04-stdlib/io-and-platform/57-std-fs-path.md)

**Type Traits** (`04-stdlib/type-traits/`)

58. [`std.traits` — Type Predicates and Transformations](04-stdlib/type-traits/58-std-traits.md)
59. [`std.limits` — Numeric Limits](04-stdlib/type-traits/59-std-limits.md)
60. [Meta Queries](04-stdlib/type-traits/60-meta-queries.md)

**Testing** (`04-stdlib/testing/`)

61. [Unit Testing (`std.test`)](04-stdlib/testing/61-std-test.md) — Partial

## Diagnostics

 Cinder's compiler treats diagnostics as part of the language's interface, not an afterthought. A diagnostic states what was expected, what was found, why the constraint exists, and — where applicable — how to satisfy it:

```
error[E0012]: type mismatch
  --> src/main.cn:14:5
   |
13 |     let x: int32 = compute()
14 |     process(x)
   |             ^ expected float64, found int32
   |
   = compute() returns int32
   = process() expects float64
   = hint: use float64(x), or change compute()'s return type
```

When a feature from a later section would resolve the diagnostic — for example, an ownership rule firing on code that has no annotations to explain it — the diagnostic names the concept and points at the section that documents it. This specification is the target those pointers resolve to.

## See also

- [The Prelude](04-stdlib/42-prelude.md) — names available in every module without a `use` declaration.
- `../cinder-grammar.ebnf` — grammar productions referenced by name throughout.
