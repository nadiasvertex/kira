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

namespace cinder::hir {

/// A program lowered to the storage form and verified, ready for either
/// backend: the driver's program, or the code a compile-time call reaches. Owns
/// the layouts and the environment the storage modules refer to, so it must
/// outlive anything compiled from it.
struct storage_program {
  std::unique_ptr<runtime::value_layouts> layouts;
  std::unique_ptr<hir::storage_env> env;
  std::vector<hir::storage_lowering> lowerings;
  /// `lowerings`' modules, entry module first.
  std::vector<const hir::storage_module *> modules;
};

/// Lowers `modules` (the entry module first, then every module it reaches)
/// to the storage form and verifies each one. A function the storage form
/// cannot express yet, or a verifier failure, is a gap in the compiler; the
/// error names it.
[[nodiscard]] auto
prepare_storage_program(std::span<const hir::hir_module *const> modules,
                        semantic::checked_types &checked)
    -> std::expected<std::unique_ptr<storage_program>, std::string>;

} // namespace cinder::hir
