# Value Semantics T-01: Inventory and Baselines (2026-11-09)

T-01 of [value-semantics.md](value-semantics.md) is complete. The stdlib, the
test fixtures, the demo, and the benchmarks contain no recursive inline types,
so the infinite-size diagnostic needs no source migration. The inventory found
three separate "is this a heap pointer" classifiers that disagree, one reserved
builtin name set with no representation, and a corpus whose 180 files bypass the
ownership checker (14 of them would fail it). All 18 focused test targets pass
before any change. Allocation baselines for slices, tuple/option loops, generator
`next`, and blocking pipe reads are recorded below and are enforced as passing
behavioral fixtures only.

## Baseline test results

All 18 targets from the plan's focused list pass under `--config=debug`:
`runtime:{layout,allocator,allocator_arena,arena}_test`,
`hir:{lower,inline,captures,live_across_yield}_test`,
`semantic:{check,ownership_check}_test`, `bytecode:vm_test`,
`bytecode_compiler:{compile,register_alloc}_test`,
`llvm_codegen:{codegen,aot}_test`, `codegen_stress_test`, `std_test`, `cli_test`.

`codegen_stress_test` took 295 s against Bazel's 300 s default test timeout before
the new fixtures were added, and 268 s after. It has little headroom. Raising its
`timeout` in `src/BUILD.bazel` is worth doing before the suite grows.

The TSan, release, and `CINDER_COMPTIME_VERIFY=1` runs were not part of this
baseline. They run in T-08 and T-13.

## Allocation baselines

Measured with the new opt-in `CINDER_STRESS_REPORT=1` environment variable in
`src/codegen_stress_test.cpp` (pass it with `--test_env`). It prints heap
allocations and residual live blocks per tier. The VM and LLVM tiers produce
identical counts in every case. `N` is the loop length; `10N` is ten times that.

| Fixture | Workload | Allocations N / 10N | Live blocks at end N / 10N |
|---|---|---|---|
| `180`/`181` | slice a local array and pass the view, per pass | 1001 / 10001 | 1001 / 10001 |
| `182`/`183` | build and destructure a tuple, match an `option`, per pass | 2000 / 20000 | 2000 / 20000 |
| `184`/`185` | `generator.next()` and match, per pass | 1003 / 10003 | 1001 / 10001 |
| `186`/`187` | blocking pipe `write_all` + `read_exact`, per round (500 / 5000), plus `read_to_end` | 7051 / 70051 | 7050 / 70050 |
| `177` | existing coroutine borrow storage | 7 | 5 |
| `178` | existing coroutine objects freed | 6000 | 0 |
| `179` | existing `par`/`race` tasks freed | 3500 | 2000 |

Per-iteration cost today: one block per slice, two per tuple-plus-option pass, one
per generator `next`, and about fourteen per pipe round. None of these blocks is
freed, so allocations and live blocks grow linearly with `N`. Rows `180`–`187`
carry only `# expect:` values. Allocation bounds are added in T-13.

The expected values were computed independently of the compiler and match on both
tiers. They also match the exit codes from `cinder` run through the driver, which
runs the ownership checker.

## Boundary inventory

**Heap-value classification.** Three predicates answer the same question and
disagree:

| Predicate | Location | `opaque_kind` | Notes |
|---|---|---|---|
| `type_table::is_heap_represented` | `src/semantic/type_table.cpp:657` | no | Used by the LLVM tier, `reference_check.cpp`, `check_bounds.cpp`. |
| `function_compiler::is_heap_pointer_value` (VM) | `src/bytecode_compiler/compile.cpp:1618` | yes | Lists builtin generics by name: slice, slice_mut, option, result, cell, cell_mut. |
| `is_heap_kind` | `src/runtime/layout.cpp:~245` | yes | Every `builtin_generic_kind` is 8/8, including task, crew, generator, uninit. |

T-02 and T-03 replace all three with one layout/ABI classification.

**Opaque and builtin types.** `opaque_kind` is not a handle type. It is what
`type_table::user_type` assigns to any `type` declaration whose definition is not
a struct or sum, which means aliases, refinements, and functor-produced types
(`conn :: int32`, `positive :: int32 where self > 0`). Layout currently treats it
as an 8-byte pointer, which is wrong for a scalar alias. T-02 must resolve an
alias or refinement to its target before layout, and fail closed if it cannot.

Classification of every builtin generic in `k_builtin_generic_arities`:

| Name | Target representation |
|---|---|
| `option`, `result` | Sum: tag plus inline payload. |
| `slice`, `slice_mut` (and `str`) | Inline `{len, data}` descriptor. |
| `cell`, `cell_mut` | Inline address descriptor. |
| `task`, `crew`, `generator` | Fixed handle to runtime-owned, address-stable state. |
| `uninit[T, N]` | Inline bytes, size `N * size_of(T)`, alignment of `T`. |
| `box` | Reserved name. No construction site found in `src/`. Fail closed. |
| `channel`, `watch`, `mutex`, `atomic` | Reserved names only. The usable types are structs in `src/std/sync.cn`. Fail closed for the builtin form. |

