#pragma once

namespace kira::driver {

/// Best-effort HIR lowering outcome for one module file (see
/// `src/hir/lower.h`). Lowering coverage is still partial — generics,
/// `for`/`while let`/comprehensions over a user-defined iterable, and the
/// concurrency/compile-time forms all still reject — so this is
/// informational only: it does not affect `compile_report::error_count`
/// or whether metadata gets written.
/// A module that fails to lower is not a compilation failure yet.
struct hir_lowering_result {
  std::string
      module_path; ///< Same module label as the matching `compiled_module`.
  bool lowered =
      false;         ///< Whether `hir::lower_module` succeeded for this module.
  std::string error; ///< Lowering error message; empty when `lowered` is true.
};
} // namespace kira::driver
