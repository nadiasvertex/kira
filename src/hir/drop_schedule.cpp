#include "src/hir/drop_schedule.h"

#include <ranges>

namespace kira::hir {
namespace {

using semantic::checked_types;
using semantic::type_id;

struct local_entry {
  std::string name;
  type_id type = 0;
  bool moved = false;
};

/// One lexical scope on the walker's stack. `is_loop_boundary` marks a
/// `while`/`for` loop's own body scope — the point a `break`/`continue`
/// inside it (however deeply nested in further `if`/`block` scopes) stops
/// draining at.
struct scope_frame {
  std::vector<local_entry> locals;
  bool is_loop_boundary = false;
};

/// Walks one function/lambda body computing a `drop_schedule`. Mirrors
/// `semantic::move_checker`'s traversal shape (same statement/expression
/// positions count as consuming vs. not — see that class's doc comments for
/// the rationale behind each case) with three deliberate differences:
///
/// 1. Only bindings whose type is in `checked_.drop_plans` are tracked at
///    all — everything else (the overwhelming majority of locals) is
///    invisible to this pass, unlike move_check's much broader "any
///    non-scalar" trackability.
/// 2. Scopes are **ordered** (`std::vector`, declaration order preserved)
///    rather than move_check's `unordered_map`, since the whole point here
///    is emitting drops in reverse declaration order.
/// 3. **Only plain `let`/`var name = expr` bindings and simple (non-
///    destructured) parameters are tracked.** A destructuring `let (a, b) =
///    ...`, a `match` arm pattern, or a `for`/`while let` loop pattern can
///    also bind a droppable value, but those bindings are not covered here
///    — deliberately deferred (spec/todo.md item 6's remaining-gaps note),
///    since desugaring their lowering already routes through a synthesized
///    subject temporary (`lower_pattern`) this pass would need its own
///    parallel model of. The common case — a `let`-bound resource in
///    ordinary code — is unaffected.
///
/// **Branch-merge policy is the inverse of move_check's, and this matters
/// for correctness, not just diagnostics.** `move_checker::merge_states`
/// treats a binding as moved after an `if`/`match` only if *every* live
/// branch moved it (the right call for a "was this definitely already used
/// up" diagnostic — a false negative there just misses a warning). Scope-
/// exit drop scheduling needs the opposite: a binding must be treated as
/// moved after the construct if *any* branch moved it, because a single
/// static drop call is emitted once for the merged point, and on whichever
/// path a branch actually did move the value, a drop call there would be a
/// double-drop / drop of a moved-from value — memory-unsafe, not merely a
/// missed diagnostic. A leaked value on the path where it *wasn't* moved is
/// the safe direction to be wrong in; this walker takes it deliberately.
class drop_walker {
public:
  explicit drop_walker(const checked_types &checked) : checked_(checked) {}

  auto walk_function(const ast::func_decl &decl) -> void {
    // A method's `self`/`mut self` is always passed *by reference*,
    // regardless of the `mut` spelling — `move_check.cpp`'s
    // `receiver_is_moved` documents this ("methods always take `self` by
    // reference ... called repeatedly on the same binding throughout
    // `std.algo`"). So the function body never owns `self`, and it must
    // never be scheduled for a scope-exit drop — not just inside a type's
    // own `drop` method (which would otherwise recurse into itself
    // forever), but in *every* method: `push`/`reserve`/etc. on a droppable
    // receiver would otherwise drop it out from under a caller who still
    // owns and uses it after the call returns.
    push_scope();
    for (const auto &param : decl.params) {
      if (param.pattern != nullptr &&
          param.pattern->kind == ast::node_kind::binding_pattern &&
          dynamic_cast<const ast::binding_pattern &>(*param.pattern).name ==
              "self") {
        continue;
      }
      declare_param(param);
    }
    walk_nested_block(decl.body_stmts);
    pop_scope(&decl);
  }

