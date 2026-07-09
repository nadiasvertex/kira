#pragma once

#include "driver.h"
#include "src/hir/nodes.h"
#include "src/semantic/types.h"

namespace kira::driver {

/// Executes `cfg.run_function` via the tier-0 VM when `cfg.run` is set,
/// recording the outcome in `report.run`. Locates the target module by
/// scanning `lowered_modules` for one that defines a matching function;
/// reports a "no such function" outcome rather than running anything when
/// none is found. No-op (leaves `report.run` unset) when `cfg.run` is false.
///
/// @param cfg Command-line configuration naming the function to run.
/// @param lowered_modules Successfully lowered HIR modules from this
/// session.
/// @param checked Type-checking result the target function's types resolve
/// against.
/// @param report Aggregate compile report; `run` is populated on request.
auto run_requested_function(
    const cli_config &cfg, const hir::ptr_vec<hir::hir_module> &lowered_modules,
    const semantic::checked_types &checked, compile_report &report) -> void;

/// Builds `cfg.build_function` into a standalone executable when
/// `cfg.build` is set, recording the outcome in `report.build`. Mirrors
/// `run_requested_function`'s module lookup; the output path defaults to
/// the first source file's stem plus `.out` when `cfg.build_output` is
/// empty.
///
/// @param cfg Command-line configuration naming the function/output to
/// build.
/// @param lowered_modules Successfully lowered HIR modules from this
/// session.
/// @param checked Type-checking result the target function's types resolve
/// against.
/// @param report Aggregate compile report; `build` is populated on request.
auto build_requested_function(
    const cli_config &cfg, const hir::ptr_vec<hir::hir_module> &lowered_modules,
    const semantic::checked_types &checked, compile_report &report) -> void;

} // namespace kira::driver
