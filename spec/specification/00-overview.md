# 0. Overview

**Status:** Implemented

This is the normative specification of the Kira language and standard library. It states what is true of the language and its implementation, precisely and without tutorial framing. For a guided introduction, see the (forthcoming) tutorial; for the grammar, see `../kira-grammar.ebnf`; for C++ compiler-implementation conventions, see `../CONVENTIONS.md`.

## Structure

The specification is organized in three layered sections, followed by a standard library section:

- **Core** (`01-core/`) — values, functions, types, pattern matching, built-in collections, and error handling. Most programs are written using only Core material.
- **Intermediate** (`02-intermediate/`) — ownership and borrowing, traits and generics, and async concurrency. Needed for library code and concurrent programs.
- **Advanced** (`03-advanced/`) — compile-time execution, dependent and refinement types, contracts, concepts, modules as compile-time values, and the low-level `machine` layer.
- **Standard Library** (`04-stdlib/`) — the modules that ship with the compiler: collections, algorithms, string/formatting, and I/O/platform.

A later section never needs to be understood to use an earlier one. Within a section, each chapter is a single language or library feature, keeping the specification's chapters small and independently addressable — the goal, per the project's structuring intent, is that both a human and an LLM can find a specific rule without loading unrelated context.

Each chapter opens with a status line:

- **Implemented** — works today, end to end.
- **Partial** — part of the chapter's surface works; the chapter's "Implementation status" subsection says exactly which part.
- **Planned** — designed but not yet implemented; the chapter is still written normatively, as the target design, with a note on what's missing.

## Diagnostics

Kira's compiler treats diagnostics as part of the language's interface, not an afterthought. A diagnostic states what was expected, what was found, why the constraint exists, and — where applicable — how to satisfy it:

```
error[E0012]: type mismatch
  --> src/main.kira:14:5
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

- [The Prelude](stdlib/42-prelude.md) — names available in every module without a `use` declaration.
- `../kira-grammar.ebnf` — grammar productions referenced by name throughout.
