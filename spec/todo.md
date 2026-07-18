1. Contracts on a `generator def` (its body runs in steps, so entry/exit don't mean what they mean for a call — lowering rejects them rather than checking at the wrong times).
2. A value parameter no argument determines (`def zeros[n: usize]() -> array[int32, n]`, with no explicit `f[8]()` call syntax) is diagnosed, not solved; 
3. Const-generic *methods* (on an `impl`/`extend` target) still fall back to the template path and are refused by lowering; 
4. Type-generic monomorphization (`def identity[T](x: T)`) remains unimplemented.
5. Lambdas containing string interpolation failing to lower ("captured variable could not be found")
6. Tail call optimization
7. A hand-rolled type implementing std.iter.iterator[T] is not usable in a for loop.