  auto take_schedule() -> drop_schedule { return std::move(schedule_); }

private:
  const checked_types &checked_;
  drop_schedule schedule_;
  std::vector<scope_frame> scopes_;
  /// Mirrors `move_checker::diverged_`: true once the statement just walked
  /// unconditionally leaves the enclosing block, so the rest of it is dead
  /// code and contributes nothing to a branch merge.
  bool diverged_ = false;

  [[nodiscard]] auto is_droppable(type_id type) const -> bool {
    return checked_.drop_plans.contains(type);
  }

  [[nodiscard]] auto lookup_type(const ast::node *node) const -> type_id {
    if (node == nullptr) {
      return 0;
    }
    const auto it = checked_.node_types.find(node);
    return it != checked_.node_types.end() ? it->second : 0;
  }

  auto push_scope(bool is_loop_boundary = false) -> void {
    scopes_.push_back(scope_frame{.is_loop_boundary = is_loop_boundary});
  }

  /// Pops the top scope. When `key` is non-null, records its still-live
  /// (unmoved) droppable locals into `schedule_.scope_exit[key]`, in
  /// reverse declaration order — `key` is the address of whichever AST
  /// statement vector `hir::lowerer::lower_block` will itself be called
  /// with for this same scope (see `drop_schedule`'s doc comment), so the
  /// two passes agree without any new plumbing between them.
  auto pop_scope(const void *key) -> void {
    auto &frame = scopes_.back();
    if (key != nullptr) {
      auto &out = schedule_.scope_exit[key];
      for (const auto &local : std::views::reverse(frame.locals)) {
        if (!local.moved) {
          out.push_back(pending_drop{.name = local.name, .type = local.type});
        }
      }
    }
    scopes_.pop_back();
  }

  auto declare(std::string_view name, type_id type) -> void {
    if (name.empty() || scopes_.empty() || !is_droppable(type)) {
      return;
    }
    scopes_.back().locals.push_back(
        local_entry{.name = std::string(name), .type = type, .moved = false});
  }

  auto declare_param(const ast::param &param) -> void {
    if (param.pattern == nullptr ||
        param.pattern->kind != ast::node_kind::binding_pattern) {
      return; // destructured params: not tracked, see class doc comment.
    }
    const auto &binding =
        dynamic_cast<const ast::binding_pattern &>(*param.pattern);
    declare(binding.name, lookup_type(param.pattern.get()));
  }

  auto walk_lambda_body(const ast::lambda_expr &lambda) -> void {
    push_scope();
    for (const auto &param : lambda.params) {
      declare_lambda_param(param);
    }
    if (lambda.body_expr != nullptr) {
      walk_expr(*lambda.body_expr, /*consume_top=*/true);
    } else {
      walk_nested_block(lambda.body_stmts);
    }
    pop_scope(&lambda);
  }

  auto declare_lambda_param(const ast::lambda_param &param) -> void {
    if (param.pattern == nullptr ||
        param.pattern->kind != ast::node_kind::binding_pattern) {
      return; // destructured params: not tracked, see class doc comment.
    }
    const auto &binding =
        dynamic_cast<const ast::binding_pattern &>(*param.pattern);
    declare(binding.name, lookup_type(param.pattern.get()));
  }

  auto find(std::string_view name) -> local_entry * {
    for (auto &frame : std::views::reverse(scopes_)) {
      for (auto &local : std::views::reverse(frame.locals)) {
        if (local.name == name) {
          return &local;
        }
      }
    }
    return nullptr;
  }

  auto reset(std::string_view name) -> void {
    if (auto *entry = find(name)) {
      entry->moved = false;
    }
  }

  auto consume_named(std::string_view name) -> void {
    if (auto *entry = find(name)) {
      entry->moved = true;
    }
  }

  auto touch(const ast::ident_expr &ident) -> void {
    // A read never changes drop-tracked state; only ownership transfer does.
    (void)ident;
  }

  auto consume(const ast::ident_expr &ident) -> void {
    consume_named(ident.name);
  }

