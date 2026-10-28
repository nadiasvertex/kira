# 67. LLVM and Ahead-of-Time Compilation

**Status:** Implemented

How the reference compiler produces native code: HIR is lowered to LLVM IR, optimized, emitted as an object file, and linked with Cinder's runtime support library into a standalone executable.

## Code generation

`src/llvm_codegen/codegen.cpp` lowers the same HIR the bytecode compiler reads into an `llvm::Module`. It reads types from the checker's type table and never re-infers.

- **Values.** Scalars are LLVM scalars. Every other value is allocated through one runtime entry point and laid out by `src/runtime/layout.h`, the same rules the VM uses.
- **Calls.** Direct calls name the instance's symbol. Closures and function values use indirect calls. A call that the HIR pass marked as a tail call becomes `musttail` when LLVM's preconditions provably hold and an ordinary call otherwise ([Tail-Call Optimization](../03-advanced/39-tail-call-optimization.md)).
- **Generators.** A `generator def` compiles to a small constructor that builds the generator's state block, plus a step function that resumes from the last `yield`. The state block holds the locals that the HIR pass found live across a `yield`. Both backends share one coroutine object of five slots: step function, state pointer, resume index, status, and the last yielded value (`src/runtime/coroutine.h`). The step function returns the new status (suspended or done) and writes a yielded item to the value slot; `.next()` call sites read the status and build `some(value)` or `none` themselves.
- **Panics.** Checked arithmetic and other failing operations call a runtime panic function with a reason from the same closed set the VM uses, followed by `unreachable`.
- **Intrinsics.** The small set of primitive operations the standard library is written over (allocation, string scalar decoding, platform queries, I/O) become calls into the runtime library.
- **Fails closed.** A HIR construct the backend cannot lower is reported with its source location, never miscompiled.

## Optimization

At `-O0`, the default, the emitted IR is not optimized. `-O1`, `-O2` and `-O3` run LLVM's standard per-module pipeline at that level (bare `-O` means `-O1`). HIR inlining has already run before code generation at every level.

## Producing an executable

`cinder --compile FILE` takes these steps (`src/driver/aot.cpp`, `src/llvm_codegen/aot.cpp`):

1. Lower every reachable module to LLVM IR.
2. Synthesize a C-ABI `int main(void)` that calls the program's entry function and turns its result into an exit code: `unit` is 0, and an integer or `bool` is converted to `int`.
3. Optimize at the requested level and emit a native object file for the host target.
4. Link the object with the system C++ driver against Cinder's runtime support archives: panic handling, the heap allocator, string operations, and the libraries these depend on.

The resulting executable has no dependency on LLVM.

## Relationship to the VM

The VM and the LLVM backend are separate implementations of the same HIR semantics. The codegen stress test compiles each program in `src/testdata/codegen_stress/` with both and requires them to agree. Because the two backends share layout rules, they can share a mistake and still agree, so a corpus file may also state its expected result with `# expect:`, which is checked independently of agreement.

## See also

- [JIT Execution](68-jit-execution.md) — running LLVM output in-process.
- [The `machine` Layer](../03-advanced/38-machine-layer.md) — the low-level surface that maps most directly onto generated code.
