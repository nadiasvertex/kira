#pragma once

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "src/core/nodes.h"
#include "src/hir/lower.h"
#include "src/hir/nodes.h"
#include "src/semantic/checker_results.h"

/// HIR lowering from the typed core (`spec/unified-checker-plan.md`, phase
/// 3): turns one `core::function` into the `hir_function` the old,
/// AST-reading lowerer (`lower.h`) produces for the same declaration.
///
/// What it may read:
///   * the core function, and nothing else of the program;
///   * from `checked_types`: types, shapes and drop plans;
///   * a core node's `origin`, only as the key the checker's ownership and
///     drop records are stored under, while the adapter exists.
///
/// What it may not do: read the AST for meaning, or make a decision the core
/// already made (no inference, name lookup or dispatch). When it needs
/// something the core does not hold, that is a gap in the core.
///
/// A construct it does not handle yet is `unsupported`, and the old lowerer
/// lowers that function instead (`substitute_core_functions`). Anything else
/// wrong with a verified core tree is an internal error.
namespace cinder::hir {

/// A core construct this lowering does not handle yet: the core node kind
/// (or a narrower description, such as "intrinsic len"), and where it is.
struct unsupported {
  std::string kind;
  source_span span = source_span::dummy();
};

/// Lowers `function` (a non-generic core function of the core module
/// `core_module`) as a function of the HIR module `hir_module`.
[[nodiscard]] auto lower_core_function(const core::function &function,
                                       std::string_view core_module,
                                       std::string_view hir_module,
                                       const semantic::checked_types &checked,
                                       const lowering_options &options = {})
    -> std::expected<ptr<hir_function>, unsupported>;

/// Which path lowered one HIR function.
struct core_lowering_record {
  std::string module;
  std::string function;
  bool from_core = false;
  /// For a fallback: the first unsupported construct, or why there was no
  /// core function to lower ("not built by the adapter").
  std::string fallback;
  source_span span = source_span::dummy();
};

/// The core program and how its module names map onto HIR's. A file with no
/// `module` declaration is the core module "" and the HIR module named after
/// the file (`file_module_names`), and its submodules are named under it.
struct core_lowering_input {
  std::span<const core::module *const> modules;
  std::unordered_map<file_id_type, std::string> file_module_names;
};

/// Replaces each function of `modules` that has a core counterpart and that
/// `lower_core_function` supports with the function lowered from the core;
/// every other function keeps what the old lowerer made. Returns one record
/// per HIR function.
auto substitute_core_functions(ptr_vec<hir_module> &modules,
                               const core_lowering_input &input,
                               const semantic::checked_types &checked,
                               const lowering_options &options = {})
    -> std::vector<core_lowering_record>;

} // namespace cinder::hir
