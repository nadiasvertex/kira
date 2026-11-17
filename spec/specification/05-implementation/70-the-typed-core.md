# 70. The Typed Core

**Status:** Partial

A second representation of a checked program, the typed core, is built and verified when a check is switched on, and nothing else reads it yet. HIR lowering, the ownership check, and both backends still read the AST and the checker's side tables. The core is a program in which nothing is implicit. A verifier re-checks it without inference, and a temporary adapter builds it from the checker's output. The tests run the whole pipeline over the standard library and the test corpus. For current coverage and the remaining phases, see `spec/unified-checker-plan.md` and the output of `//src:core_corpus_test`.

## Purpose

The core sits between the checker and HIR lowering and has one job: to say everything the checker decided, so that a later stage can read it without inferring anything again. Today those decisions are spread over side tables keyed by AST node (see [Type Checking and Inference](63-type-checking-and-inference.md)), and HIR lowering combines them with the AST. The core puts them in one tree (`src/core/nodes.h`):

- Every expression has a settled type.
- A place is a root (a local, a `static`, or a temporary) plus a list of projections: field, tuple element, variant payload, dereference, and builtin element. Indexing or dereferencing a user type is a call to its impl, followed by a dereference of the returned reference.
- A place is read, written, or borrowed only through `use` (a copy or a move), `borrow` (shared or mutable, written or inserted), and `assign`. There is no implicit borrow or dereference.
- There is one `call` form. Its callee names a declared function or an instance of one, a function or closure value, or an intrinsic. Operators on user types, method calls, conversions, default arguments, and `for` loops over iterators are calls. Builtin scalar operators are `primitive` nodes.
- There is one `match` form: a subject place and arms, where each arm has tests (a structural description of the value) and bindings (each a projection of the subject, as a copy, a move, or an alias). `if let`, `let ... else`, and destructuring `let` elaborate to it.
- There is one loop, `while_loop`. A `for` loop is a `let` of its state followed by that loop.
- A function keeps its type and value parameters. An instance is a function whose `instance_of` points at its template.

The core does not say where values are dropped and does not fix a layout. The ownership check and HIR lowering decide those.

## The verifier

The verifier (`src/core/verify.h`, `verify.cpp`) is a type checker for the core with no inference. It trusts no decision made upstream: it recomputes each fact from the core, the type table, the member types in `checked_types::shapes`, and `checked_types::is_copy`. Type equality is equality of `type_id`.

Every rule has an id, listed with its statement in `verify.h`, and has a passing and a failing case in `src/core/verify_test.cpp`. The groups are:

- **T**, types: every type is settled, free of refinements and existentials, and mentions a type parameter only inside a function that lists it.
- **P**, places: roots are in scope, each projection is valid for the type before it, and writability follows from the root and path alone.
- **A**, access: a `use` is a copy exactly when its type is `copy`, a move leaves no reference or element behind, and a mutable borrow is of a writable place.
- **C**, calls: one argument per parameter, each of the parameter's type; the callee form is consistent with its declaration, value, or intrinsic name; generic arguments are settled and, for a callee whose template is in the program, as many as the template has parameters.
- **E**, expressions: literals, primitives, casts, aggregates, blocks, `if`, `match`, lambdas, and `await` have the types their parts imply.
- **S**, statements: `let`, assignment, loops, jumps, and contract checks.
- **F**, functions: each symbol is introduced once, the body has the function's type, and a static's value has the static's type.

A violation is an internal compiler error, never a user diagnostic. The verifier does not yet check that a generic call's arguments meet the callee's bounds, and it does not check that a `match` is exhaustive.

The compiler's own operations are the `intrinsic` callees whose names the elaborator chooses (builtin addressing, views, cells, raw pointers, layout queries, string iteration, task forms). `src/core/intrinsics.h` holds the table: each name has a fixed shape for its signature and generic arguments, and rule C5 checks a call against it. An intrinsic name outside the table must be declared by an `intrinsic def` in the program.

## The adapter

The adapter (`src/core/adapter.h` and the `adapter_*.cpp` files, split by construct: expressions, statements, patterns, loops, lambdas, contracts, interpolation, static items, tasks) builds the core from the AST and the checker's side tables while the checker still writes them. It decides nothing. Each type, callee, conversion, and dispatch comes from a record in `checked_types`. It interns only the reference and function types that the core makes explicit.

When a record is missing, or a construct has no core form yet, the adapter reports a `gap` that names the construct and the function, and leaves that function out. The verifier checks only the functions that were built. The adapter is temporary: the plan replaces it, and the side tables it reads, with a checker that emits the core directly.

