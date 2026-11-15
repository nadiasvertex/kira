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

Each node built from the AST keeps a pointer to its `origin` node, which is present only while the adapter builds the core.

## Running and testing

The core is not part of a normal compilation. It is built and verified when `cli_config::core_check` is set, or when the environment variable `CINDER_CORE_VERIFY` is defined, after the ownership check and only if checking reported no errors (`src/driver/driver.cpp`). There is no command-line option for it. The result is stored in `compile_report::core`: the functions built (and which of them are generic templates), the gaps, and the violations. Nothing in the report changes the exit status or the diagnostics.

Three tests cover it:

- `//src/core:verify_test` builds small cores by hand and checks that each rule accepts a correct program and rejects a broken one with the right id.
- `//src/core:adapter_test` checks the shape of the core built for each mapping rule of the adapter.
- `//src:core_corpus_test` compiles every program in `src/testdata/codegen_stress/` with the standard library, each in its own child process so that a crash is an observation. It de-duplicates functions by module and name, then prints the functions built, a histogram of gaps by construct, and the violations by rule. It fails when the number of functions built falls below a floor, when a program produces no observation, and when the verifier reports a violation that is not in `src/testdata/core_known_violations.txt`. The floor is raised as the adapter covers more. Setting `CORE_CORPUS_GAPS` prints each gap.

The allow-list has one entry per line: a rule id, a module, a function, and a comment giving the reason. A trailing `*` on a module or function matches by prefix. An entry is removed once the violation is fixed.

## Open shapes

`checked_types::open_shapes` holds the member types of struct and sum types that mention a generic parameter, such as `pair[T]` inside a generic body, expressed in terms of those parameters. It is kept apart from `shapes` because no layout exists for such a type. Only the core reads it: the adapter, to build field and payload projections in templates, and the verifier, to check them.

## See also

- [Type Checking and Inference](63-type-checking-and-inference.md) — the checker whose side tables the adapter reads.
- [Compiler Structure](62-compiler-structure.md) — where the core sits among the other stages.
- [HIR and the Bytecode VM](66-hir-and-bytecode-vm.md) — the representation lowering produces today.
