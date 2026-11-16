#pragma once

#include <cstddef>
#include <expected>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "src/hir/drop_schedule.h"
#include "src/hir/lower.h"
#include "src/hir/nodes.h"
#include "src/hir/reference_check.h"
#include "src/semantic/checker_results.h"

/// The HIR drops a function's drop schedule (`drop_schedule.h`) asks for:
/// the calls that drop one value, the drops at a scope's end and before a
/// jump, an assignment's drop of the value it overwrites, and the drop flags
/// that guard drops of values moved on some paths only. Both HIR lowerings
/// call it, the AST-reading `lowerer` (`lower.cpp`) and the one from the
/// core (`lower_core.h`); each says how to mint a symbol and find a local.
namespace cinder::hir {

/// Whether control never leaves `node` normally: a jump, or a block, `if`
/// or `match` whose every path ends in one. Nothing may be appended after
/// such a node.
[[nodiscard]] auto always_exits(const hir_node &node) -> bool;

/// Whether control can never reach the statement *after* `node` — every path
/// through it returns. Postcondition lowering asks this about a function
/// body's tail: a tail that always returns has already checked the
/// postcondition at each of those returns, and appending anything after it
/// would be not just dead code but invalid (both backends terminate the
/// current basic block on the last `return`, and LLVM's verifier rejects an
/// instruction that follows a terminator).
[[nodiscard]] auto always_returns(const hir_node &node) -> bool;

/// Rebuilds a place made of pure projections rooted at a local (`p`,
/// `p.inner`, `p.0`, `*r`) so it can be evaluated again; anything else is
/// refused.
[[nodiscard]] auto clone_place(const hir_expr &place)
    -> std::expected<ptr<hir_expr>, lowering_error>;

/// Whether `decl`, or the template it is an instance of, owns the value its
/// `self` refers to (`ownership_results::consuming_receivers`).
[[nodiscard]] auto owns_receiver(const semantic::checked_types &checked,
                                 const ast::func_decl *decl) -> bool;

class drop_emitter {
public:
  template <typename T> using outcome = std::expected<T, lowering_error>;
  /// Builds a fresh expression for the place a drop reads. Called once per
  /// drop call made, since each needs its own tree.
  using place_fn = std::function<ptr<hir_expr>()>;
  using field_paths = std::vector<moved_path>;

  /// What the drops need from the lowering that places them.
  struct environment {
    /// A fresh symbol of the function being lowered.
    std::function<symbol_id()> mint;
    /// The innermost local in scope that `node` (a `pending_drop::node`)
    /// declares whole.
    std::function<std::optional<symbol_id>(const void *node)> whole_local;
    /// The symbol a call names the function `name` by.
    std::function<symbol_id(std::string_view name)> function_symbol;
  };

  drop_emitter(const semantic::checked_types &checked, environment env)
      : checked_(checked), env_(std::move(env)) {}

  /// Starts a function: its schedule, with no drop flag declared yet.
  auto start(drop_schedule schedule) -> void;
  [[nodiscard]] auto schedule() const -> const drop_schedule & {
    return schedule_;
  }

  // ---- drop flags ---------------------------------------------------------
  [[nodiscard]] auto flag_ref(std::size_t flag, source_span span)
      -> outcome<ptr<hir_expr>>;
  /// Appends `flag = value` for each of `flags`.
  [[nodiscard]] auto set_flags(const std::vector<std::size_t> &flags,
                               bool value, source_span span,
                               ptr_vec<hir_node> &out) -> outcome<void>;
  /// Declares, set, the drop flags of the binding `node` makes.
  auto declare_flags(const void *node, source_span span,
                     ptr_vec<hir_node> &out) -> void;
  /// `body` under one `if flag` per flag, the last innermost.
  [[nodiscard]] auto guarded(const std::vector<std::size_t> &flags,
                             ptr_vec<hir_node> body, source_span span)
      -> outcome<ptr_vec<hir_node>>;
  /// `lowered`, the value of the expression `key`, as `{flag = false;
  /// value}` for each flag that a move by `key` clears; unchanged when none.
  [[nodiscard]] auto clear_moved(const void *key, ptr<hir_expr> lowered)
      -> outcome<ptr<hir_expr>>;

  // ---- drops ---------------------------------------------------------------
  /// Appends the drop calls of one value of type `type`: its own `drop`,
  /// then each droppable field, skipping `moved_paths`.
  [[nodiscard]] auto build_drop_calls(const place_fn &make_receiver,
                                      type_id type, source_span span,
                                      ptr_vec<hir_node> &out,
                                      const field_paths &moved_paths = {})
      -> outcome<void>;
  /// Appends the drop of one scheduled binding, under its flag.
  [[nodiscard]] auto emit_one_drop(const pending_drop &drop,
                                   ptr_vec<hir_node> &stmts) -> outcome<void>;
  /// Appends the drops `schedule().exits[key]` lists.
  [[nodiscard]] auto emit_scope_exit_drops(const void *key,
                                           ptr_vec<hir_node> &stmts)
      -> outcome<void>;
  /// `emit_scope_exit_drops` for a scope whose last statement is its value:
  /// the value goes to a temporary first, the drops run, and the temporary
  /// is the tail again.
  [[nodiscard]] auto emit_tail_scope_exit_drops(const void *key,
                                                ptr_vec<hir_node> &stmts)
      -> outcome<void>;
  /// Before a jump keyed by `key` that leaves the function or loop
  /// (`kind`): the placeholder `make_references_explicit` fills with the
  /// temporaries the jump leaves, then the scheduled drops.
  [[nodiscard]] auto emit_jump_drops(const void *key, jump_exit kind,
                                     source_span span, ptr_vec<hir_node> &stmts)
      -> outcome<void>;
  /// Appends `target op= value` for the assignment `key`: when the schedule
  /// says to, the new value goes to a temporary and the old one drops
  /// first, and afterwards the target's drop flags are set again.
  [[nodiscard]] auto assignment(const void *key, ast::assign_op op,
                                ptr<hir_expr> target, ptr<hir_expr> value,
                                source_span span, ptr_vec<hir_node> &stmts)
      -> outcome<void>;
  /// The drops of a temporary `make_references_explicit` made.
  [[nodiscard]] auto drop_temporary(const temporary &temp,
                                    ptr_vec<hir_node> &out) -> outcome<void>;
  /// Where the temporary holding the value of the expression `origin` ends.
  [[nodiscard]] auto temporary_end(const void *origin) const
      -> std::optional<const void *>;

  /// The result type of a call to `decl`, as the calls the drops make need
  /// it: an undeclared return type is the one the checker inferred.
  [[nodiscard]] auto declared_result_type(const ast::func_decl &decl) const
      -> type_id;

private:
  [[nodiscard]] auto build_array_drop(const place_fn &make_receiver,
                                      const semantic::drop_plan &plan,
                                      source_span span, ptr_vec<hir_node> &out,
                                      const field_paths &moved_paths)
      -> outcome<void>;

  const semantic::checked_types &checked_;
  environment env_;
  drop_schedule schedule_;
  /// The local each drop flag is declared as, once its binding is lowered.
  std::vector<symbol_id> flag_symbols_;
};

} // namespace cinder::hir