Each node built from the AST keeps its `origin`, the key the drop schedule and the ownership check name it by, which is valid only while the AST is alive. For most nodes it is the AST node. For a block it is the address of the AST statement list the block was built from, which is how the ownership check names a scope. When one AST node becomes several core nodes, the one that plays its role for drops carries it: the jump of a `return`, `break` or `continue`, the `assign` of an assignment, the `let` of a `let` or `var`. Each core statement after the first that an AST statement becomes is marked `continues_statement`, so its temporaries end with that statement's.

## Running and testing

The core is not part of a normal compilation. It is built and verified when `cli_config::core_check` is set, or when the environment variable `CINDER_CORE_VERIFY` is defined, after the ownership check and only if checking reported no errors (`src/driver/driver.cpp`). There is no command-line option for it. The result is stored in `compile_report::core`: the functions built (and which of them are generic templates), the gaps, and the violations. Nothing in the report changes the exit status or the diagnostics.

Three tests cover it:

- `//src/core:verify_test` builds small cores by hand and checks that each rule accepts a correct program and rejects a broken one with the right id.
- `//src/core:adapter_test` checks the shape of the core built for each mapping rule of the adapter.
- `//src:core_corpus_test` compiles every program in `src/testdata/codegen_stress/` with the standard library, each in its own child process so that a crash is an observation. It de-duplicates functions by module and name, then prints the functions built, a histogram of gaps by construct, and the violations by rule. It fails when the number of functions built falls below a floor, when a program produces no observation, and when the verifier reports a violation that is not in `src/testdata/core_known_violations.txt`. The floor is raised as the adapter covers more. Setting `CORE_CORPUS_GAPS` prints each gap.

The allow-list has one entry per line: a rule id, a module, a function, and a comment giving the reason. A trailing `*` on a module or function matches by prefix. An entry is removed once the violation is fixed.

## Lowering from the core

HIR lowering is being moved onto the core. `hir::lower_core_function` (`src/hir/lower_core.h`, with one method per core node kind in `lower_core_*.cpp`) lowers one function from its core tree and `checked_types` (types, shapes, drop plans). It reads the AST only as a lookup key through `origin`. It returns `unsupported` with the node kind for anything it does not cover yet, and aborts with an internal error for a malformed tree.

