# 65. Compile-Time Execution in the Compiler

**Status:** Implemented

How the reference compiler runs compile-time code: it lowers that code to HIR and executes it on the same bytecode VM that runs programs, in the middle of type checking.

## What runs at compile time

Compile-time code includes `static let` initializers, `static assert` and `static if` conditions, `static for` iterables, splice operands, item-level splices, `deriving` clauses, and calls to `static def` functions ([Compile-Time Execution](../03-advanced/31-compile-time-execution.md)). Each of these produces a value the checker needs before it can continue: a constant that appears in a type, a branch to keep, or syntax to splice in.

## One executor

Compile-time code is not interpreted from the AST. It goes through the same path as run-time code: check, lower to HIR, compile to bytecode, run on the VM. The language's semantics are therefore implemented in one lowering and two backends, and compile-time results cannot drift from run-time results.

## How a compile-time call runs

The checker cannot call HIR lowering or the VM directly, because both depend on the checker's output. Two interfaces in `src/semantic/comptime_backend.h` break the cycle:

- The checker implements `comptime_program`. It hands out checked functions ready to lower, checking a function out of turn if compile-time code reaches it before the checker's normal walk does.
- `src/comptime_vm/` implements `comptime_backend`. It lowers the callee and every function its lowered code names, compiles them into one bytecode module, and runs it.

A compile-time expression that is not a call, such as a `static assert` condition, is lowered as the body of a synthetic function. Names it reads from its surroundings, such as a `static for` loop variable or a type parameter, are passed in as parameters.

Arguments and results cross the boundary through `src/comptime_vm/values.cpp`. Each conversion is given the type the checker assigned, and builds or reads the VM's representation using the same heap layout compiled code uses. Integers, floats, `bool`, `char`, `unit`, `str`, tuples, arrays, `list[T]`, structs, sum types, `option`, and `result` convert in any nesting. Quoted syntax (`expr`, `stmt`, and related types) is held by handle.

## Limits on a compile-time run

- **No effects.** A compile-time run uses the VM's hosted mode with effects disallowed. It cannot perform I/O or reach anything outside the VM.
- **Step budget.** A run stops with a diagnostic when it exceeds a fixed step budget. The diagnostic says this usually means a loop or recursion that never ends.
- **Host calls for syntax.** Building quoted syntax and calling the `expr.*` builders are host calls (`op_host_call`) from the VM back into the compiler. `src/comptime/fragments.cpp` builds the nodes, and `src/comptime/hygiene.cpp` renames bindings introduced inside a quote so a splice cannot capture names at the splice site.
- **Unsupported conversions fail closed.** A value whose type has no conversion yet (closures, slices, types as values) produces an error naming the type.

## Consistency with the final program

A compile-time run lowers functions while checking is still in progress, from the checker's records at that moment. Setting `CINDER_COMPTIME_VERIFY` makes the driver re-lower every function that ran at compile time from the final results and report any difference. This checks that the checker's mid-session view held every record lowering reads.

## See also

- [Compile-Time Semantics](../03-advanced/32-compile-time-semantics.md) — the language-level evaluation model.
- [HIR and the Bytecode VM](66-hir-and-bytecode-vm.md) — the VM these runs use.