  /// Mirrors `move_checker::param_name_of`.
  [[nodiscard]] static auto param_name_of(const ast::param &param)
      -> std::string {
    if (param.pattern != nullptr &&
        param.pattern->kind == ast::node_kind::binding_pattern) {
      return dynamic_cast<const ast::binding_pattern &>(*param.pattern).name;
    }
    return {};
  }

  /// Mirrors `move_checker::receiver_is_moved`.
  [[nodiscard]] auto receiver_is_moved(const ast::call_expr &call) const
      -> bool {
    const auto it = checked_.resolved_callees.find(&call);
    if (it == checked_.resolved_callees.end() || it->second.decl == nullptr) {
      return false;
    }
    const auto &decl = *it->second.decl;
    if (decl.params.empty()) {
      return false;
    }
    const auto &front = decl.params.front();
    if (param_name_of(front) == "self") {
      return false;
    }
    const auto param_type = lookup_type(front.pattern.get());
    if (param_type == 0) {
      return false;
    }
    return checked_.types.entry(param_type).kind !=
           semantic::type_kind::ref_kind;
  }

  auto walk_callee(const ast::call_expr &call) -> void {
    if (call.callee->kind == ast::node_kind::field_expr) {
      const auto &field = dynamic_cast<const ast::field_expr &>(*call.callee);
      if (field.object != nullptr && receiver_is_moved(call)) {
        walk_expr(*field.object, /*consume_top=*/true);
        return;
      }
    }
    walk_expr(*call.callee, /*consume_top=*/false);
  }

  auto walk_expr(const ast::expr &expr, bool consume_top) -> void {
    if (expr.has_error) {
      return;
    }
    switch (expr.kind) {
    case ast::node_kind::ident_expr: {
      const auto &ident = dynamic_cast<const ast::ident_expr &>(expr);
      if (consume_top) {
        consume(ident);
      } else {
        touch(ident);
      }
      return;
    }
    case ast::node_kind::unary_expr: {
      const auto &unary = dynamic_cast<const ast::unary_expr &>(expr);
      if (unary.operand == nullptr) {
        return;
      }
      const auto is_borrow = unary.op == ast::unary_op::addr_of ||
                             unary.op == ast::unary_op::addr_of_mut;
      const auto is_projection = unary.op == ast::unary_op::deref;
      walk_expr(*unary.operand,
                (is_borrow || is_projection) ? false : consume_top);
      return;
    }
    case ast::node_kind::field_expr: {
      const auto &field = dynamic_cast<const ast::field_expr &>(expr);
      if (field.object != nullptr) {
        walk_expr(*field.object, /*consume_top=*/false);
      }
      return;
    }
    case ast::node_kind::index_expr: {
      const auto &index = dynamic_cast<const ast::index_expr &>(expr);
      if (index.object != nullptr) {
        walk_expr(*index.object, /*consume_top=*/false);
      }
      if (index.index != nullptr) {
        walk_expr(*index.index, /*consume_top=*/true);
      }
      return;
    }
    case ast::node_kind::binary_expr: {
      const auto &binary = dynamic_cast<const ast::binary_expr &>(expr);
      if (binary.lhs != nullptr) {
        walk_expr(*binary.lhs, /*consume_top=*/false);
      }
      if (binary.rhs != nullptr) {
        walk_expr(*binary.rhs, /*consume_top=*/false);
      }
      return;
    }
    case ast::node_kind::call_expr: {
      const auto &call = dynamic_cast<const ast::call_expr &>(expr);
      if (call.callee != nullptr) {
        walk_callee(call);
      }
      for (const auto &arg : call.args) {
        if (arg.value != nullptr) {
          walk_expr(*arg.value, /*consume_top=*/true);
        }
      }
      return;
    }
    case ast::node_kind::cast_expr: {
      const auto &cast = dynamic_cast<const ast::cast_expr &>(expr);
      if (cast.operand != nullptr) {
        walk_expr(*cast.operand, consume_top);
      }
      return;
    }
    case ast::node_kind::try_expr: {
      const auto &tri = dynamic_cast<const ast::try_expr &>(expr);
      if (tri.operand != nullptr) {
        walk_expr(*tri.operand, consume_top);
      }
      return;
    }
    case ast::node_kind::group_expr: {
      const auto &group = dynamic_cast<const ast::group_expr &>(expr);
      if (group.inner != nullptr) {
        walk_expr(*group.inner, consume_top);
      }
      return;
    }
    case ast::node_kind::tuple_expr: {
      const auto &tuple = dynamic_cast<const ast::tuple_expr &>(expr);
      for (const auto &element : tuple.elements) {
        if (element != nullptr) {
          walk_expr(*element, /*consume_top=*/true);
        }
      }
      return;
    }
    case ast::node_kind::array_expr: {
      const auto &array = dynamic_cast<const ast::array_expr &>(expr);
      for (const auto &element : array.elements) {
        if (element != nullptr) {
          walk_expr(*element, /*consume_top=*/true);
        }
      }
      if (array.fill_value != nullptr) {
        walk_expr(*array.fill_value, /*consume_top=*/true);
      }
      return;
    }
    case ast::node_kind::struct_expr: {
      const auto &literal = dynamic_cast<const ast::struct_expr &>(expr);
      for (const auto &field : literal.fields) {
        if (field.value != nullptr) {
          walk_expr(*field.value, /*consume_top=*/true);
        }
      }
      return;
    }
    case ast::node_kind::lambda_expr:
      // A lambda is only ever lowered inline, as part of lowering its
      // enclosing function (`lowerer::lower_lambda`'s sole call site is
      // `lower_expr`'s own `lambda_expr` case) — so its body gets its own
      // nested scope in the *same* walk and the *same* `schedule_`, exactly
      // mirroring `move_checker::check_lambda`'s recursive call.
      walk_lambda_body(dynamic_cast<const ast::lambda_expr &>(expr));
      return;
    case ast::node_kind::if_expr: {
      const auto &if_e = dynamic_cast<const ast::if_expr &>(expr);
      walk_if(if_e.branches, if_e.else_body);
      return;
    }
    case ast::node_kind::match_expr: {
      const auto &match_e = dynamic_cast<const ast::match_expr &>(expr);
      if (match_e.subject != nullptr) {
        walk_expr(*match_e.subject, /*consume_top=*/true);
      }
      walk_match_arms(match_e.arms);
      return;
    }
    case ast::node_kind::block_expr: {
      const auto &block = dynamic_cast<const ast::block_expr &>(expr);
      walk_nested_block(block.stmts);
      return;
    }
    default:
      return;
    }
  }

