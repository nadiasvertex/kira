1. Modules as compile-time values (signatures, parameterized modules) —
   Phases 0–2 landed (see `spec/module-values-design.md` and
   `spec/module-values-implementation-plan.md`): `signature` keyword/grammar/
   AST/parser, parameterized `module m[P: sig]` declarations, `use m[args] as
   name` instantiation syntax, a `signature_symbol` symbol kind, signature-body
   well-formedness checking, and a *structural* `satisfies(module, signature)`
   check that runs at every `use m[args]` site with concept-quality
   diagnostics. Phase 3 *semantic materialization* also landed: a satisfied
   `use audited[postgres] as db` clones the functor's `def`s into a memoized
   synthetic module and binds the module parameter as an import alias, so
   `db.query(...)` resolves and type-checks and `DB.conn` resolves to the
   argument's concrete `conn`. **Codegen also landed:** the checker records
   each instantiation's cloned `def`s as `checked_types::functor_instance`s,
   `hir::lower_functor_modules` lowers them into standalone `hir_module`s
   appended by the driver's lowering stage, and an instantiated functor now
   runs end-to-end on both the bytecode VM and the LLVM/AOT backend (`db.open_
   and_ping(...)` executes). Remaining: functor bodies with non-`def` members;
   reflection (4); metadata (5); `static if` around `use` (6); and hardening
   (7, incl. deep type-equality in `satisfies`).
2. Contracts on a `generator def` (its body runs in steps, so entry/exit don't mean what they mean for a call — lowering rejects them rather than checking at the wrong times).
3. A value parameter no argument determines (`def zeros[n: usize]() -> array[int32, n]`, with no explicit `f[8]()` call syntax) is diagnosed, not solved; 
4. Const-generic *methods* (on an `impl`/`extend` target) still fall back to the template path and are refused by lowering; 
5. Type-generic monomorphization (`def identity[T](x: T)`) remains unimplemented.
6. Lambdas containing string interpolation failing to lower ("captured variable could not be found")
7. Tail call optimization
