# Execution Pipeline Design: Bytecode Interpreter + LLVM JIT/AOT

Status: draft, not yet implemented. This document picks the architecture
for `spec/llm-compiler-roadmap.md` phase 5 ("LLVM code generation") and
extends its scope to cover two requirements the roadmap didn't originally
capture as design constraints: Kira needs to feel as fast to start as a
scripting language, not just eventually produce fast binaries; and Kira
needs to produce fully standalone AOT executables — no interpreter,
runtime, or package manager the end user has to separately install —
specifically to avoid the distribution problems that come with shipping
Python (or any language whose deployment story assumes the target
machine already has a matching interpreter/runtime installed). Both
requirements shape the same architecture, so they're designed together
here rather than in separate documents. Written before any codegen code
exists, so implementation doesn't have to re-derive these calls mid-PR.

## Why "just emit LLVM IR" isn't the whole answer

The roadmap's phase 5 exit criterion is "small end-to-end programs
compile and run through LLVM," which undersells a real constraint: LLVM
is an optimizing compiler backend, not a low-latency one. Even at `-O0`,
compiling a single function through LLVM's IR verifier, instruction
selection, and register allocation runs hundreds of microseconds to low
milliseconds. That's an acceptable one-time cost for a function that ends
up running a million times; it's the wrong latency class for "run this
15-line script and print the result," which is a use case
`spec/llm-compiler-roadmap.md`'s Non-Negotiable Goals explicitly commits
to ("fast enough that `.kira` can replace small Python scripts in local
workflows").

Every production system that has solved "scripting-fast to start, fast
once hot" keeps LLVM-class codegen out of the first tier:

- V8 runs a hand-written bytecode interpreter (Ignition) first, promotes
  to a fast single-pass baseline JIT (Sparkplug), and only reaches an
  optimizing JIT (TurboFan — not LLVM) for code that's actually hot.
- Wasmtime/Wasmer use Cranelift, a purpose-built low-latency backend, for
  their baseline tier specifically because LLVM is too slow to
  compile-on-first-call.
- Julia is the cautionary counterexample: it uses LLVM as its only
  backend with no interpreter tier, and its widely-documented
  "time-to-first-plot" latency is directly attributable to LLVM
  compilation on cold code.

So this document scopes two things together, because they share a value
representation and can't be designed independently: a dependency-free
bytecode interpreter for immediate execution, and an LLVM-backed JIT that
promotes hot functions — the same LLVM path also used for AOT
(`kira build`) object emission.

## Decision 1: Two-tier execution, LLVM shared between JIT tier-up and AOT

```
 kira run file.kira                       kira build file.kira
        │                                          │
        ▼                                          │
 ┌─────────────────┐                                │
 │ bytecode compile │ (HIR → flat bytecode)          │
 └────────┬─────────┘                                │
          ▼                                          ▼
 ┌─────────────────┐   call-count threshold   ┌──────────────────┐
 │ tier-0: bytecode │ ───────────────────────▶ │ HIR → LLVM IR    │
 │ interpreter (VM) │                          │ (llvm_codegen)   │
 └────────┬─────────┘                          └─────────┬────────┘
          │  swap function slot to native ptr             │
          │◀───────────────────────────────────────────── ┤ orc::LLJIT
          ▼                                                │
 (subsequent calls run                                     ▼
  JIT-compiled native code)                        TargetMachine::
                                                    addPassesToEmitFile
                                                    (object file)
```

Tier 0 is a hand-rolled bytecode VM (its own component, no LLVM
dependency at all) compiled directly from HIR — not a tree-walking
interpreter over HIR nodes, since dispatch through HIR's node-kind
hierarchy on every evaluation is materially slower than dispatching flat
bytecode. Tier 1 is `llvm::orc::LLJIT`, fed by the same `llvm_codegen:
hir_module → llvm::Module` lowering that AOT compilation uses to emit
object files via `TargetMachine`. This is the "one dependency for AOT and
interpreting" property: LLVM is linked once and used for both, while the
tier that actually needs to be fast (cold-start scripting) never touches
it.

## Decision 2: LLVM C++ API, LLVM located via `llvm-config`, not vendored

Uses `llvm::IRBuilder`, `llvm::Module`, `llvm::orc::LLJIT`, and
`llvm::TargetMachine` directly — the full C++ API, not the C bindings.
This is a deliberate departure from this project's usual "hand-roll it,
minimize dependencies" style (see the lexer, parser, and
`src/testing/test_assert.h`), justified because ORC JIT's ergonomic
surface (lazy compilation, `ThreadSafeModule`, materialization units) is
substantially more usable in C++ than through the C bindings, and because
this is a real, unavoidable heavy dependency either way — there's no
hand-rolled alternative to an entire optimizing backend.

