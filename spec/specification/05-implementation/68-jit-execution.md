# 68. JIT Execution

**Status:** Partial

How the reference compiler runs LLVM-generated code in-process. A just-in-time path exists and is used to test the LLVM backend. A tiered JIT that promotes hot VM code to native code is designed but not built.

## What exists

`src/llvm_codegen/jit_support.cpp` wraps LLVM's ORC `LLJIT`. It takes a module produced by LLVM code generation ([LLVM and Ahead-of-Time Compilation](67-llvm-and-aot.md)), adds it to a JIT session, resolves runtime symbols such as the panic and allocation entry points from the host process, and calls a zero-argument entry function.

This path is used by `codegen_test` and the codegen stress test. They run each program on the bytecode VM and through the JIT and compare results, so the LLVM backend is checked by execution without linking an executable for every test case. The JIT runs the same IR, with the same runtime and layout rules, that `--compile` writes to an object file.

## What is planned

The planned design is a tiered runtime:

- Programs start on the bytecode VM.
- The VM counts calls to each function. A function that crosses a threshold is compiled through LLVM in the background.
- When native code is ready, the function's dispatch slot is swapped to point at it, so later calls from both tiers go to native code.
- Both tiers share the runtime library, heap layout and panic reasons, so a value created in one tier is valid in the other.

None of this is wired into the command-line driver. There are no call counters, no dispatch-slot swap, and no interaction between the JIT and the VM at run time. A program today runs entirely on the VM (`cinder FILE`) or entirely as native code (`cinder --compile FILE`).

## Implementation status

- Implemented: in-process execution of LLVM output through ORC `LLJIT`, used for backend testing.
- Not implemented: tier-up from the VM, call counting, dispatch-slot replacement, and any command-line option to run a program through the JIT.
