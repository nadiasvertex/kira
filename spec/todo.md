1. Contracts on a `generator def` (its body runs in steps, so entry/exit don't mean what they mean for a call — lowering rejects them rather than checking at the wrong times).
2. A value parameter no argument determines (`def zeros[n: usize]() -> array[int32, n]`, with no explicit `f[8]()` call syntax) is diagnosed, not solved; 
3. Const-generic *methods* (on an `impl`/`extend` target) still fall back to the template path and are refused by lowering; 
4. Still open: type-generic *methods* on impl/extend blocks that aren't higher-kinded (those route through `instantiate_hk_method` today) and mixed value+type generics (`[n: usize, T]`), which still fail lowering closed.
5. Tail call optimization
6. Full Unicode support for `std.string` (spec/std-string.md ships Unicode *simple* 1:1 case mapping only). Deferred: full/special case mapping (`ß`→`SS`, final-sigma, locale-tailored Turkish/Azeri/Lithuanian), case folding for caseless comparison, NFC/NFD normalization, and grapheme-cluster segmentation (UAX #29) so `reversed`/`split` can operate on graphemes rather than scalars. Each needs additional generated Unicode tables.