The cost this incurs: LLVM's C++ ABI is not stable across releases, so
Kira pins to one LLVM major version (matching the Clang 22.1+ toolchain
version already required to build Kira itself, per `CLAUDE.md`) rather
than floating. There is no Bazel Central Registry module for the LLVM
C++ libraries (llvm-project is not published there as a consumable
`bazel_dep`), so integration goes through a repository rule that shells
out to `llvm-config --cxxflags --ldflags --libs` on the host to locate
headers/libraries already installed by the pinned toolchain, rather than
vendoring or building llvm-project from source under Bazel (which would
dwarf this project's build times). This mirrors how other Bazel projects
that need a system LLVM typically integrate it. First implementation
increment should confirm `llvm-config` is present and produces sane
output before writing any codegen code — that's a prerequisite spike, not
an assumption to bake in silently.

## Decision 3: Shared runtime value representation between both tiers

Whatever in-memory layout a value has while running interpreted must be
byte-for-byte identical to its layout once JIT-compiled, or tier-up
requires marshaling data at the boundary — which defeats the point of a
"swap the function pointer" tier-up (Decision 4) and would make every
promoted function's first call pay a conversion cost indefinitely. So the
runtime ABI (struct layouts for `list[T]`, `str`, sum-type payloads,
closures) is designed once, in a shared header the bytecode VM and
`llvm_codegen` both target, not designed twice and reconciled later.

`spec/llm-compiler-roadmap.md`'s "What Not To Assume Yet" is explicit
that there's no settled ownership/borrow-checking model yet. This design
does not solve that — it picks an explicit placeholder so codegen has
*something* concrete to target:

- Heap values (`list[T]`, `str`, boxed sum-type payloads) are allocated
  from a bump/arena allocator. Nothing is freed. This is intentionally
  the simplest possible thing that could work, not a real memory
  strategy — it's a placeholder until ownership/borrowing lands and
  determines the real deallocation story (refcounting, ownership-based
  drop, or a tracing GC). Every place this placeholder shows up in code
  should say so in a comment, the same way `k_unknown_type` says so.
- Sum types are a tag (`i32`) plus the union of variant payload sizes,
  matching what `src/semantic/check.cpp` and `src/hir` already assume
  about variant shape — codegen doesn't get to invent a different layout
  than what earlier phases already committed to.
- Scalars (`bool`, `int32`, `usize`, `char`, ...) map directly to their
  obvious LLVM/native types; no boxing.

## Decision 4: Function-boundary tier-up only, synchronous compile, no OSR

A function starts executing in the bytecode interpreter. Each call
increments a per-function counter; crossing a fixed threshold triggers
LLVM codegen + `LLJIT` compilation for that function, and its dispatch
slot (a function pointer the interpreter calls through, not a re-lookup
each time) is swapped to the compiled native entry point. Two
simplifications are deliberate for this first design, both narrowing
scope rather than closing off better versions later:

- **No on-stack replacement.** A long-running loop inside a single
  invocation that crosses the hotness threshold mid-execution finishes
  that call in the interpreter; only the *next* call uses the compiled
  version. Real OSR (swapping tiers mid-frame) is a substantial project
  on its own and isn't needed for "a scripting workload eventually gets
  compiled" — it's needed for "one very long-running call gets fast
  without returning," which is a narrower, later concern.
- **Compilation runs synchronously on the call that crosses the
  threshold**, momentarily pausing that call while LLVM compiles.
  Background compilation (continue interpreting while a worker thread
  compiles, swap the pointer whenever it finishes) is the better latency
  profile and the eventual target, but it requires a concurrency story
  this codebase doesn't have yet — `spec/llm-compiler-roadmap.md`'s "What
  Not To Assume Yet" already flags there's no settled concurrency
  implementation. Ship synchronous first; move to background compilation
  once Kira's own concurrency primitives (or plain `std::thread` as an
  implementation detail, not a language feature) are appropriate to lean
  on.

