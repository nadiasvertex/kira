#pragma once

#include <cstdint>
#include <expected>
#include <string>

#include "src/hir/nodes.h"
#include "src/k-parser/ast.h"
#include "src/k-parser/source_location.h"
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

/// Lowers one free function to HIR. Requires (Decision 1, Decision 2, and
/// this milestone's Non-Goals): no generic parameters, every parameter
/// explicitly annotated, an explicit declared return type, and a body built
/// only from constructs in this milestone's scope (literals, identifiers,
/// binary/unary ops, calls with positional arguments, field/index access,
/// blocks, `let`, `if` as a statement or expression, `return`).
[[nodiscard]] auto lower_function(const ast::func_decl &decl,
                                  const semantic::checked_types &checked)
    -> std::expected<ptr<hir_function>, lowering_error>;

/// Lowers every top-level free function declared directly in `file`'s item
/// list — not one nested inside a `module`/`impl`/`extend`/`trait` block,
/// which is out of this milestone's scope. Fails closed on the first
/// function `lower_function` rejects: a module-level failure means the
/// module isn't lowered yet, not that the offending function was silently
/// dropped from the result.
[[nodiscard]] auto lower_module(const ast::file &file, std::string module_name,
                                const semantic::checked_types &checked)
    -> std::expected<ptr<hir_module>, lowering_error>;

} // namespace kira::hir
