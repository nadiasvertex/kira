#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "src/parser/ast.h"
#include "src/semantic/types.h"

namespace cinder::hir {

/// One local binding that needs a drop call inserted where it goes out of
/// scope — by name (not `semantic::symbol_id`: this pass runs over the raw
/// AST, before `lowerer` mints its own per-function symbol ids) and type
/// (the key into `semantic::checked_types::drop_plans`, which says what
/// calling drop actually looks like).
struct pending_drop {
  std::string name;
  semantic::type_id type = 0;
};

/// Where every synthesized drop call in one function/lambda body belongs,
/// computed once (`compute_drop_schedule`) before `lowerer` walks the same
/// tree for real — see `spec/todo.md` item 6 and `spec/list-migration-
/// design.md`'s "Phase 0" for why this exists.
///
/// Both maps are keyed by the *address of the exact AST statement vector or
/// node* `lowerer` already has in hand at the point it needs to consult
/// them — `scope_exit` by `(const void *)&stmts` for whichever
/// `std::vector<ast::ptr<ast::node>>` a block's body lives in (the same
/// vector `lower_block` receives by reference: `&decl.body_stmts`,
/// `&if_branch.body`, `&while_stmt.body`, `&match_arm.body_stmts`, a
/// `for_stmt.body` forwarded through `lower_guarded_for_body`, ...), and
/// `jump_exit` by the `ast::node*` of the `return_stmt`/`break_stmt`/
/// `continue_stmt` itself. This works with zero new plumbing through
/// `lower_block`'s parameter list because both this pass and the real
/// lowering walk the identical tree, back to back, within one
/// `lower_function`/lambda-lowering call — including for a monomorphized
/// generic instance, whose cloned tree is already the one both passes see.
struct drop_schedule {
  /// Bindings still live (declared, not moved) when a scope closes
  /// normally, in reverse declaration order — the order they drop in.
  std::unordered_map<const void *, std::vector<pending_drop>> scope_exit;
  /// Bindings a `return`/`break`/`continue` must drop before it jumps,
  /// grouped per enclosing scope it passes through (innermost first, each
  /// group itself in reverse declaration order) — a `return` covers every
  /// scope back to the function/lambda's own top scope; `break`/`continue`
  /// cover only scopes back to (and including) the nearest enclosing loop's
  /// body scope.
  std::unordered_map<const ast::node *, std::vector<std::vector<pending_drop>>>
      jump_exit;
};

/// Computes the drop schedule for one function body, including every lambda
/// syntactically nested inside it (a lambda is only ever lowered inline, as
/// part of lowering its enclosing function — see `lowerer::lower_lambda`'s
/// sole call site — so one combined schedule per top-level function body
/// covers everything `lower_block`/`lower_stmt` will ever need to consult
/// while lowering it).
[[nodiscard]] auto compute_drop_schedule(const ast::func_decl &decl,
                                         const semantic::checked_types &checked)
    -> drop_schedule;

} // namespace cinder::hir
