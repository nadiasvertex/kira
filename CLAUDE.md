# Project

 Cinder is an early-stage language and compiler project written in C++26 (Clang 23.1+), built with Bazel/bzlmod. 

- Language and standard library specification: spec/specification/ (start at spec/specification/00-overview.md)
- Language grammar: spec/cinder-grammar.ebnf

CRITICAL: When writing cinder code that doesn't compile but should according to the spec, don't try to work around compiler limitations. Stop and identify the limitations so that we can build a better compiler.

# Default Response Rule

**Assume I want the short version.** Give me the important facts, the result, and any necessary next steps. Only provide a detailed explanation, extensive reasoning, historical context, or alternative approaches when I ask for it or when it is necessary to avoid misunderstanding.

# Communication Style

Communicate clearly, directly, and concisely.

* **Lead with the answer.** Start with the important result, decision, or relevant facts. Do not build up to the conclusion.
* **Be brief by default.** Give me the minimum explanation necessary for me to understand what happened and what matters.
* **Prioritize information over narrative.** Do not turn technical work into a story, journey, or account of your "process." I generally do not need to know how you arrived at an answer.
* **Do not narrate your struggles.** If something could not be completed, briefly state:

  1. what could not be completed,
  2. why, and
  3. what, if anything, I need to do.

  Do not provide a lengthy account of attempts, dead ends, discoveries, or obstacles unless I specifically ask for that detail.
* **Avoid unnecessary precision.** Include exact details when they affect correctness or a decision. Otherwise, prefer simple explanations over exhaustive qualifications and edge cases.
* **Do not manufacture complexity.** A straightforward task should receive a straightforward answer. Do not add elaborate structure, terminology, caveats, or background merely because they are available.
* **Do not repeat information.** Once something has been established, do not restate it in multiple forms unless repetition materially improves clarity.
* **Use lists when they make information easier to scan.** Prefer short bullet points over long explanatory paragraphs for summaries, status updates, and results.
* **Do not turn every answer into a narrative.** Avoid metaphors, dramatic framing, fictional scenarios, or storytelling unless they are genuinely useful or explicitly requested.
* **Do not explain obvious things.** Assume I understand common programming concepts and terminology. Explain unfamiliar or non-obvious details when they matter.


# Commands

```sh
just build      # bazelisk build //src:cinder
just test       # bazelisk test //...
just run        # bazelisk run //src:cinder
just package    # release archives (tar.bz2, .deb on Linux) under dist/
just format     # regenerate compile_commands.json, run clang-tidy --fix over src/
```

Run a single test target directly with bazel/bazelisk, e.g.:

```sh
bazelisk test //src/parser:parser_test
bazelisk test //src/semantic:check_test
bazelisk test //src:cli_test
```

Run the compiled binary against a source file:

```sh
bazelisk run //src:cinder -- path/to/module.cn
bazelisk run //src:cinder -- --metadata-dir build/meta path/to/module.cn
```

Tests are hand-rolled binaries (no gtest) using `cinder::testing::expect`/`fail` from `src/testing/test_assert.h`; a failing `expect()` calls `std::exit(1)` with a message, so add new checks as additional `expect(...)` calls rather than introducing a framework.

# A test must be able to fail

A test that cannot fail is worse than no test: it costs the same to run and maintain, and it actively buys false confidence. Before trusting a new test, make it fail on purpose — break the code, or feed the assertion a wrong expected value — and confirm you see the failure message you expected. Then undo it. A test whose failure you have never observed has not been tested.

The way this goes wrong here is rarely a forgotten assertion. It is an assertion that is real but weaker than it looks:

- **Differential tests are blind to shared bugs.** `codegen_stress_test` cross-checks the bytecode VM against the LLVM tier, which is a strong check for one tier drifting from the other — and no check at all for both being wrong the same way. Two backends reading the same layout rules from `src/runtime/layout.h` share their assumptions, so they fail *together*: a real instance had `&mut xs[2]` on a `list` addressing bytes inside the `{len, cap, data}` header in both tiers, returning the same wrong answer, with agreement perfectly satisfied. Corpus files may declare `# expect: <integer>`, checked in addition to agreement — **use it for anything touching layout, addressing, or representation**, where agreement is exactly the check that will not save you.
- **"Compiles cleanly" is not "behaves correctly."** Much of this pipeline can accept a program and lower it to something subtly wrong. Prefer asserting a computed value over asserting the absence of diagnostics.
- **A test that passes the first time deserves suspicion.** Confirm it exercises the path you think it does — a spike that silently hits a different code path (an unintended prelude name, a skipped lowering) will pass without ever reaching the new code.

Relatedly, when a diagnostic-quality bug and a correctness bug are both plausible explanations for a surprising result, establish which one you are looking at before fixing anything. ``expected `box`, found `box` `` looked like a type-interning bug and was actually the prelude `box` silently shadowing a user type — the kind of message this compiler's stated philosophy forbids, and a reminder that a confusing diagnostic can masquerade as a deeper defect.

# Architecture

The compiler above all is meant to teach the user how to use the language. Friendliness and clarity of diagnostics are prioritized over performance or cleverness.

- `spec/`: `specification/` (the normative language and standard library specification — Core/Intermediate/Advanced sections plus a stdlib section, one chapter per feature, each with an implementation-status marker; start at `specification/00-overview.md`), `cinder-grammar.ebnf` (grammar sketch), `CONVENTIONS.md` (authoritative C++ style rules), `todo.md` (known compiler gaps and bugs).

- `spec/`: `specification/05-implementation/` is a description of how the compiler is implemented. Keep it up to date when we change the structure of the system.

## C++ Conventions

Full rules in `spec/CONVENTIONS.md`; the essentials:

- All names (including namespaces) are `snake_case`. `#pragma once`, not header guards.
- Functional-first: prefer pure free functions, value semantics, `const`/`constexpr`. One logical public entity per header; prefer forward declares.
- `std::expected<T, E>` for recoverable errors, `std::optional<T>` for "maybe" values; reserve exceptions for truly exceptional, non-local failures.
- `std::move` only when consuming a named object in the current scope; `std::forward` only in deduced forwarding-reference parameters; never `return std::move(local);` — let NRVO/copy elision work.
- No raw owning pointers or naked `new`/`delete`; prefer `std::unique_ptr`, `std::span`/views for non-owning access.
- Must build warning-free with `-std=c++2c -Wall -Wconversion -Wsign-conversion -Wshadow -Wdeprecated -Wold-style-cast -Werror`.
- Prefer small, targeted parser/semantic changes over new abstractions; add or update a regression test whenever parser or semantic behavior changes.
