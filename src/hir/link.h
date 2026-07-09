#pragma once

#include <vector>

#include "src/hir/nodes.h"

namespace kira::hir {

/// Every module reachable from `entry`'s own functions, transitively, via a
/// module- or type-qualified call (`hir_local_ref::owner_module` — see
/// `check.cpp`'s `infer_qualified_call` and `lower.cpp`'s `lower_call`).
/// `entry` is always `result.front()`; every other module a program's
/// entry function needs to actually run is discovered by walking every
/// function body reachable so far and following `owner_module` references
/// into `all_modules`, one whole module at a time — a module referenced at
/// all is pulled in in full, matching the granularity lowering already
/// compiles at (one file/module = one compiled unit).
///
/// This is a read-only discovery pass, not a linker: nothing here mutates or
/// moves any `hir_module`/`hir_function` — `bytecode_compiler::compile_module`
/// and `llvm_codegen::compile_module`'s multi-module overloads consume the
/// returned pointers directly, computing each cross-module call's dispatch
/// key from `owner_module` at compile time instead. Because discovery never
/// touches the underlying HIR, the same `all_modules` can be reused any
/// number of times (e.g. once for `--run`, once for `--build`, in the same
/// invocation).
[[nodiscard]] auto
find_reachable_modules(const hir_module &entry,
                       const ptr_vec<hir_module> &all_modules)
    -> std::vector<const hir_module *>;

} // namespace kira::hir