  auto walk_body(const std::vector<ast::ptr<ast::node>> &stmts) -> void {
    for (const auto &stmt : stmts) {
      if (stmt != nullptr) {
        walk_stmt(*stmt);
      }
      if (diverged_) {
        break;
      }
    }
  }

  /// Pushes a fresh (non-loop) scope, walks `stmts`, and pops it with `key`
  /// = `&stmts` — the same address `hir::lowerer::lower_block` will later
  /// receive this exact vector by reference at.
  auto walk_nested_block(const std::vector<ast::ptr<ast::node>> &stmts)
      -> void {
    push_scope();
    walk_body(stmts);
    pop_scope(&stmts);
  }

  /// One live branch's ending state, snapshotted for the OR-merge below.
  struct branch_result {
    std::vector<scope_frame> scopes;
  };

  /// OR-merge: a binding reads as moved after the construct if *any* live
  /// branch moved it — see the class doc comment for why this is the
  /// opposite of `move_checker::merge_states`.
  static auto merge_scopes(std::vector<branch_result> results,
                           std::vector<scope_frame> base)
      -> std::vector<scope_frame> {
    for (auto &result : results) {
      for (size_t s = 0; s < base.size() && s < result.scopes.size(); ++s) {
        for (size_t l = 0;
             l < base[s].locals.size() && l < result.scopes[s].locals.size();
             ++l) {
          if (result.scopes[s].locals[l].moved) {
            base[s].locals[l].moved = true;
          }
        }
      }
    }
    return base;
  }

