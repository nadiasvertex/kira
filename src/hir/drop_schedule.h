#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "src/parser/ast.h"
#include "src/semantic/ownership_cfg.h"
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
/// scope — by the AST node that declares it (not `semantic::symbol_id`: this
/// pass runs over the raw AST, before `lowerer` mints its own per-function
/// symbol ids) and type (the key into `semantic::checked_types::drop_plans`,
/// which says what calling drop actually looks like). `name` is only the
/// spelling, for the reference the drop call is made on.
struct pending_drop {
  /// What declares the binding: a `let`/`var` pattern, a pattern binding, a
  /// parameter's pattern, a loop's `iterable` for its `<for iterator>`, ...
  /// (`semantic::ownership::local_info::node`).
  const void *node = nullptr;
  std::string name;
  semantic::type_id type = 0;
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
  /// The nodes that declare a `whole` local (`local_info::node`).
  std::unordered_set<const void *> owned_bindings;
  /// The top-level patterns (a `let`, `if let`, `while let`, `match` arm or
  /// destructuring parameter) whose bindings own the parts they bind, so a
  /// `_` or `..` in them leaves a part to drop.
  std::unordered_set<const void *> owning_patterns;
  /// The `match` arm, `let` and `if let`/`while let` patterns whose bindings
  /// overlap, so the arm owns the whole subject as a `<subject>` local.
  std::unordered_set<const void *> owning_subjects;
  /// `function_cfg::leftover_drops` and `unmatched_drops`.
  std::unordered_set<const void *> leftover_drops;
  std::unordered_set<const void *> unmatched_drops;
  /// `function_cfg::owning_loops`, `loop_handles` and `consuming_calls`.
  std::unordered_set<const void *> owning_loops;
  std::unordered_set<const void *> loop_handles;
  std::unordered_set<const void *> consuming_calls;
  /// `function_cfg::temporary_ends`.
  std::unordered_map<const void *, const void *> temporary_ends;
  /// `function_cfg::owned_captures`: what each lambda's drop glue drops.
  std::unordered_map<const void *,
                     std::vector<semantic::ownership::owned_capture>>
      owned_captures;
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
