#pragma once

#include <expected>
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

/// HIR lowering (`spec/unified-checker-plan.md`, phase 3): turns the typed
/// core into the `hir_function`s and `hir_module`s storage lowering and the
/// backends consume. It is the compiler's only lowering.
///
/// What it may read:
///   * the core program;
///   * from `checked_types`: types, shapes, drop plans, and the ownership
///     facts the drop schedule is computed from;
///   * a core node's `origin`, only as the key the checker's ownership and
///     drop records are stored under, while the adapter exists.
///
/// What it may not do: read the AST for meaning, or make a decision the core
/// already made (no inference, name lookup or dispatch). When it needs
/// something the core does not hold, that is a gap in the core.
///
/// A construct the checker accepts and the compiler does not build is
/// refused with a user-facing "not implemented" message (`unsupported::
/// message`). Any other refusal is an internal compiler error.
namespace cinder::hir {

/// A core construct lowering refused: the core node kind (or a narrower
/// description, such as "intrinsic len"), and where it is.
struct unsupported {
  std::string kind{};
  source_span span = source_span::dummy();
  /// The message the user sees when the program uses a feature the
  /// compiler does not build yet. Empty when the refusal is an internal
  /// compiler error: a verified core tree the lowering should handle.
  std::string message{};
};

/// Lowers a function the adapter built on demand for compile-time code
/// (`core::adapt_function`, `core::adapt_expression`) as a function of the
/// module `module`, with `lowering_options::compile_time`. A function with
/// no declaration is a compile-time expression's: its captures are its parameters, it returns
/// the body's value, and it drops nothing.
[[nodiscard]] auto
lower_core_comptime_function(const core::function &function,
                             std::string_view module,
                             const semantic::checked_types &checked)
    -> std::expected<ptr<hir_function>, unsupported>;

/// The core program and how its module names map onto HIR's. A file with no
/// `module` declaration is the core module "" and the HIR module named after
/// the file (`file_module_names`), and its submodules are named under it.
struct core_lowering_input {
  std::span<const core::module *const> modules;
  std::unordered_map<file_id_type, std::string> file_module_names;
};

/// Why lowering could not make one module: a function the adapter left out,
/// or a construct lowering refused.
struct core_module_failure {
  /// The HIR name of the module.
  std::string module;
  /// The function or static, when the failure is one.
  std::string item;
  std::string message;
  source_span span = source_span::dummy();
  std::optional<file_id_type> file_id;
  /// The program uses a feature the compiler does not build yet, and
  /// `message` says which. Otherwise the failure is an internal compiler
  /// error, and `construct` names what could not be built.
  bool feature = false;
  std::string construct;
};

/// What `lower_core_program` made: the modules it lowered whole, in the
/// core program's order, and the modules it could not lower.
struct core_program_lowering {
  ptr_vec<hir_module> modules;
  std::vector<core_module_failure> failures;
};

/// Lowers every module of `input.modules`: each file's module, its inline
/// submodules, and each functor instantiation. A module holds its functions
/// in the adapter's order, each followed after the rest by the functions its
/// task blocks made, and its statics. A generic template is not lowered. A
/// module with a function the adapter left out (`gaps`, other than a
/// template's or an `intrinsic def`'s) or one lowering refuses is not made;
/// its failure is reported instead.
[[nodiscard]] auto lower_core_program(const core_lowering_input &input,
                                      std::span<const core::gap> gaps,
                                      const semantic::checked_types &checked,
                                      const lowering_options &options = {})
    -> core_program_lowering;

/// The value of a reified `static let` (`core::static_global::value`) as the
/// global the backends initialize once before running anything.
[[nodiscard]] auto lower_core_static(const core::static_global &global)
    -> std::expected<hir_static_global, unsupported>;

/// A whole program lowered: the core the adapter built, and its HIR.
struct lowered_program {
  core::adapted_program core;
  core_program_lowering hir;
};

/// Builds the core of `inputs` (`core::adapt_program`) and lowers every
/// module of it (`lower_core_program`): what the driver, the corpus runners
/// and the test fixtures run to get HIR for a checked program.
/// `file_module_names` names the HIR module of each file with no `module`
/// declaration (the driver uses the file's stem).
[[nodiscard]] auto
lower_program(std::span<const semantic::parsed_module> inputs,
              const std::vector<bool> &file_has_errors,
              semantic::checked_types &checked,
              std::unordered_map<file_id_type, std::string> file_module_names,
              const lowering_options &options = {}) -> lowered_program;

/// Lowers a core the adapter already built (`core::adapt_program`), as
/// `lower_program` lowers the one it builds: for a caller that checks the
/// core first (`core::check_ownership`).
[[nodiscard]] auto
lower_adapted_program(core::adapted_program core,
                      semantic::checked_types &checked,
                      std::unordered_map<file_id_type, std::string>
                          file_module_names,
                      const lowering_options &options = {}) -> lowered_program;

} // namespace cinder::hir
