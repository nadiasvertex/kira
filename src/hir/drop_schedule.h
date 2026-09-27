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
/// tree for real.
///
/// `exits` is keyed by what `lowerer` already has in hand where it emits the
/// drops: the address of the statement vector whose scope closes normally
/// (the same vector `lower_block` receives by reference: `&decl.body_stmts`,
/// `&if_branch.body`, `&match_arm.body_stmts`, ...), the `func_decl` itself
/// for its parameters' scope, or the `return_stmt`/`break_stmt`/
/// `continue_stmt` node that leaves early. Each entry lists the bindings
/// still owned there, in the order they drop: innermost scope first, each
/// scope in reverse declaration order.
struct drop_schedule {
  std::unordered_map<const void *, std::vector<pending_drop>> exits;
};

/// Computes the drop schedule for one function body, including every lambda
/// nested inside it (a lambda is lowered inline, as part of its enclosing
/// function). What is still owned at each exit comes from the ownership
/// checker's own control-flow graph and "maybe moved" pass
/// (`semantic::ownership::owned_at_scope_exits`), so a value only lent to a
/// `&`/`&mut` parameter is still dropped, and one moved on any path to an
/// exit is not — leaking on the paths that did not move it, rather than
/// dropping a moved-from value. Only bindings that own their value whole
/// (`let`/`var name`, single-name parameters) are dropped; a pattern binding
/// may alias part of its subject.
[[nodiscard]] auto compute_drop_schedule(const ast::func_decl &decl,
                                         const semantic::checked_types &checked)
    -> drop_schedule;

} // namespace cinder::hir