The threshold itself (call count, not loop-iteration count, for this
first cut — loop-iteration-based triggers need OSR to pay off, which is
explicitly out of scope above) is a tunable constant, not a design
commitment; expect to revisit the number empirically once both tiers
exist and can be benchmarked against each other.

## Decision 5: AOT output is a fully standalone binary — no LLVM, no VM, no shared runtime to install

The AOT path (`kira build`) is not "phase 5's exit criterion, satisfied
eventually" — it's a first-class deliverable this design is built around,
because avoiding Python-style distribution (users needing a matching
interpreter/runtime already installed, or a version-pinned virtualenv, or
a package manager, just to run a program someone handed them) is a stated
goal, not a nice-to-have. Concretely, this constrains what's allowed to
end up in a `kira build` output binary:

- **No libLLVM at runtime.** `llvm::orc::LLJIT` and everything it pulls
  in (the JIT's own runtime support, symbol resolution machinery) is a
  `kira run`-only dependency. `TargetMachine::addPassesToEmitFile`
  produces a plain object file with no reference to LLVM's own runtime —
  linking that object file must not require `libLLVM*.so`/`.dylib` to be
  present on the machine that eventually runs the binary, only a
  standard system linker and libc at build time.
- **No bytecode VM in the output either.** Tiering (Decisions 1 and 4) is
  purely a `kira run` concern for interactive/scripting use. A `kira
  build` binary is 100% code produced by `llvm_codegen`, statically
  linked with Kira's own small native runtime support library (arena
  allocator, list/string helpers, panic/print) — there is no bytecode
  interpreter fallback baked into shipped executables, and no reason for
  one, since AOT compilation isn't gated behind a hotness threshold.
- **Kira's own runtime support library links statically**, not as a
  shared object the deployed binary depends on finding at load time. The
  arena-allocator placeholder from Decision 3 is not a problem for this
  goal specifically *because* AOT binaries are ordinary short-to-medium-
  lived processes where "never free, reclaim on process exit" is often
  fine outright — this placeholder becomes a real liability only for
  long-running services, which is a separate concern from "can I hand
  someone a binary and have it run."
- **The bar is higher than "statically link libc" — see Decision 6.**
  Kira's ambition is closer to Go's: not just a self-contained binary, but
  one that doesn't route its runtime through libc at all where the
  platform allows it, for the same reason Go doesn't — libc is a moving,
  version-sensitive compatibility surface (glibc symbol versioning is
  exactly the kind of "works on my machine" distribution failure this
  goal exists to avoid), and a runtime built directly against stable
  kernel syscalls sidesteps it entirely rather than merely bundling it.

This decision doesn't change Decisions 1-4's tiering architecture; it
just makes explicit that tiering exists for `kira run`'s benefit only,
and `kira build`'s output must never accidentally acquire a runtime
dependency on any piece of that tiering machinery.

## Decision 6: own runtime substrate over raw syscalls by default; C-ABI interop as an explicit, separate opt-in