  auto walk_if(const std::vector<ast::if_branch> &branches,
               const std::vector<ast::ptr<ast::node>> &else_body) -> void {
    auto results = std::vector<branch_result>{};
    const auto chain_state = scopes_;
    for (const auto &branch : branches) {
      scopes_ = chain_state;
      if (branch.condition != nullptr) {
        walk_expr(*branch.condition, /*consume_top=*/true);
      }
      diverged_ = false;
      walk_nested_block(branch.body);
      if (!diverged_) {
        results.push_back(branch_result{.scopes = scopes_});
      }
    }
    scopes_ = chain_state;
    if (!else_body.empty()) {
      diverged_ = false;
      walk_nested_block(else_body);
      if (!diverged_) {
        results.push_back(branch_result{.scopes = scopes_});
      }
    } else {
      results.push_back(branch_result{.scopes = chain_state});
    }
    if (results.empty()) {
      diverged_ = true;
      scopes_ = chain_state;
      return;
    }
    scopes_ = merge_scopes(std::move(results), chain_state);
    diverged_ = false;
  }

  auto walk_match_arms(const std::vector<ast::match_arm> &arms) -> void {
    auto results = std::vector<branch_result>{};
    const auto chain_state = scopes_;
    for (const auto &arm : arms) {
      scopes_ = chain_state;
      // Pattern-bound names are not tracked (see class doc comment); only
      // the guard and body are walked for outer-binding consume tracking.
      if (arm.guard != nullptr) {
        walk_expr(*arm.guard, /*consume_top=*/true);
      }
      diverged_ = false;
      if (arm.body_expr != nullptr) {
        walk_expr(*arm.body_expr, /*consume_top=*/true);
      } else {
        walk_nested_block(arm.body_stmts);
      }
      if (!diverged_) {
        results.push_back(branch_result{.scopes = scopes_});
      }
    }
    if (results.empty()) {
      diverged_ = true;
      scopes_ = chain_state;
      return;
    }
    scopes_ = merge_scopes(std::move(results), chain_state);
    diverged_ = false;
  }

  /// Records every scope's still-live droppable bindings, innermost first,
  /// for a `return`/`break`/`continue` at `node` — `stop_at_loop_boundary`
  /// is false for `return` (drains back to the function/lambda's own top
  /// scope) and true for `break`/`continue` (stops at, and includes, the
  /// nearest enclosing loop's body scope).
  auto record_jump(const ast::node &node, bool stop_at_loop_boundary) -> void {
    auto groups = std::vector<std::vector<pending_drop>>{};
    for (const auto &frame : std::views::reverse(scopes_)) {
      auto group = std::vector<pending_drop>{};
      for (const auto &local : std::views::reverse(frame.locals)) {
        if (!local.moved) {
          group.push_back(pending_drop{.name = local.name, .type = local.type});
        }
      }
      groups.push_back(std::move(group));
      if (stop_at_loop_boundary && frame.is_loop_boundary) {
        break;
      }
    }
    schedule_.jump_exit[&node] = std::move(groups);
  }

