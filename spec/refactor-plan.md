# Compiler refactor plan

This plan restructures the compiler in `src/` to reduce redundancy and make it
easier to read, change, and navigate. The largest problem is the type checker,
which is a single class of about 30k lines in one file (roughly a quarter of
`src/`). The plan splits the checker into files, then into components with
narrow interfaces. It also splits the checker's output into clearer groups,
adds shared tables and traversal helpers for the AST, and replaces the AST
compile-time interpreter with HIR running on the bytecode VM. Every step
except the last keeps behavior the same and is checked by the existing test
suite. The file split for step 1 is in `spec/refactor-step-1-checker-split.md`.

## Current state

Most of `src/` is reasonably sized and split along phase boundaries. The
problems are concentrated in a few places.

### The checker is one class in one file

`semantic/check.cpp` holds one class, `checker`, with about 600 methods and
120 member fields. It covers type resolution, reasoning and proofs,
monomorphization, calls, method lookup, patterns, statements, declarations,
and impl coherence. About 25 banner comments are the only structure. Any edit
recompiles the whole file, no part can be tested or read on its own, and every
method can reach all of the shared state. The state is already grouped by
comments into session, file, functor, function, and cache blocks, but those
groups are not types.

### The checker's output is a large set of side tables

The checker hands its results to later stages as `checked_types`. This is a
struct with about 60 maps and sets keyed by AST node, read by about 20 files;
HIR lowering alone reads about 50 of them. The same header also
holds the type table and the program index, which are separate concerns.

### There is duplicated logic inside the checker

Several pieces of logic exist in more than one copy:

- Two near-identical routines build linear polynomials from expressions, one
  for proofs and one for the value solver.
- Operator classification (which operators are arithmetic, which trait an
  operator maps to, which binary operator a compound assignment means) is
  written out as separate `switch` statements in the checker and again in the
  LLVM backend.
- Method lookup is spread across about 40 functions with overlapping
  responsibilities, covering declared methods, builtin methods, extend
  methods, bound methods, UFCS, and lookup through `deref`.

### AST traversal is hand-written in every pass

HIR has a shared child walker. The AST does not. The checker, AST cloner,
hygiene pass, binding walker, ownership CFG builder, resolver, session
builder, and compile-time evaluator each write their own traversal, using
about 900 `dynamic_cast`s between them. Each walker can silently miss a node
kind when the AST grows.

### There are three executors

The VM and the LLVM backend run HIR. The compile-time evaluator interprets
the AST directly, during checking. It therefore implements operators,
strings, collections, pattern matching, and calls a third time, and its
behavior can drift from the run-time tiers. This is the largest redundancy in
the compiler. It was not a deliberate design choice.

### Minor clean-up

`src/parser/` contains stray `bazel-*` symlinks and a `MODULE.bazel`. They are
ignored by git but clutter the tree. `cli_test.cpp` is about 5k lines and can
be split by feature.

## Plan

The steps are ordered so that each one makes the next easier. Steps 1, 4, and
5 are independent of each other and can be done in any order. Step 6 is
easier after steps 1 through 3.

### Step 1: split the checker into files

The checker's class declaration moves into a private header, and its method
definitions move into about 16 files along the existing section lines. No
logic changes. This makes builds faster and navigation easier right away, and
it exposes which parts of the checker depend on which. The exact split is in
`spec/refactor-step-1-checker-split.md`.

### Step 2: turn the checker's state groups into types

The session, file, functor, function, and cache blocks become structs. Each
split file then shows in its code which state it uses. Saving and restoring
per-function state around a declaration becomes a single value copy instead of
many field copies.

### Step 3: extract components with narrow interfaces

Parts of the checker that have their own state and a small surface become
separate classes with their own tests. In order:

1. The reasoning and proof engine, including flow-sensitive narrowing and
   `try_from` proofs. This step also merges the two polynomial builders.
2. Method resolution. One entry point takes a receiver type and a method name
   and returns a single result describing what was found and how to call it.
   The current lookup functions become internal steps of that one query.
3. Monomorphization: const-generic instances, higher-kinded method instances,
   and instances by substitution.

Each extraction keeps the checker as the caller. The goal is that a reader
can understand one component without reading the checker.

### Step 4: split the checker's output