The goal is closer to Go than to "a statically-linked C program": by
default, Kira's runtime support library (memory allocation, threading if/
when Kira has language-level concurrency, I/O) should talk directly to
the OS kernel's syscall interface, the way Go's runtime does, rather than
going through libc's implementations of `malloc`, `pthread_*`, `read`/
`write`, etc. This is a stronger claim than Decision 5's "statically
linked" — a binary can statically link libc and still inherit its
behavior (allocator internals, NSS-based name resolution quirks, thread
model assumptions); bypassing libc for Kira's own runtime avoids
inheriting *any* of that, and lets the runtime's memory/scheduling model
be designed to fit Kira's own semantics rather than adapted to fit libc's.

This has to be scoped honestly rather than promised uniformly across
platforms:

- **Linux** has a genuinely stable raw syscall ABI (the kernel commits to
  not breaking it), which is exactly what makes Go's `CGO_ENABLED=0`
  binaries able to skip libc entirely there. Kira's own runtime
  (allocator built on raw `mmap`/`munmap`, etc.) can target this directly.
- **macOS is not the same situation**, and this document should not
  pretend otherwise: Apple does not guarantee a stable raw syscall
  interface and requires going through `libSystem.dylib` — which is
  exactly why Go itself still links `libSystem` on Darwin even in
  "pure Go" builds. Kira's Darwin runtime will have the same constraint;
  "no system libraries" is a Linux-specific ambition within this
  decision, not a cross-platform one, and this document is explicit about
  that rather than deferring the surprise to whoever implements it.
- **Windows** similarly has no stable raw syscall ABI for user programs
  and expects calls through its own system DLLs; treat it the same way
  as macOS above — a per-platform runtime shim is expected, not a defect.

**C-ABI interop is a separate, explicit opt-in, not something this goal
is allowed to design away.** Kira needs to call into (and be called from)
existing C-ABI libraries — that's a hard requirement independent of the
"avoid libc" ambition above, and the two must not be allowed to conflict:

