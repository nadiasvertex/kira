#pragma once

#include <vector>

#include "driver.h"
#include "parse_stage.h"
#include "src/hir/nodes.h"
#include "src/semantic/types.h"

namespace kira::driver {

/// Emits module metadata and (unless `cfg.parse_only`) lowers to HIR for
/// every file in `parsed_inputs` that parsed and checked cleanly.
///
/// Files already flagged in `file_has_errors`, or with a null AST, are
/// skipped entirely. For the rest: metadata is written under `metadata_root`
/// and recorded in `report.modules`; when lowering is enabled, the outcome
/// (success or failure message) is recorded in `report.hir_modules`
/// regardless of whether it succeeded, but only successfully lowered
/// modules are returned for `--run`/`--compile` to target.
///
/// @param cfg Command-line configuration (checked for `parse_only`).
/// @param parsed_inputs Parsed files from `parse_sources`, in source order.
/// @param file_has_errors Per-file-id flags from parsing/semantic analysis.
/// @param checked Type-checking result HIR lowering resolves types against.
/// @param metadata_root Root directory configured for metadata output.
/// @param report Aggregate compile report; `modules` and `hir_modules` are
/// appended to.
[[nodiscard]] auto
lower_and_emit_modules(const cli_config &cfg,
                       const std::vector<parsed_input> &parsed_inputs,
                       const std::vector<bool> &file_has_errors,
                       const semantic::checked_types &checked,
                       const std::filesystem::path &metadata_root,
                       compile_report &report) -> hir::ptr_vec<hir::hir_module>;

} // namespace kira::driver