`list` and `shared` are ordinary structs in `src/std/list.cn` and
`src/std/shared.cn`, not builtins. `fn_kind` is a 3-slot closure value today.

**Recursive types.** A scan of `type` declarations in `src/std`, `src/testdata`,
`demo`, and `bench` for a declaration that names itself found none. The only hit,
`src/std/platform.cn`'s `io_error`, is an alias to `std.io.io_error`. The scan
does not detect mutual recursion across two declarations. T-02's cycle detector
covers that case and should be run over the same trees.

**Strings.** `str` literals point at static bytes (VM: the function's
`string_constants`; LLVM: a global string) and allocate only the 2-word header.
Runtime routines (`rt_str_concat`, `reverse`, `trim`, `replace`,
`truncate_scalars`, `repeat_char`, all `rt_fmt_*`, `rt_str_find`/`rfind`'s result
struct, the platform queries) allocate both a fresh byte block and a header, and
nothing frees either. Moving the descriptor inline removes the header
allocation only. The byte-block leak is independent and is recorded here rather
than claimed fixed by this plan.

**Host and runtime boundaries that pass or return an aggregate pointer.**

- `src/intrinsics.h`: 67 `known_intrinsics`. Every `wire::ptr` parameter or result
  is an aggregate or handle pointer. Results of `rt_open`, `rt_close`, `rt_read`,
  `rt_write`, `rt_flush`, `rt_pipe`, `rt_set_nonblocking`, the `rt_*_version` and
  platform queries, and `rt_fd_*`/`rt_sleep_until` are `result` wrappers or task
  handles.
- `src/runtime/io.cpp:36` and `src/runtime/platform.cpp:34`: `make_result_ok` and
  `make_result_err` allocate the wrapper. `src/bytecode/vm.cpp` has parallel
  `alloc_struct`-based copies.
- `src/runtime/string.cpp`, `src/runtime/fmt.cpp`: `make_str`, `make_find_result`.
- `src/runtime/scheduler.h`, `reactor.h`: tasks cross as coroutine-object
  pointers (`rt_task_*`, `rt_crew_*`).
- `src/bytecode/vm.h`: `vm_result` carries one `slot_value`; `host_fn` takes and
  returns `slot_value`.
- `src/llvm_codegen/jit_support.cpp`: `run`, `run_ptr_result`, and `call_and_pack`
  cast the JIT symbol address to `T (*)()` / `void *(*)()`.
- `src/comptime_vm/values.cpp` (`to_vm`, `from_vm`) and `backend.cpp`: marshal
  through `slot_value`.
- Static initialization: `static_init_function` in the bytecode module, run by
  both `driver::run_hir_module` and the stress harness.
- `src/llvm_codegen/aot_runtime.cpp` and the C++ stubs in `aot_test.cpp`
  (`cinder_rt_task_drop`, `cinder_rt_task_release`) define the AOT-side runtime.

## Corpus ownership bypass

`check_source` in `src/codegen_stress_test.cpp` calls `check_program` and never
`check_ownership`, so all corpus files run without ownership checking. Running the
corpus through the driver (which does check ownership) shows 166 of the 180
pre-existing files pass and 14 fail:

| File | Diagnostic |
|---|---|
| `033_const_generic_monomorphization` | use of moved value `v` |
| `034_generic_monomorphization` | `[v; n]` needs a `copy` value, `T` is not one |
| `043_impl_on_generic_target` | cannot move out of `self.value` |
| `050_bound_driven_inference` | use of moved value `x` |
| `053_mutable_iteration` | cannot use `it` while the view `slot` of `it` is in use |
| `055_generic_impl_static_on_builtin` | use of moved value `final` |
| `058_parameterized_extend` | cannot move out of `self.a` |
| `091_unannotated_accumulator_in_generic` | cannot move out of an element of `xs` |
| `103_user_type_shadows_prelude_generic` | cannot move out of `self.w` |
| `104_bare_generic_static_call` | cannot move out of `self.v` |
| `105_open_leaf_answered_by_sibling` | cannot move out of an element of `xs` |
| `110_instances_by_substitution` | cannot move out of an element of `self.items` |
| `120_generic_call_names_instance_late` | cannot move out of an element of `xs` |
| `122_generic_block_method_implicit_param` | cannot move out of `self.v` |

Nine are "cannot move out of" a field or element reached through a borrow, three
are use-after-move, one lacks a `copy` bound, and one overlaps a live view. The
nine borrowed-move cases match decision 4 of the plan (moves through borrowed
`self` stay rejected). Whether each sits in a generic body was not checked. Each
needs a source rewrite (a `copy` bound, `clone`, or an owning parameter) before
it can serve as evidence for the migration. This is T-13 step 2. The new
`180`–`187` fixtures pass ownership checking.

## Changes made in T-01

- `src/codegen_stress_test.cpp`: opt-in `CINDER_STRESS_REPORT` output.
- `src/testdata/codegen_stress/180`–`187`: eight baseline fixtures.
- This document.

No compiler behavior changed.
