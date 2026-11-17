#pragma once

#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "src/core/adapter.h"
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
  /// The message the user sees when the program uses a feature the
  /// compiler does not build yet; empty when the gap is the compiler's own.
  std::string message;
};

/// Lowers `function` (a non-generic core function of the core module
/// `core_module`) as a function of the HIR module `hir_module`.
[[nodiscard]] auto lower_core_function(const core::function &function,
                                       std::string_view core_module,
                                       std::string_view hir_module,
                                       const semantic::checked_types &checked,
                                       const lowering_options &options = {})
    -> std::expected<ptr<hir_function>, unsupported>;

/// Lowers a function the adapter built on demand for compile-time code
/// (`core::adapt_function`, `core::adapt_expression`) as a function of the
/// module `module`, as `lower_function` and `lower_comptime_expression` do
/// with `lowering_options::compile_time`. A function with no declaration is
/// a compile-time expression's: its captures are its parameters, it returns
/// the body's value, and it drops nothing.
[[nodiscard]] auto
lower_core_comptime_function(const core::function &function,
                             std::string_view module,
                             const semantic::checked_types &checked)
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
/// per HIR function. `on_replace`, when given, sees each function the old
/// lowerer made beside its replacement, and their module, before the
/// replacement is made.
using replace_fn = std::function<void(const hir_module &,
                                      const hir_function &old_lowered,
                                      const hir_function &)>;
auto substitute_core_functions(ptr_vec<hir_module> &modules,
                               const core_lowering_input &input,
                               const semantic::checked_types &checked,
                               const lowering_options &options = {},
                               const replace_fn &on_replace = {})
    -> std::vector<core_lowering_record>;

/// Why the core lowering could not lower one module whole: a function the
/// adapter left out, or a construct `lower_core_function` does not handle.
struct core_module_failure {
  /// The HIR name of the module.
  std::string module;
  /// The function or static, when the failure is one.
  std::string item;
  std::string message;
  source_span span = source_span::dummy();
  std::optional<file_id_type> file_id;
};

/// What `lower_core_program` made: the modules it lowered whole, in the
/// core program's order, one record per function, and the modules it could
/// not lower.
struct core_program_lowering {
  ptr_vec<hir_module> modules;
  std::vector<core_lowering_record> records;
  std::vector<core_module_failure> failures;
};

/// Lowers every module of `input.modules` from the core alone, as
/// `lower_module`, `lower_inline_submodules` and `lower_functor_modules` do
/// from the AST (`cli_config::lower_from_core_only`): its functions in the
/// adapter's order (which is the old lowerer's), each followed after the
/// rest by the functions its task blocks made, and its statics. A generic
/// template is not lowered, except a trait default copied for a generic
/// impl, which the old lowerer lowers once with the impl's parameters
/// abstract. A module with a function the adapter left out (`gaps`, other
/// than a template's or an `intrinsic def`'s) or one this lowering does not
/// support is not made; its failure is reported instead.
[[nodiscard]] auto lower_core_program(const core_lowering_input &input,
                                      std::span<const core::gap> gaps,
                                      const semantic::checked_types &checked,
                                      const lowering_options &options = {})
    -> core_program_lowering;

/// The value of a reified `static let` (`core::static_global::value`) as the
/// expression `lower_static_global` builds for it.
[[nodiscard]] auto lower_core_static(const core::static_global &global)
    -> std::expected<hir_static_global, unsupported>;

} // namespace cinder::hir
