#pragma once

#include <string>
#include <vector>

#include "driver.h"
#include "parse_stage.h"
#include "src/hir/lower_core.h"
#include "src/hir/nodes.h"
#include "src/semantic/checker_results.h"
#include "src/semantic/ownership_cfg.h"

namespace cinder::driver {

/// Emits module metadata for every file in `parsed_inputs` that parsed and
/// checked cleanly, and takes the HIR modules lowering made.
///
/// Files already flagged in `file_has_errors`, or with a null AST, are
/// skipped entirely. For the rest, metadata is written under `metadata_root`
/// and recorded in `report.modules`. Each module's lowering outcome
/// (success or failure message) is recorded in `report.hir_modules`, but
/// only successfully lowered modules are returned for `--run`/`--compile`
/// to target.
///
/// A lowering failure is also reported as a real error diagnostic and counted
/// in `report.error_count`. It used to be recorded *only* in
/// `report.hir_modules`, which `render_compile_summary` prints just under
/// `--show-compile-details` — so the ordinary user saw a module silently
/// vanish and then the unrelated-looking `no compiled module defines a
/// function named `main``. A phase that cannot emit code has to say so.
///
/// @param cfg Command-line configuration (checked for `parse_only`).
/// @param parsed_inputs Parsed files from `parse_sources`, in source order.
/// @param file_has_errors Per-file-id flags from parsing/semantic analysis.
/// @param checked Type-checking result HIR lowering resolves types against.
/// @param metadata_root Root directory configured for metadata output.
/// @param renderer Renders lowering-failure diagnostics against the sources.
/// @param report Aggregate compile report; `modules`, `hir_modules`,
/// `diagnostics`, and `error_count` are appended to.
/// The HIR name of `input`'s module: its `module` path, or the file's stem.
[[nodiscard]] auto hir_module_name(const parsed_input &input) -> std::string;

/// `lowering` holds the modules `hir::lower_program` made, or is null when
/// the program was not lowered (it has errors, or `cfg.parse_only`). Each
/// module it made is returned, and each it could not make is reported.
/// `summaries` are the ownership summaries each module's metadata records,
/// or null when the ownership check did not run.
[[nodiscard]] auto
lower_and_emit_modules(const cli_config &cfg,
                       const std::vector<parsed_input> &parsed_inputs,
                       const std::vector<bool> &file_has_errors,
                       const semantic::checked_types &checked,
                       const std::filesystem::path &metadata_root,
                       const diagnostic_renderer &renderer,
                       compile_report &report,
                       hir::core_program_lowering *lowering,
                       const semantic::ownership::summary_table *summaries)
    -> hir::ptr_vec<hir::hir_module>;

} // namespace cinder::driver
