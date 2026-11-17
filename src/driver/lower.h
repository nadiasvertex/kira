#pragma once

namespace cinder::driver {

/// The HIR lowering outcome for one module (`hir::lower_program`), shown
/// under `--show-compile-details`. A failure is also reported as a
/// diagnostic when code was asked for (`lowering_stage.cpp`).
struct hir_lowering_result {
  std::string
      module_path; ///< Same module label as the matching `compiled_module`.
  bool lowered =
      false;         ///< Whether lowering made this module.
  std::string error; ///< Lowering error message; empty when `lowered` is true.
};
} // namespace cinder::driver