  auto walk_stmt(const ast::node &node) -> void {
    if (node.has_error) {
      return;
    }
    switch (node.kind) {
    case ast::node_kind::let_stmt: {
      const auto &stmt = dynamic_cast<const ast::let_stmt &>(node);
      if (stmt.initializer != nullptr) {
        walk_expr(*stmt.initializer, /*consume_top=*/true);
      }
      if (!stmt.else_body.empty()) {
        auto chain_state = scopes_;
        diverged_ = false;
        walk_nested_block(stmt.else_body);
        // `let ... else` requires the else body to diverge; if it somehow
        // doesn't, fall back to the pre-else state rather than guess.
        if (!diverged_) {
          scopes_ = chain_state;
        }
        diverged_ = false;
      }
      if (stmt.pattern != nullptr &&
          stmt.pattern->kind == ast::node_kind::binding_pattern) {
        const auto &binding =
            dynamic_cast<const ast::binding_pattern &>(*stmt.pattern);
        declare(binding.name, lookup_type(stmt.pattern.get()));
      }
      return;
    }
    case ast::node_kind::var_stmt: {
      const auto &stmt = dynamic_cast<const ast::var_stmt &>(node);
      if (stmt.initializer != nullptr) {
        walk_expr(*stmt.initializer, /*consume_top=*/true);
      }
      declare(stmt.name, lookup_type(&stmt));
      return;
    }
    case ast::node_kind::assign_stmt: {
      const auto &stmt = dynamic_cast<const ast::assign_stmt &>(node);
      if (stmt.value != nullptr) {
        walk_expr(*stmt.value, /*consume_top=*/true);
      }
      if (stmt.target != nullptr) {
        if (stmt.target->kind == ast::node_kind::ident_expr) {
          reset(dynamic_cast<const ast::ident_expr &>(*stmt.target).name);
        } else {
          walk_expr(*stmt.target, /*consume_top=*/false);
        }
      }
      return;
    }
    case ast::node_kind::expr_stmt: {
      const auto &stmt = dynamic_cast<const ast::expr_stmt &>(node);
      if (stmt.expr != nullptr) {
        walk_expr(*stmt.expr, /*consume_top=*/true);
      }
      return;
    }
    case ast::node_kind::return_stmt: {
      const auto &stmt = dynamic_cast<const ast::return_stmt &>(node);
      if (stmt.value != nullptr) {
        walk_expr(*stmt.value, /*consume_top=*/true);
      }
      record_jump(node, /*stop_at_loop_boundary=*/false);
      diverged_ = true;
      return;
    }
    case ast::node_kind::break_stmt:
    case ast::node_kind::continue_stmt: {
      record_jump(node, /*stop_at_loop_boundary=*/true);
      diverged_ = true;
      return;
    }
    case ast::node_kind::if_stmt: {
      const auto &stmt = dynamic_cast<const ast::if_stmt &>(node);
      walk_if(stmt.branches, stmt.else_body);
      return;
    }
    case ast::node_kind::while_stmt: {
      const auto &stmt = dynamic_cast<const ast::while_stmt &>(node);
      if (stmt.condition != nullptr) {
        walk_expr(*stmt.condition, /*consume_top=*/true);
      }
      if (stmt.let_expr != nullptr) {
        walk_expr(*stmt.let_expr, /*consume_top=*/true);
      }
      // Pattern-bound `while let` names are not tracked, matching `match`.
      push_scope(/*is_loop_boundary=*/true);
      walk_body(stmt.body);
      pop_scope(&stmt.body);
      diverged_ = false;
      return;
    }
    case ast::node_kind::for_stmt: {
      const auto &stmt = dynamic_cast<const ast::for_stmt &>(node);
      if (stmt.iterable != nullptr) {
        walk_expr(*stmt.iterable, /*consume_top=*/true);
      }
      // Loop-variable patterns are not tracked, matching `match`/`while let`.
      push_scope(/*is_loop_boundary=*/true);
      if (stmt.guard != nullptr) {
        walk_expr(*stmt.guard, /*consume_top=*/true);
      }
      walk_body(stmt.body);
      pop_scope(&stmt.body);
      diverged_ = false;
      return;
    }
    case ast::node_kind::match_stmt: {
      const auto &stmt = dynamic_cast<const ast::match_stmt &>(node);
      if (stmt.subject != nullptr) {
        walk_expr(*stmt.subject, /*consume_top=*/true);
      }
      walk_match_arms(stmt.arms);
      return;
    }
    case ast::node_kind::block_expr: {
      const auto &block = dynamic_cast<const ast::block_expr &>(node);
      walk_nested_block(block.stmts);
      return;
    }
    default:
      if (const auto *expr = dynamic_cast<const ast::expr *>(&node)) {
        walk_expr(*expr, /*consume_top=*/true);
      }
      return;
    }
  }
};

} // namespace

auto compute_drop_schedule(const ast::func_decl &decl,
                           const semantic::checked_types &checked)
    -> drop_schedule {
  auto walker = drop_walker(checked);
  walker.walk_function(decl);
  return walker.take_schedule();
}

} // namespace kira::hir
