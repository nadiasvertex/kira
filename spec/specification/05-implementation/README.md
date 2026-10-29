# Implementation

A sketch of how the reference compiler, `cinder`, is built: its layers, what each one does, and how they hand work to each other. See the [full table of contents](../00-overview.md#table-of-contents) for the whole specification.

This section is informative, not normative. The language is defined by sections 1–4; an implementation may use any architecture that produces the behavior they specify. Nothing here is a requirement on another compiler. These chapters describe how the reference compiler actually works today, so that a reader can find their way around `src/` and understand why the compiler behaves as it does. Where a chapter names a file or component, the name is current as of this writing and may drift; the source is authoritative.

62. [Compiler Structure](62-compiler-structure.md)
63. [Type Checking and Inference](63-type-checking-and-inference.md)
64. [Proof Solving](64-proof-solving.md)
65. [Compile-Time Execution in the Compiler](65-compile-time-execution-in-the-compiler.md)
66. [HIR and the Bytecode VM](66-hir-and-bytecode-vm.md)
67. [LLVM and Ahead-of-Time Compilation](67-llvm-and-aot.md)
68. [JIT Execution](68-jit-execution.md) — Partial
69. [The Task Scheduler](69-task-scheduler.md) — Partial

Previous: [Standard Library](../04-stdlib/README.md)