`hir::substitute_core_functions` runs after the AST lowering and replaces each function the new lowering supports, so a module can mix functions from both paths. They use the same mangled names, owner modules and receiver convention. It covers scalar code, places, aggregates, direct calls, consuming receivers, the layout and container intrinsics, and drops: scope exits, early exits, assignment drops, drop flags and temporaries. The drops come from the same drop schedule the old lowerer reads, looked up by each core node's `origin`, and are emitted by `hir::drop_emitter` (`src/hir/drop_emission.h`), which both lowerings share. A binding's schedule key is `let::binding_origin` (the binding pattern, the `var` statement, or a `for` loop's iterable for its iterator) and a parameter's comes from the declaration.

`match_expr` carries the surface form it was elaborated from (`match_form`: `match`, `if let`, `while let`, a pattern `let`, `or else`, `?`, and the three `for` loop shapes), the AST subject as `subject_origin`, and how many dereferences the elaboration added to the subject (`subject_derefs`). Arms and pattern tests carry their AST patterns as `origin`, which is how the drop schedule's per-arm keys (owning subjects, unmatched and leftover drops) are found. A `for` loop's `while_loop` origin is the `for` statement or comprehension clause, and its block's origin is the iterable. `lower_core_match.cpp` uses these to rebuild the old lowerer's shapes: `hir_match` over a named subject local, `hir_let_else`, `hir_while_let`, and loop variables spliced into the enclosing block. A consumed `self` is the value in the core; HIR passes its reference, so each use reads through it. `lower_core_lambda.cpp` lowers lambdas with their capture lists and closure drop glue: a `lambda` records whether its captures are the source's own list (`capture_list`) and the AST lambda the drop schedule keys its captures by (`closure_origin`), and each `param` carries its binding pattern as `origin`. Generators and the bodies of `async def` functions lower with their cancel checks (`lower_core_task.cpp`); a `yield` statement's `jump::suspension` is the AST `yield` expression, which keys the drops of a generator dropped while suspended there. `lower_core_contract.cpp` lowers `contract_check`, whose `kind` says which clause it checks, and rebuilds the old lowerer's exits: a `return` through postconditions or crew ends, and a function body's tail, which returns through the postconditions after the body's drops. Indexing with an inclusive range uses the `range_view_inclusive` intrinsic, which takes the last index rather than one past it.

Tasks lower in `lower_core_task.cpp`. In a task, each suspension (an `await`, a `?`, a crew that ends, a `par` or `race`) is placed ahead of its statement, as the old lowerer's `await_hoist_` does: `core_lowerer::hoist_` collects those statements while one statement lowers, and a branch that produces a value and may suspend assigns a `<branch value>` local instead. A suspension in a place that cannot go first (the right side of `&&` or `||`, a later `elif` condition, a guard, or a loop condition outside a task) falls back. A task block lowers to a nested function named `<task block FN@start>` and an `on(...)` to a wrapper named `<on FN@start>`; `substitute_core_functions` replaces the old lowerer's copies by name and keeps them when the function that made them falls back. A crew is the block `{ let k = crew_new(); body; crew_end(k) }`, whose `origin` is the crew statement or expression. Whether `crew_end` joins or yields depends on whether it is in a task, and a cancel check ends every open crew before it returns. `crew_errors[task](&k, new, push)` names the functions the checker chose to make and fill the list.

The adapter takes `core::adapt_options`. With `contract_checks` off it leaves out every `contract_check` and the `let return` and `let self` bindings they read, and the driver passes the same setting it gives lowering. A trait default the checker copied for a generic impl is a template in the core, but the old lowerer lowers it once with the impl's parameters left abstract; `is_type_erased_default_copy` in `lower_core.cpp` lowers that template the same way until phase 5 of the unified checker plan instantiates generic impls. A `for` over a borrowed collection whose consuming `into_iter` the checker allows (todo 39: the elements are `copy`) is `into_iter(bitwise_copy(xs))` in the core. `bitwise_copy[T](&T) -> T` duplicates the referent's bytes without making a second owner; it exists only for this loop and is removed in phase 7 of the unified checker plan. Lowering maps it back to the old lowerer's call on the reference, marked `receiver_borrowed`, and the iterator is not dropped. The failure arm of a `?` makes the value it hands back after the crews it leaves end and the drops run, as the old lowerer does (`try_failure_exit_`). The core checks postconditions at a `?` exit and the old lowerer does not; the specification (chapter 34) checks a `post` at each exit, so the core's behaviour is the intended one. Owned static reads still fall back.

`hir::lower_core_program` (`lower_core_module.cpp`) builds whole modules from the core, with no call into the old lowerer: each core module's functions in the adapter's order, which is the old lowerer's, then the functions its task blocks made, and its statics (`lower_core_static`, the expression `lower_static_global` builds). A module with a function the adapter left out (`core::gap::item` says whether a gap is a function, a static or an `intrinsic def`) or a construct the lowering does not support is not made, and the driver reports it as a lowering failure of that module.

The driver uses it when `cli_config::lower_from_core` is set or `CINDER_LOWER_FROM_CORE` is defined, and records each function's path in `compile_report::core_lowering`. With `cli_config::lower_from_core_only`, or `CINDER_LOWER_FROM_CORE=only`, every module comes from `lower_core_program` instead. Compile-time code still lowers from the AST in both modes. With the switch off nothing changes. `//src:lower_core_corpus_test` runs the codegen corpus this way on both tiers with every check `codegen_stress_test` makes. It prints the share of functions lowered from the core and a histogram of fallback reasons, and it fails below a floor. `CINDER_LOWER_CORE_FILTER` selects files by name prefix, `CINDER_LOWER_CORE_DIFF=1` prints each function whose HIR differs structurally from the old lowerer's (`=own` limits it to the program's own functions), and `CINDER_LOWER_CORE_LIST=1` prints each fallback with its reason. `//src:lower_core_only_corpus_test` runs the same corpus with `CINDER_LOWER_FROM_CORE=only`: it replaces every module with the one `lower_core_program` makes and fails when a module is missing, holds other functions or statics, or holds them in another order. The corpus runner both tests share is `src/codegen_corpus.h`.

## Open shapes

`checked_types::open_shapes` holds the member types of struct and sum types that mention a generic parameter, such as `pair[T]` inside a generic body, expressed in terms of those parameters. It is kept apart from `shapes` because no layout exists for such a type. Only the core reads it: the adapter, to build field and payload projections in templates, and the verifier, to check them.

## See also

- [Type Checking and Inference](63-type-checking-and-inference.md) — the checker whose side tables the adapter reads.
- [Compiler Structure](62-compiler-structure.md) — where the core sits among the other stages.
- [HIR and the Bytecode VM](66-hir-and-bytecode-vm.md) — the representation lowering produces today.
