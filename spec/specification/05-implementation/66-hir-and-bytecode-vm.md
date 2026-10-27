# 66. HIR and the Bytecode VM

**Status:** Implemented

The reference compiler's typed intermediate representation, the passes that run over it, and the bytecode compiler and virtual machine that execute it. The VM runs programs by default and runs all compile-time code.

## HIR

HIR (`src/hir/nodes.h`) is a typed tree. Every node carries a concrete type from the checker's type table, and every call names its target function or instance. By the time code is in HIR:

- generics are instantiated, so each instance is an ordinary function;
- operators, indexing, method calls and UFCS calls are direct calls;
- `for` loops over user iterators are desugared to `while let` over `next`;
- contract checks that were not proved are explicit nodes;
- drops are explicit calls at scope exits, with drop flags where a value is moved on only some paths;
- `try_from` proofs and `static for` unrolling have been expanded.

Lowering (`src/hir/lower.cpp`) reads the checker's results and never infers. It fails closed: an unsupported or not fully typed construct is a reported error, never a skipped function or placeholder.

## HIR passes

These run once, before either backend:

- **Explicit references** (`reference_check.cpp`). Every borrow and dereference becomes an explicit node. A leftover implicit reference is reported as a lowering gap rather than compiled.
- **Tail calls** (`tail_calls.cpp`). Marks direct calls in tail position ([Tail-Call Optimization](../03-advanced/39-tail-call-optimization.md)).
- **Captures** (`captures.cpp`). Computes what each lambda captures and how.
- **Generator state** (`live_across_yield.cpp`). Finds the locals live across a `yield`; these form the generator's state block.
- **Frame budget** (`frame_budget.cpp`). Rejects a function whose stack-allocated buffers exceed the per-frame limit.
- **Reachability** (`link.cpp`). Finds every module the entry function reaches. A reached module is included in full.
- **Inlining** (`inline.cpp`). Replaces calls to small functions with a copy of the body. This is what makes standard library collections such as `list[T]` cost about what a built-in would. `--no-inline` turns it off.

## Bytecode compiler

The bytecode compiler (`src/bytecode_compiler/`) turns each HIR function into a flat instruction stream with its own constant pool. Code is emitted into an unbounded virtual register space, then a linear-scan register allocator maps virtual registers onto a frame's physical registers, reusing one as soon as its value dies. Parameters occupy the first registers; there is no separate locals area.

## The VM

The VM (`src/bytecode/vm.cpp`) is a register machine.

- **Dispatch** is an ordinary `switch` over the opcode. Clarity takes priority over dispatch speed in this tier.
- **Frames** are an explicit vector, not C++ recursion, so Cinder's call-depth limit and its `stack_overflow` panic do not depend on the host stack.
- **Values** in registers are untagged slots; the compiler knows every type statically. Scalars live in the slot. Every other value lives in heap memory laid out by `src/runtime/layout.h`, the same layout the LLVM backend uses.
- **Instructions** cover arithmetic in checked, wrapping and saturating forms; comparisons and casts; jumps; direct, indirect and tail calls; closures; generators (`op_make_generator`, `op_yield`, `op_coroutine_resume`); stack and heap allocation; slot and indexed loads and stores; address-of; panics; atomics; calls into runtime intrinsics; and host calls.
- **Runtime support** comes from `src/runtime/`: the allocator, string operations, formatting, platform queries and I/O. The VM calls these directly; compiled code links against the same library.
- **Panics** use one closed set of reasons shared with the LLVM backend, so both tiers fail the same way on overflow, division by zero, a failed contract, and similar conditions.

## Hosted runs

A hosted run supplies limits to the VM: a step budget, whether effects are allowed, and a callback for `op_host_call`. Compile-time execution uses a hosted run with effects off ([Compile-Time Execution in the Compiler](65-compile-time-execution-in-the-compiler.md)). A program run from the command line is not hosted.

## Use from the command line

`cinder FILE` compiles the program and runs its entry function on the VM. The VM is the reference tier: the test suite treats its results as the baseline the LLVM tier is compared against.
