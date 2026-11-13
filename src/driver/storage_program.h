#pragma once

#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "src/hir/nodes.h"
#include "src/hir/storage.h"
#include "src/hir/storage_lower.h"
#include "src/runtime/value_layout.h"
#include "src/semantic/checker_results.h"

namespace cinder::driver {

/// A program lowered to the storage form and verified, ready for either
/// backend. Owns the layouts and the environment the storage modules refer
/// to, so it must outlive anything compiled from it.
struct storage_program {
  std::unique_ptr<runtime::value_layouts> layouts;
  std::unique_ptr<hir::storage_env> env;
  std::vector<hir::storage_lowering> lowerings;
  /// `lowerings`' modules, entry module first.
  std::vector<const hir::storage_module *> modules;
};

/// Whether the driver runs and builds programs through the storage form, the
/// default, rather than the pointer-per-aggregate form (`CINDER_STORAGE_FORM=0`).
/// A whole program takes one form or the other (spec/value-semantics.md,
/// decision 6). The pointer form is removed by T-14.
[[nodiscard]] auto storage_form_enabled() -> bool;

/// Lowers `modules` (the entry module first, then every module it reaches)
/// to the storage form and verifies each one. A function the storage form
/// cannot express yet, or a verifier failure, is a gap in the compiler; the
/// error names it.
[[nodiscard]] auto
prepare_storage_program(std::span<const hir::hir_module *const> modules,
                        semantic::checked_types &checked)
    -> std::expected<std::unique_ptr<storage_program>, std::string>;

} // namespace cinder::driver
