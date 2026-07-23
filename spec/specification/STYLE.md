# Specification Style Guide

Internal guide for writing chapters in this directory. This file is not part of the published spec's narrative — it governs how the other files are written. (It may be deleted once the specification is complete and stable, at the author's discretion.)

## Voice

Write like the ISO C++ standard or the C++ standard library reference (cppreference's terse mode), not like a tutorial. This is Layer/Section material for people (and LLMs) who already know the language exists and need the precise rule. `spec/kira-reference.md` and `spec/std-reference.md` were tutorials; this replaces them with something else. A tutorial explains *why* and *how to learn*; this states *what is true*.

- Declarative sentences. "A `let` binding is immutable." not "You use `let` to create an immutable binding."
- No motivating preamble ("Sometimes you want to..."). State the construct, its syntax, its semantics, its rules.
- Keep prose short; prefer a list of rules to a paragraph of explanation.
- Code examples are allowed but minimal — one canonical example per construct, not a tour. Do not repeat an example to show "another way to do the same thing" unless the second form has different semantics.
- Cross-reference other chapters by title and relative path, e.g. `see [Traits](../intermediate/18-traits.md)`.

## Required chapter header

Every chapter file starts with:

```markdown
# <N>. <Title>

**Status:** Implemented | Partial | Planned

<one-sentence scope statement: what this chapter covers>
```

Status meanings (verify against the actual compiler/stdlib source before assigning — do not trust a design doc's aspirational description):
- **Implemented** — works end-to-end today; you could write this code and compile/run it. Grep `src/` and `src/testdata/` for evidence (a passing test, a real `.kira` stdlib file) before marking this.
- **Partial** — some of the chapter's surface works, some does not. State exactly which parts, in a "## Implementation status" subsection near the end, with specifics (function names, file:line if useful, or "parses but does not type-check/lower").
- **Planned** — designed but not implemented (e.g., only parser/lexer support exists, or it's pure design-doc content with no code). Still write the chapter normatively (as if describing the target design), but the status line must say so and a closing note should say what's missing.

A chapter that mixes implemented and planned material must not hide that in prose — use the "## Implementation status" subsection.

## Structure within a chapter

Typical shape (omit sections that don't apply):

1. Header (above)
2. Syntax — grammar sketch or production reference (point to `spec/kira-grammar.ebnf` by production name rather than re-deriving EBNF, unless the chapter's whole point is grammar)
3. Semantics — the rules, stated as numbered or bulleted normative statements
4. One example
5. Edge cases / errors — what's diagnosed and how, only if noteworthy
6. Implementation status (if Partial)
7. See also

## Content sourcing

Chapters are assembled from, in priority order when they conflict:
1. Actual source code in `src/` (ground truth for behavior)
2. Design docs in `spec/*.md` (the ones being folded in and deleted)
3. The old `spec/kira-reference.md` / `spec/std-reference.md` (tutorial prose — mine for facts, discard the tutorial voice)

When a design doc describes something not yet built, keep the content (mark Planned/Partial) rather than deleting it — this reorg must not lose information, only restructure and re-voice it.

## Cross-linking

- Each chapter links forward/backward to adjacent chapters in its section only where genuinely relevant (a "borrowing" chapter linking to "views" because views are a consequence of borrowing) — not a mechanical prev/next footer.
- Link to `spec/kira-grammar.ebnf` for grammar, `spec/CONVENTIONS.md` never (that's C++ style, unrelated).

## What NOT to do

- Do not write a "Motivation" or "Why" section as a separate heading — a single clause inline is fine if truly needed, but this is not a design-rationale document. (Design rationale that's genuinely load-bearing — e.g., *why* borrows can't escape a call, which explains half a dozen downstream rules — may stay, briefly, since it's necessary to understand the rules; but trim aggressively.)
- Do not duplicate the same explanation across chapters; link instead.
- Do not invent new examples with new scenarios (imaginary business domains); prefer the existing examples from the source material, trimmed.
