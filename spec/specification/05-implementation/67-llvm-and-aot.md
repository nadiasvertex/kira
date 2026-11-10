# 67. LLVM and Ahead-of-Time Compilation

**Status:** Implemented

How the reference compiler produces native code: HIR is lowered to LLVM IR, optimized, emitted as an object file, and linked with Cinder's runtime support library into a standalone executable.

## Code generation

`src/llvm_codegen/codegen.cpp` lowers the same HIR the bytecode compiler reads into an `llvm::Module`. It reads types from the checker's type table and never re-infers.

- **Values.** Scalars are LLVM scalars; `int128`/`uint128` are `i128`. Every other value is allocated through one runtime entry point and laid out by `src/runtime/layout.h`, the same rules the VM uses. Closure environments and generator state blocks use 16-byte entries, matching the VM's register images. The heap allocator guarantees only 8-byte alignment, so every `i128` load and store is emitted with alignment 8 at most.
- **Calls.** Direct calls name the instance's symbol. Closures and function values use indirect calls. A call that the HIR pass marked as a tail call becomes `musttail` when LLVM's preconditions provably hold and an ordinary call otherwise ([Tail-Call Optimization](../03-advanced/39-tail-call-optimization.md)).
- **Generators.** A `generator def` compiles to a small constructor that builds the generator's state block, plus a step function that resumes from the last `yield`. The state block holds the locals that the HIR pass found live across a `yield`. Both backends share one coroutine object of seven slots: step function, state pointer, resume index, status, the scheduler's record, and the last yielded value, which takes two slots so a 128-bit item fits (`src/runtime/coroutine.h`). The step function returns the new status (suspended, done or pending) and writes a yielded item to the value slot; `.next()` call sites read the status and build `some(value)` or `none` themselves.
- **Tasks.** An `async def` compiles like a generator that yields its result once. Inside a task, `await` is a suspension point: the code calls `cinder_rt_task_await_poll`, and if the awaited task has not finished it stores the resume index and returns `pending`. The step function resumes at the same `await` once the scheduler wakes it. In an ordinary function (the entry `async def main`) `await` calls `cinder_rt_task_block_on` and blocks the calling thread. Dropping a task asks the scheduler to cancel it first (`cinder_rt_task_cancel`), because a task the scheduler owns may be running on another thread. Locals in a step function are normally copied between the state block and allocas at each suspension. A local whose address is taken, or that a closure captures by reference, lives in its state-block entry instead, so a pointer to it stays valid across a suspension (`hir::coroutine_borrowed_locals` lists them for both backends). A borrowed temporary (`await f(&xs[a..b])`) is one of these locals, because `make_references_explicit` stores every borrowed value that is not a place in a local first. A `match`, `while let` or `let ... else` subject that is suspended state is stored into its state slot. See [The Task Scheduler](69-task-scheduler.md).
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
4. Link the object with the system C++ driver (with `-pthread`, for the scheduler's worker threads) against Cinder's runtime support archives: panic handling, the heap allocator, string operations, and the libraries these depend on.

The resulting executable has no dependency on LLVM.

## Storage-form code generation

`storage_codegen.{h,cpp}` builds LLVM from the HIR storage form (chapter 62) instead of from typed HIR. Every slot is an entry-block byte `alloca` of its layout size and alignment, so aggregates are inline bytes and no heap allocation is emitted; SROA turns slots nothing takes the address of back into registers. Scalars load and store at their own width, and aggregates move with `memmove`. `p[i]` on a raw pointer is a byte `getelementptr` of the index times the element size. A function's signature starts with the hidden result address when the result is an aggregate or descriptor, followed by each non-zero-sized parameter; aggregate parameters are passed by address. An access claims the alignment its slot base and offset guarantee, packed projections are accessed unaligned, and anything reached through a loaded address uses alignment 1. Checked arithmetic, casts, literals, and panics come from `scalar_ops.{h,cpp}`, shared with `codegen.cpp`. With `storage_codegen_options::entry_thunks`, each function also gets `__cinder_entry_<name>(const uint64_t *args, void *result)` so the JIT test helper (`jit_module::run_storage`) needs no per-signature call shape. A call to a runtime intrinsic is checked with `hir::check_intrinsic_signature` and declared on first use: an intrinsic that returns an aggregate or `str` calls `cinder_<name>_into(dest, args...)` and returns `void`, and any other calls `cinder_<name>` with the wire kinds in `src/intrinsics.h`, converting scalars to and from those widths. A `fn` value is three words `{entry, env, glue}`: a function address, the environment block (null when nothing is captured), and a drop-glue address (null only when the environment is empty, since any block needs the glue to free it). A closure body or glue takes the environment as a leading `ptr` parameter. `make_closure` allocates the block with `cinder_rt_alloc`, stores each capture at its offset, and stores the three words; a call through a `fn` value loads the entry and environment and calls through the pointer with the environment first; `drop_in_place` of a `fn` value branches on a non-null glue and calls it; `release_environment` calls `cinder_rt_free` with the block and its size. Closure bodies and glue get no `__cinder_entry_` thunk, because they are reached only through a `fn` value. The driver does not use this compiler yet, and it refuses by name calls into another module, `drop_in_place` of anything but a `fn` value, and 128-bit scalars.

## Relationship to the VM

The VM and the LLVM backend are separate implementations of the same HIR semantics. The codegen stress test compiles each program in `src/testdata/codegen_stress/` with both and requires them to agree. Because the two backends share layout rules, they can share a mistake and still agree, so a corpus file may also state its expected result with `# expect:`, which is checked independently of agreement. A file may also bound how many heap blocks `main` allocates on each tier with `# max-allocations:`, which catches storage that should be reused but is allocated afresh, such as a borrow taken in a loop. `# max-live-blocks:` bounds how many heap blocks `main` leaves allocated, counted on every thread once `main` and the tasks it started have finished, which catches storage that is never freed.

## See also

- [JIT Execution](68-jit-execution.md) — running LLVM output in-process.
- [The `machine` Layer](../03-advanced/38-machine-layer.md) — the low-level surface that maps most directly onto generated code.
