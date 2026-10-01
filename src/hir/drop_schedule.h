#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "src/parser/ast.h"
#include "src/semantic/types.h"

namespace cinder::hir {

/// A field path moved out of a value that is otherwise dropped. With a
/// `flag`, it was moved on some paths only, and is dropped with the value
/// when that drop flag says it is still there; without one, it was moved on
/// every path and is never dropped with the value.
struct moved_path {
  std::vector<std::string> path;
  std::optional<std::size_t> flag;
};

/// One local binding that needs a drop call inserted where it goes out of
/// scope — by name (not `semantic::symbol_id`: this pass runs over the raw
/// AST, before `lowerer` mints its own per-function symbol ids) and type
/// (the key into `semantic::checked_types::drop_plans`, which says what
/// calling drop actually looks like).
///
/// `shadow_depth` says which binding of `name` this is when several of that
/// name are owned at one exit (`let a = ...; let a = ...`): 0 is the most
/// recently declared, 1 the one it shadows, and so on.
struct pending_drop {
  std::string name;
  semantic::type_id type = 0;
  std::size_t shadow_depth = 0;
  /// Set when the binding was moved on some paths to this exit only: the
  /// drop runs when this drop flag is still set.
  std::optional<std::size_t> flag;
  /// Field paths (`{"output"}`, `{"a", "b"}`) that may have been moved out
  /// of the binding: those fields are not dropped with it (or only under
  /// their flag), and every other droppable field still is (ch. 14, Moving
  /// out of places).
  std::vector<moved_path> moved_paths;
};

/// The old value an assignment overwrites. It is dropped first when
/// `drop_old`, under every flag in `when` (the target and anything it is a
/// field of that may have been moved), less the `moved_paths`. After the
/// write, the drop flags in `sets` — the target's and those of the fields
/// below it — say it holds a value again.
struct assignment_drop {
  bool drop_old = false;
  std::vector<std::size_t> when;
  std::vector<moved_path> moved_paths;
  std::vector<std::size_t> sets;
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
///
/// A value moved on some paths to a drop and not on others has a run-time
/// drop flag (ch. 17, When drops run), numbered `0..flag_count`. Lowering
/// declares it set where the binding is made (`binding_flags`, keyed by the
/// `let`/`var` binding pattern, the `var` statement, or the parameter's
/// pattern), clears it where an expression moves the value
/// (`move_clears`, keyed by that expression), and sets it again where an
/// assignment fills it (`assignment_drop::sets`).
struct drop_schedule {
  std::unordered_map<const void *, std::vector<pending_drop>> exits;
  /// Keyed by the `ast::assign_stmt`.
  std::unordered_map<const void *, assignment_drop> assignments;
  std::unordered_map<const void *, std::vector<std::size_t>> binding_flags;
  std::unordered_map<const void *, std::vector<std::size_t>> move_clears;
  std::size_t flag_count = 0;
};

/// Computes the drop schedule for one function body, including every lambda
/// nested inside it (a lambda is lowered inline, as part of its enclosing
/// function). What is still owned at each exit comes from the ownership
/// checker's own control-flow graph and move passes
/// (`semantic::ownership::drop_facts_of`), so a value only lent to a
/// `&`/`&mut` parameter is still dropped, one moved on every path is not,
/// and one moved on some paths is dropped under a drop flag. A pattern
/// binding has no place to set a flag, so one moved on some paths only
/// leaks on the others.
[[nodiscard]] auto compute_drop_schedule(const ast::func_decl &decl,
                                         const semantic::checked_types &checked)
    -> drop_schedule;

} // namespace cinder::hir