- Kira's calling-convention lowering in `llvm_codegen` must be able to
  emit and consume plain C-ABI-compatible function signatures and struct
  layouts (matching the platform's C calling convention exactly), for any
  function declared as an external C interface — this is a codegen-level
  requirement, not an afterthought bolted on later.
- Declaring and linking against a real C library (statically or
  dynamically) must remain fully supported, even though doing so may
  reintroduce a libc dependency the default build otherwise avoids —
  mirroring how Go's `cgo` is available and fully supported, but is an
  opt-in that trades away some of `CGO_ENABLED=0`'s self-containment
  guarantees only for the specific binary that uses it. A `kira build`
  that never declares a C-ABI import keeps Decision 5's full
  self-containment; one that does is making an explicit, local trade,
  not silently losing a guarantee.
- Concretely, this means the runtime substrate work in this decision and
  the C-ABI interop story are two separate, independently-scoped pieces
  of work, not one — the runtime substrate is an increment 7+ concern (it
  doesn't block the scalar/control-flow subset in increments 1-6, which
  can run against a temporary libc-backed placeholder allocator/IO in the
  meantime), while C-ABI calling-convention support in `llvm_codegen`
  should be designed for from increment 3 onward so it's never a
  late-breaking architectural change to function lowering.

## Implementation scope, sliced into increments

Each increment should land with its own tests before the next starts,
matching how `src/hir`'s lowering was built shape-by-shape:

1. **Bytecode design + compiler (HIR → bytecode)** for the non-generic,
   non-concurrent, scalar/control-flow subset HIR already lowers today
   (arithmetic, comparisons, `if`, `while`/`while let`, `for` in all its
   currently-lowered shapes, function calls). No heap types yet.
2. **Bytecode VM** (tier 0 interpreter) executing that bytecode. This is
   the piece that should be benchmarked early against a naive
   HIR-tree-walking interpreter to confirm the dispatch-speed assumption
   in Decision 1 before committing further design around it.
3. **`llvm_codegen`: HIR → `llvm::Module`** for the same scalar/control-
   flow subset, with no JIT or tier-up wiring yet — just prove the
   lowering is correct by emitting `.ll` text and comparing against
   `llc`/`opt` output by hand, the same way `hir::lower` was validated
   against hand-written expected-shape assertions before anything
   consumed it downstream.
4. **AOT path**: `TargetMachine` object emission from step 3's IR, wired
   into a `kira build` driver mode, statically linking Kira's native
   runtime support library (no bytecode VM, no libLLVM — see Decision 5)
   to produce a real standalone executable for the scalar/control-flow
   subset. This is the roadmap's phase-5 exit criterion, satisfied for a
   narrow subset first, and should be checked (e.g. `ldd`/`otool -L` on
   the output) to confirm no LLVM or Kira-runtime shared-library
   dependency leaked in. The runtime support library backing this
   increment is allowed to be libc-backed as a placeholder (`malloc`,
   `mmap` via libc wrappers) — swapping it for Decision 6's direct-syscall
   substrate is deliberately deferred to increment 8, not bundled in here.
5. **Tier-up wiring**: call counters, dispatch-slot swap, `LLJIT`
   invocation from the interpreter's call path, using step 3's IR
   construction. This is where Decision 4's synchronous/no-OSR
   simplifications actually get implemented and tested.
6. **Heap types** (`list[T]`, `str`, sum-type payloads) added to both the
   bytecode VM and `llvm_codegen` together, per Decision 3 — never add a
   heap type to one tier without the other, since that's exactly the
   representation-mismatch Decision 3 exists to prevent.
7. **C-ABI interop**: `extern "C"` declarations lowered to real C-ABI
   calling convention in `llvm_codegen` (Decision 6), and a driver story
   for linking a named C library into a `kira build` output — designed
   alongside step 3 rather than bolted on later, since retrofitting
   calling-convention support onto an existing function-lowering path is
   exactly the kind of late architectural change this document exists to
   avoid.
8. **Own runtime substrate**: replace increment 4's libc-backed
   allocator/IO placeholder with Kira's own direct-syscall implementation
   per Decision 6, Linux first (stable syscall ABI), with macOS/Windows
   shims following since neither offers a stable raw syscall interface —
   this is deliberately its own increment, decoupled from step 4's AOT
   milestone, so "produces a standalone binary" and "doesn't route
   through libc" can be validated as two separate claims rather than one
   conflated one.
9. Generics/monomorphization, real memory management (replacing the
   arena/leak placeholder), background compilation, and concurrency
   codegen are explicitly out of scope for this document and stay
   follow-on roadmap items — consistent with how `spec/typed-ir-design.md`
   deferred monomorphization to "phase 5, where it's needed anyway,"
   which this document is part of but does not fully discharge.

## Explicitly deferred / open questions

- Exact bytecode instruction set (stack-based vs. register-based) is not
  decided here — that's a detail for increment 1, not an architectural
  fork worth blocking this document on.
- Real memory management strategy (refcounting vs. ownership-based drop
  vs. tracing GC) is blocked on Kira's ownership/borrow model, per
  `spec/llm-compiler-roadmap.md`'s existing "What Not To Assume Yet."
- Background (async) tier-up compilation, on-stack replacement, and
  loop-iteration-based hotness triggers are named above as deliberately
  deferred, not rejected.
- Debug info (DWARF via LLVM's `DIBuilder`) is untouched by this
  document; add it if/when source-level debugging of AOT binaries
  becomes a stated goal.
- Full static linking against libc (e.g. musl on Linux, for a truly
  zero-dependency binary rather than one that still expects glibc at a
  compatible version) is a packaging goal named in Decision 5 but not
  designed here — it's per-platform work, tackled once increment 4
  produces a working dynamically-linked-against-libc baseline to compare
  against.