The checker's output header is split into three headers: the type table, the program index, and the
checker results. The checker results are then grouped by the stage that reads
them (lowering, ownership, drop scheduling, metadata), so each consumer
depends on a smaller surface.

### Step 5: share operator tables and an AST walker

A single operator table in `parser/` answers the classification questions now
answered by repeated `switch` statements. An AST `for_each_child` and `walk`,
modeled on `hir/traversal.h`, replaces the hand-written traversals one pass at
a time. Passes that dispatch on node kind use the kind tag and a checked cast
helper instead of chains of `dynamic_cast`.

Status: the operator table (`parser/operators.h`) and the AST walker
(`parser/ast_traversal.h`, with `for_each_child`, `walk`, kind-class
predicates, and the checked cast `as`) have landed. The operator table
replaced the duplicated spelling, overload-trait, comparison-negation,
compound-assignment, and range switches in the checker, HIR lowering, the
bytecode compiler, the LLVM backend, and the compile-time evaluator. The
hygiene pass, the session scope builder (the binder-free node kinds), the
resolver's qualified-path validation, and the pattern binding collector
(`semantic/binding_walk.cpp`) are moved onto the walker. The
ownership CFG builder and the AST cloner use the checked casts `as` and
`cast` in place of raw `dynamic_cast`s, and the CFG builder's pattern
leftover scan uses `for_each_child`. The cloner builds new nodes, so it keeps
its own per-kind construction. Every `dynamic_cast` to an AST node type in
`semantic/`, `comptime/` and the lowerer is now `ast::cast` or `ast::as`. The
compile-time evaluator's recursive helpers (`quote_body_has_nested_splice`,
`materialize_quote`, `clone_expr_fragment`) stay as they are: they cover only
the node kinds `materialize_quote` can rebuild, so a full walk would change
which quotes are accepted, and step 6 removes the evaluator. The checker has
no recursive read-only traversal of its own that was found to need the walker.

### Step 6: run compile-time code as HIR on the VM

Compile-time code is checked, lowered to HIR, and run on the bytecode VM, the
same as run-time code. The AST interpreter is removed. After this step the
language's semantics are implemented in one lowering and two backends, rather
than in a lowering, two backends, and an interpreter.

This step changes behavior and has real design work. The main issues are:

- **Ordering.** Compile-time code runs in the middle of checking. A `static
  if` decides which branch is checked, and a compile-time call needs its
  callee checked and lowered first. Lowering currently runs once, after the
  whole program is checked. It needs to lower a single checked function on
  demand.
- **Compile-time-only values.** Compile-time code works with values that
  have no run-time form: types, quoted syntax and splices, modules, and
  reflection records such as `field_info`. The VM needs a representation for
  these, most likely as opaque handles backed by host calls into the checker.
- **Budgets and diagnostics.** The interpreter enforces an iteration budget
  and reports errors at the compile-time call site with notes in the callee's
  file. The VM needs a step budget and needs to map a failure back to source
  spans.

The work proceeds in phases, each of which leaves the compiler working:

1. Lower one checked function to HIR on demand, independent of whole-program
   lowering.
2. Add a step budget and source-span error reporting to the VM.
3. Route compile-time calls that use only ordinary values (integers, strings,
   collections, structs, variants) to the VM. The AST interpreter remains for
   everything else.
4. Represent types, quoted syntax, modules, and reflection records as VM
   values backed by host calls, and route the remaining cases to the VM.
5. Delete the AST interpreter.

During phases 3 and 4, both paths exist. Running the compile-time test
corpus through both and comparing results catches differences before the
interpreter is deleted. The comparison is only a check on the move from one
path to the other, since a bug shared by both paths will not show up in it.
Tests that check a computed value remain the primary check.

Status: phases 1 through 3 have landed. Phase 4 is partly done; phase 5 has
not started.

- Phase 1. The checker keeps a lowering view (`semantic/check_lowering_view.
  cpp`): a `checked_types` holding just the functions compile-time code
  runs, copied out of the session tables with types settled and
  refinements erased, sharing the session's type table. Preparing a
  function checks its item out of turn when file order has not reached it,
  makes the generic instances it names, and settles its own literal leaves
  and list literals. `//src/comptime_vm` lowers a function and everything
  it reaches, compiles them into one bytecode module, and converts values
  between the evaluator and VM memory.
