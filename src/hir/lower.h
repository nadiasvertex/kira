#pragma once

#include <cstdint>
#include <expected>
#include <string>

#include "src/hir/nodes.h"
#include "src/parser/ast.h"
#include "src/parser/source_location.h"
#include "src/semantic/types.h"

namespace kira::hir {

/// Why `lower_function`/`lower_module` refused to produce HIR for some
/// construct. Lowering fails closed (spec/typed-ir-design.md Decision 1):
/// an unsupported or not-fully-typed construct is a hard error, never a
/// silently-skipped function or a placeholder node.
enum class lowering_error_kind : uint8_t {
  unsupported_construct, ///< Outside this milestone's scope — generics,
                         ///< destructuring patterns, `if let`, named call
                         ///< arguments, and every construct not listed in
                         ///< spec/typed-ir-design.md's milestone-1 breakdown.
  unannotated_parameter, ///< A parameter has no explicit type annotation.
  missing_return_type,   ///< The function has no explicit declared return type.
  unresolved_type,       ///< An expression's or declaration's checked type
                         ///< isn't concrete, or was never recorded at all.
};

/// One reason lowering stopped, plus where in the source it happened.
struct lowering_error {
  lowering_error_kind kind;
  source_span span;
  std::string message;
};

/// Knobs the driver hands lowering, as opposed to facts it reads out of the
/// checker.
struct lowering_options {
  /// Whether a contract the checker could not discharge statically becomes a
  /// runtime `hir_contract_check`. False is the spec's release elision
  /// ("Release builds may elide runtime contract checks with an explicit
  /// flag — doing so is the programmer's assertion that all contracts hold by
  /// other means", spec/kira-reference.md), reached via `--no-contract-checks`.
  /// It is deliberately not the default: a contract that is only *believed*
  /// silently is worth less than one that is checked.
  bool contract_checks = true;
};

/// Lowers one free function to HIR. Requires (Decision 1, Decision 2, and
/// this milestone's Non-Goals): every parameter explicitly annotated, an
/// explicit declared return type, and no generic parameters — with one
/// exception, a const-generic *instance* (`semantic::const_generic_instance`),
/// which keeps its template's `[n: usize]` list but had every value parameter
/// substituted for a constant before its body was checked, and so is as
/// concrete as any other function by the time it arrives here.
[[nodiscard]] auto lower_function(const ast::func_decl &decl,
                                  const semantic::checked_types &checked,
                                  const lowering_options &options = {})
    -> std::expected<ptr<hir_function>, lowering_error>;

/// Lowers every top-level free function declared directly in `file`'s item
/// list — not one nested inside a `module`/`impl`/`extend`/`trait` block,
/// which is out of this milestone's scope — plus every function the checker
/// synthesized for this module: trait defaults, item splices, and one
/// const-generic instance per constant a call site instantiated a template
/// with (the templates themselves are skipped; they have no runtime form).
/// Fails closed on the first function `lower_function` rejects: a
/// module-level failure means the module isn't lowered yet, not that the
/// offending function was silently dropped from the result.
[[nodiscard]] auto lower_module(const ast::file &file, std::string module_name,
                                const semantic::checked_types &checked,
                                const lowering_options &options = {})
    -> std::expected<ptr<hir_module>, lowering_error>;

} // namespace kira::hir
