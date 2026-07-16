1. Modules as compile-time values (signatures, parameterized modules) — literally doesn't exist yet: no signature keyword, no grammar, no symbol-table concept. Greenfield work, but self-contained (doesn't block other features).
2. A function with a return value that doesn't return a value should fail to compile.
3. Higher-kinded traits — parses, zero semantic model of type constructors.
4. Contracts on a `generator def` (its body runs in steps, so entry/exit don't mean what they mean for a call — lowering rejects them rather than checking at the wrong times).
5. A value parameter no argument determines (`def zeros[n: usize]() -> array[int32, n]`, with no explicit `f[8]()` call syntax) is diagnosed, not solved; 
6. Const-generic *methods* (on an `impl`/`extend` target) still fall back to the template path and are refused by lowering; 
7. Type-generic monomorphization (`def identity[T](x: T)`) remains unimplemented.