- Phase 2. `vm::run_hosted` runs with a step budget, refuses intrinsics
  that reach outside the VM, and returns every panic (including the fatal
  ones and `panic(...)`) with the active frames. Bytecode functions carry a
  source map, so a failure is reported at the compile-time call with notes
  at each line it passed through.
- Phase 3. The evaluator runs every call it can on the VM, generic calls
  included (as their instances), and interprets the rest. The driver
  selects this with `CINDER_COMPTIME_VM` (`on`, the default; `off`; or
  `compare`, which runs both and reports any difference).
  `CINDER_COMPTIME_VERIFY`, on in the driver tests, re-lowers every
  function the VM ran from the final checker results and reports any that
  lower differently. `CINDER_COMPTIME_TRACE` lists each call and
  expression, and why it was not run.
- Phase 4, done so far:
  - Whole expressions run on the VM, not just calls: a `static assert` or
    `static if` condition, a `static let` initializer, a `static for`
    iterable, guard or yield, a splice operand. Each is lowered as a
    function of its own (`hir::lower_comptime_expression`) whose
    parameters are the compile-time bindings it reads (a `static for`
    loop variable). A `static` read before file order reaches it is
    checked first, so its initializer has records to lower.
  - Reflection has real types and is folded to constants on a concrete
    type: `T.fields()` is a `list[field_info]`, `T.variants()` a
    `list[variant_info]`, `M.functions()` a `list[member_info]` (structs in
    `std.traits`), `T.kind()` a `type_kind`, the counts `int32`. A list of
    field descriptors is now an ordinary run-time value. `field_info` lost
    its `type_of` field, which nothing used.
  - Quoted syntax is a VM value: a handle into a table each run keeps.
    Quotes, the `expr.*` builders and reads of a static holding syntax
    lower to `hir_host_call` / `op_host_call`, which the compiler performs
    (`comptime::fragment_builder`, shared with the interpreter so both
    build the same syntax). The checker now checks the splices a quote
    resolves as it is evaluated. Run-time code and the LLVM backend refuse
    host calls.
  - A block `static for` in a function body is unrolled: the checker
    records each iteration's loop-variable values (per instance in a
    template) and lowering emits the body once per iteration. This also
    makes it usable in run-time code, which it was not before. An inline
    `static for` in a compile-time-only function is unrolled the same way.
- Phase 4, not done:
  - `deriving` and item-level splices run in a pre-pass that must finish
    before the method table is built, because their results add `impl`
    blocks. The VM needs checked code, and checking needs the method table,
    so these stay interpreted. The likely fix is a method table built in
    two stages: every written `impl` first, then the spliced ones.
  - Every immutable `let` in ordinary code is probed with the interpreter
    to see whether it is a compile-time constant, so a later `static if`
    can read it. That should become a lazy evaluation of the `let`'s
    initializer when a compile-time expression reads it.
  - Module-level block `static for` still runs its body in the
    interpreter.
  - A compile-time function whose `static for` iterable reads its own
    parameters cannot be unrolled while checking and stays interpreted.
- Phase 5 (deleting the interpreter) depends on the items above.

Phase 3 also fixed a tail-call bug it exposed: a call whose argument
borrows a temporary (`&TABLE[0..n]`) was compiled as a tail call, which
cleared the caller's registers the borrow pointed into.

## Risks

Steps 1 through 5 do not change behavior. The main risk is a mechanical
mistake during a move, such as a lost default argument or a changed overload.
The full test suite, the diagnostic snapshot tests, and a warning-free build
under `-Werror` catch most of these. Doing each step as a series of small
commits that each build and pass keeps any mistake easy to find.

Step 2 changes how per-function state is saved and restored. A missed field
would leak state from one declaration into the next. This shows up as
order-dependent test failures, which the ASan test recipe also helps catch.

Step 6 changes when lowering runs and adds a second way to execute
compile-time code. Compile-time code that depends on a quirk of the AST
interpreter may behave differently on the VM. Running both paths side by side
in phases 3 and 4 is the way to find these cases.

Every step touches code that other work is changing. Doing step 1 in a quiet
period, and landing it quickly, keeps merge conflicts small.
