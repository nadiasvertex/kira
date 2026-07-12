1. Modules as compile-time values (signatures, parameterized modules) — literally doesn't exist yet: no signature keyword, no grammar, no symbol-table concept. Greenfield work, but self-contained (doesn't block other features).
2. Dependent/refinement types — currently a no-op that just aliases to the base type; real support needs predicate-carrying types and narrowing, which is a genuine type-theory problem, not just plumbing.
3. Higher-kinded traits — parses, zero semantic model of type constructors.
4. Contract enforcement — the one small one: contracts are checked but never lowered to a runtime assertion in bytecode/LLVM. Should be a contained fix once you get to it.
