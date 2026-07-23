#include "move_check.h"

#include <format>
#include <ranges>
#include <string>
#include <unordered_map>

#include "src/semantic/binding_walk.h"

namespace kira::semantic {
namespace {

/// Per-binding move state tracked while walking one function/lambda body.
struct binding_state {
  source_span declared_span;
  source_span moved_at = source_span::dummy();
  bool moved = false;
  /// False for builtin scalar/boolean/unit types and for bindings whose
  /// type was never resolved (see the header's `checked.node_types` note)
  /// — such a binding is bound but never flagged, since it either can't
  /// meaningfully move (a scalar) or the checker has nothing reliable to
  /// reason about (an unresolved type).
  bool trackable = false;
};

/// Walks one function or lambda body, tracking per-binding move state in a
/// stack of lexical scopes mirroring the body's block structure.
class move_checker {
public:
  move_checker(const checked_types &checked, diagnostic_bag &diag,
               file_id_type file_id)
      : checked_(checked), diag_(diag), file_id_(file_id) {}

  auto check_function(const ast::func_decl &decl) -> void {
    push_scope();
    for (const auto &param : decl.params) {
      if (param.pattern != nullptr) {
        bind_pattern(*param.pattern);
      }
    }
    if (decl.body_expr != nullptr) {
      walk_expr(*decl.body_expr, /*consume_top=*/true);
    }
    walk_body(decl.body_stmts);
    pop_scope();
  }

  auto check_lambda(const ast::lambda_expr &lambda) -> void {
    push_scope();
    for (const auto &param : lambda.params) {
      if (const auto *pat =
              dynamic_cast<const ast::pattern *>(param.pattern.get())) {
        bind_pattern(*pat);
      }
    }
    if (lambda.body_expr != nullptr) {
      walk_expr(*lambda.body_expr, /*consume_top=*/true);
    }
    walk_body(lambda.body_stmts);
    pop_scope();
  }

private:
  using scope_stack = std::vector<std::unordered_map<std::string, binding_state>>;

  const checked_types &checked_;
  diagnostic_bag &diag_;
  file_id_type file_id_;
  scope_stack scopes_;
  /// Set once the statement just walked unconditionally leaves the enclosing
  /// block (`return`/`break`/`continue`): the rest of that block is dead
  /// code, and — critically — an `if`/`match` branch that ends this way
  /// contributes no state to what the construct's *fallthrough* looks like,
  /// since control never reaches it from that branch. Cleared back to false
  /// after a `while`/`for` body, since the loop itself may run zero times or
  /// exit via its condition, so code after the loop is reachable regardless
  /// of whether the body's last iteration diverged.
  bool diverged_ = false;

  auto push_scope() -> void { scopes_.emplace_back(); }
  auto pop_scope() -> void { scopes_.pop_back(); }

  auto lookup_type(const ast::node *node) const -> type_id {
    if (node == nullptr) {
      return k_unknown_type;
    }
    if (const auto it = checked_.node_types.find(node);
        it != checked_.node_types.end()) {
      return it->second;
    }
    return k_unknown_type;
  }

  [[nodiscard]] auto is_trackable(type_id id) const -> bool {
    const auto &types = checked_.types;
    if (types.is_unknown(id)) {
      return false;
    }
    return !types.is_boolean(id) && !types.is_numeric(id) && !types.is_unit(id);
  }

  /// A parameter's bound name if it is a simple identifier pattern, or empty
  /// for a destructuring pattern — mirrors `checker::param_name_of`
  /// (`check.cpp`), which this pass can't reach directly (that helper lives
  /// inside the checker's anonymous-namespace class).
  [[nodiscard]] static auto param_name_of(const ast::param &param)
      -> std::string {
    if (param.pattern != nullptr &&
        param.pattern->kind == ast::node_kind::binding_pattern) {
      return dynamic_cast<const ast::binding_pattern &>(*param.pattern).name;
    }
    return {};
  }

  /// Whether a `receiver.method(...)` call moves `receiver`, the same way a
  /// plain by-value call argument would. True only when the call resolved to
  /// a real declaration (`checked_.resolved_callees`) whose first parameter
  /// is neither a `self` receiver (methods always take `self` by reference,
  /// regardless of `mut` — see `impl iterator`'s `next(mut self)`, called
  /// repeatedly on the same binding throughout `std.algo`) nor an explicit
  /// `&`/`&mut` reference. A first parameter with an ordinary name and a
  /// plain (non-reference) type — `for_each[I, T](it: I, ...)` in
  /// `std.algo`, e.g. — moves its receiver exactly like any other by-value
  /// parameter: calling `nv.for_each(...)` and then reusing `nv` is a
  /// use-after-move, the same as passing `nv` twice as a plain argument
  /// would be. An unresolved call (callee not in `resolved_callees`, e.g. a
  /// call through a plain `fn(...)`-typed value) is conservatively treated
  /// as not moving, matching this pass's general policy of never guessing at
  /// a type it can't look up.
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
    if (checked_.types.is_unknown(param_type)) {
      return false;
    }
    return checked_.types.entry(param_type).kind != type_kind::ref_kind;
  }

  /// A call's callee is walked specially rather than always through the
  /// generic `walk_expr(..., consume_top=false)` path used for a plain field
  /// access: when the callee is `receiver.method` and the resolved method
  /// takes its receiver by value (see `receiver_is_moved`), the receiver is
  /// consumed as a call argument would be. Otherwise this falls back to the
  /// ordinary field-projection treatment — a plain function name callee, or
  /// a method call that only borrows its receiver.
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

  auto declare(std::string_view name, source_span span, type_id type) -> void {
    if (name.empty() || scopes_.empty()) {
      return;
    }
    scopes_.back().insert_or_assign(
        std::string(name), binding_state{.declared_span = span,
                                         .moved_at = source_span::dummy(),
                                         .moved = false,
                                         .trackable = is_trackable(type)});
  }

  auto bind_pattern(const ast::pattern &pattern) -> void {
    for (const auto &binding : collect_pattern_bindings(pattern)) {
      declare(binding.name, binding.span, lookup_type(binding.node));
    }
  }

  auto find(std::string_view name) -> binding_state * {
    for (auto &scope : std::ranges::reverse_view(scopes_)) {
      if (const auto it = scope.find(std::string(name)); it != scope.end()) {
        return &it->second;
      }
    }
    return nullptr;
  }

  /// Reassigning a name gives it a fresh value — un-moves it.
  auto reset(std::string_view name) -> void {
    if (auto *state = find(name)) {
      state->moved = false;
    }
  }

  auto report_use_after_move(const ast::ident_expr &ident,
                             const binding_state &state) -> void {
    auto d = diagnostic(diagnostic_level::error,
                        std::format("use of moved value `{}`", ident.name),
                        file_id_);
    d.with_label(ident.span,
                 std::format("`{}` used here after being moved", ident.name));
    d.with_secondary_label(state.moved_at,
                           std::format("`{}` moved here", ident.name));
    d.with_note(
        "a value's owner may use it once more before it goes out of scope; "
        "moving it transfers that ownership away, and Kira does not "
        "implicitly copy non-scalar values");
    d.with_help(std::format(
        "borrow it instead with `&{0}` (or `&mut {0}`) if the callee only "
        "needs to read or modify it, or restructure the code so `{0}` is "
        "only used once",
        ident.name));
    diag_.emit(d);
  }

  /// A read of `ident` that does not consume it — fires use-after-move but
  /// never changes move state.
  auto touch(const ast::ident_expr &ident) -> void {
    if (const auto *state = find(ident.name);
        state != nullptr && state->trackable && state->moved) {
      report_use_after_move(ident, *state);
    }
  }

  /// A use of `ident` that consumes it (moves it, if not already moved).
  auto consume(const ast::ident_expr &ident) -> void {
    auto *state = find(ident.name);
    if (state == nullptr || !state->trackable) {
      return;
    }
    if (state->moved) {
      report_use_after_move(ident, *state);
      return;
    }
    state->moved = true;
    state->moved_at = ident.span;
  }

  /// Walks `expr` for identifier uses. `consume_top` is whether a bare
  /// identifier found at this exact expression position (not nested inside
  /// a borrow or projection) should move — false for read-only contexts
  /// (borrow operands, projection bases, callees), true everywhere
  /// ownership would transfer (initializers, return values, call
  /// arguments, operands of most operators).
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
      // `&x`/`&mut x` never consume `x`, same as any other borrow. `*p`
      // projects through the pointer to reach its pointee, the same way
      // `x.field` projects through `x` — it never consumes `p` itself,
      // regardless of what the dereferenced value is then used for.
      const auto is_borrow = unary.op == ast::unary_op::addr_of ||
                             unary.op == ast::unary_op::addr_of_mut;
      const auto is_projection = unary.op == ast::unary_op::deref;
      walk_expr(*unary.operand, (is_borrow || is_projection) ? false : consume_top);
      return;
    }

    case ast::node_kind::field_expr: {
      // `x.field` projects through `x` without consuming the whole
      // binding; partial (field-level) moves aren't tracked yet.
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
      // Operators dispatch to a trait method under the hood (`a + b` is
      // `a.add(b)`), but unlike a real call, neither operand's spelling
      // looks like an ownership transfer the way a plain call argument
      // does — so neither is treated as consumed, unlike a `receiver.method`
      // call whose resolved method takes its receiver by value (see
      // `walk_callee`, used from `call_expr` below).
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
      if (array.fill_count != nullptr) {
        walk_expr(*array.fill_count, /*consume_top=*/true);
      }
      return;
    }

    case ast::node_kind::struct_expr: {
      const auto &literal = dynamic_cast<const ast::struct_expr &>(expr);
      if (literal.type_name != nullptr) {
        walk_expr(*literal.type_name, /*consume_top=*/false);
      }
      for (const auto &field : literal.fields) {
        if (field.value != nullptr) {
          walk_expr(*field.value, /*consume_top=*/true);
        }
      }
      return;
    }

    case ast::node_kind::lambda_expr:
      // A nested closure gets its own binding scope; see the header's
      // capture-mode caveat for why every identifier inside it is treated
      // like an ordinary use rather than borrow-vs-move per its capture
      // kind.
      check_lambda(dynamic_cast<const ast::lambda_expr &>(expr));
      return;

    case ast::node_kind::if_expr: {
      const auto &if_e = dynamic_cast<const ast::if_expr &>(expr);
      walk_if_branches(if_e.branches, if_e.else_body);
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
      push_scope();
      walk_body(block.stmts);
      pop_scope();
      return;
    }

    default:
      // Literals, module paths, and constructs not yet special-cased here
      // (async/crew/on/where expressions, ...) have no top-level bare
      // identifier of interest; nested identifiers inside those aren't
      // walked in this first cut.
      return;
    }
  }

  auto walk_body(const std::vector<ast::ptr<ast::node>> &stmts) -> void {
    for (const auto &stmt : stmts) {
      if (stmt != nullptr) {
        walk_stmt(*stmt);
      }
      if (diverged_) {
        // Everything after a `return`/`break`/`continue` in this block is
        // unreachable; walking it would attribute moves to a path that
        // never executes.
        break;
      }
    }
  }

  /// Merges the end states of every branch of an `if`/`match` that can
  /// actually fall through (i.e. did not end in `return`/`break`/`continue`)
  /// into one state: a binding reads as moved afterward only if every
  /// surviving branch moved it. All entries share the same outer-scope
  /// shape, since each branch pushes and pops exactly its own scope.
  static auto merge_states(const std::vector<scope_stack> &results)
      -> scope_stack {
    auto merged = results.front();
    for (size_t i = 1; i < results.size(); ++i) {
      const auto &other = results[i];
      for (size_t s = 0; s < merged.size() && s < other.size(); ++s) {
        for (auto &[name, state] : merged[s]) {
          const auto it = other[s].find(name);
          if (it != other[s].end() && !it->second.moved) {
            state.moved = false;
          }
        }
      }
    }
    return merged;
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
        push_scope();
        walk_body(stmt.else_body);
        pop_scope();
      }
      if (stmt.pattern != nullptr) {
        bind_pattern(*stmt.pattern);
      }
      return;
    }

    case ast::node_kind::var_stmt: {
      const auto &stmt = dynamic_cast<const ast::var_stmt &>(node);
      if (stmt.initializer != nullptr) {
        walk_expr(*stmt.initializer, /*consume_top=*/true);
      }
      declare(stmt.name, stmt.span, lookup_type(&stmt));
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
      diverged_ = true;
      return;
    }

    case ast::node_kind::break_stmt:
    case ast::node_kind::continue_stmt: {
      diverged_ = true;
      return;
    }

    case ast::node_kind::if_stmt: {
      const auto &stmt = dynamic_cast<const ast::if_stmt &>(node);
      walk_if_branches(stmt.branches, stmt.else_body);
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
      push_scope();
      if (stmt.let_pattern != nullptr) {
        bind_pattern(*stmt.let_pattern);
      }
      walk_body(stmt.body);
      pop_scope();
      // The loop may run zero times, or exit normally once its condition
      // goes false — either way code after it is reachable regardless of
      // how the last-walked body iteration ended.
      diverged_ = false;
      return;
    }

    case ast::node_kind::for_stmt: {
      const auto &stmt = dynamic_cast<const ast::for_stmt &>(node);
      if (stmt.iterable != nullptr) {
        walk_expr(*stmt.iterable, /*consume_top=*/true);
      }
      push_scope();
      for (const auto &pattern : stmt.patterns) {
        if (pattern != nullptr) {
          bind_pattern(*pattern);
        }
      }
      if (stmt.guard != nullptr) {
        walk_expr(*stmt.guard, /*consume_top=*/true);
      }
      walk_body(stmt.body);
      pop_scope();
      // The source iterator may be exhausted immediately, so as with
      // `while`, code after the loop is reachable no matter how the body's
      // last-walked iteration ended.
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
      push_scope();
      walk_body(block.stmts);
      pop_scope();
      return;
    }

    default:
      if (const auto *expr = dynamic_cast<const ast::expr *>(&node)) {
        walk_expr(*expr, /*consume_top=*/true);
      }
      return;
    }
  }

  /// Conditions run in sequence — each one only reached once every prior
  /// condition was false — so their effect on state chains from one branch
  /// to the next. Each branch's *body*, in contrast, is one of several
  /// alternatives: it forks off that chained state, and its outcome only
  /// feeds into the merged state after the whole construct, not into the
  /// next condition.
  auto walk_if_branches(const std::vector<ast::if_branch> &branches,
                        const std::vector<ast::ptr<ast::node>> &else_body)
      -> void {
    auto live_results = std::vector<scope_stack>{};
    for (const auto &branch : branches) {
      if (branch.condition != nullptr) {
        walk_expr(*branch.condition, /*consume_top=*/true);
      }
      if (branch.let_expr != nullptr) {
        walk_expr(*branch.let_expr, /*consume_top=*/true);
      }
      const auto chain_state = scopes_;
      push_scope();
      if (const auto *pat =
              dynamic_cast<const ast::pattern *>(branch.let_pattern.get())) {
        bind_pattern(*pat);
      }
      diverged_ = false;
      walk_body(branch.body);
      pop_scope();
      if (!diverged_) {
        live_results.push_back(scopes_);
      }
      scopes_ = chain_state;
    }
    if (!else_body.empty()) {
      push_scope();
      diverged_ = false;
      walk_body(else_body);
      pop_scope();
      if (!diverged_) {
        live_results.push_back(scopes_);
      }
    } else {
      // No `else`: the implicit "no branch taken" path leaves state exactly
      // as it was once every condition had evaluated false.
      live_results.push_back(scopes_);
    }

    if (live_results.empty()) {
      // Every branch diverges, so this whole if/else diverges too.
      diverged_ = true;
      return;
    }
    scopes_ = merge_states(live_results);
    diverged_ = false;
  }

  /// Match arms are checked exhaustive upstream, so — unlike `if` — there is
  /// no implicit "no arm taken" path to fold into the merge. Guards run in
  /// sequence like `if` conditions (each only reached once every prior
  /// pattern/guard failed to match), so their effect on the outer scopes
  /// chains from one arm to the next; each arm's own pattern bindings stay
  /// local and never enter that chain.
  auto walk_match_arms(const std::vector<ast::match_arm> &arms) -> void {
    auto live_results = std::vector<scope_stack>{};
    for (const auto &arm : arms) {
      push_scope();
      if (const auto *pat =
              dynamic_cast<const ast::pattern *>(arm.pattern.get())) {
        bind_pattern(*pat);
      }
      if (arm.guard != nullptr) {
        walk_expr(*arm.guard, /*consume_top=*/true);
      }
      auto arm_scope = std::move(scopes_.back());
      scopes_.pop_back();
      const auto chain_state = scopes_;
      scopes_.push_back(std::move(arm_scope));

      diverged_ = false;
      if (arm.body_expr != nullptr) {
        walk_expr(*arm.body_expr, /*consume_top=*/true);
      }
      walk_body(arm.body_stmts);
      pop_scope();
      if (!diverged_) {
        live_results.push_back(scopes_);
      }
      scopes_ = chain_state;
    }

    if (live_results.empty()) {
      diverged_ = true;
      return;
    }
    scopes_ = merge_states(live_results);
    diverged_ = false;
  }
};

/// Recursively finds every `func_decl` reachable from `item` (including
/// through `impl`/`trait`/`extend`/nested-`module` bodies) and runs the move
/// checker over its body.
auto walk_item(const ast::node &item, const checked_types &checked,
               diagnostic_bag &diag, file_id_type file_id) -> void {
  switch (item.kind) {
  case ast::node_kind::func_decl: {
    const auto &decl = dynamic_cast<const ast::func_decl &>(item);
    if (decl.modifiers.is_intrinsic) {
      return;
    }
    move_checker(checked, diag, file_id).check_function(decl);
    return;
  }
  case ast::node_kind::impl_decl: {
    const auto &decl = dynamic_cast<const ast::impl_decl &>(item);
    for (const auto &child : decl.items) {
      if (child != nullptr) {
        walk_item(*child, checked, diag, file_id);
      }
    }
    return;
  }
  case ast::node_kind::trait_decl: {
    const auto &decl = dynamic_cast<const ast::trait_decl &>(item);
    for (const auto &child : decl.items) {
      if (child != nullptr) {
        walk_item(*child, checked, diag, file_id);
      }
    }
    return;
  }
  case ast::node_kind::extend_decl: {
    const auto &decl = dynamic_cast<const ast::extend_decl &>(item);
    for (const auto &child : decl.items) {
      if (child != nullptr) {
        walk_item(*child, checked, diag, file_id);
      }
    }
    return;
  }
  case ast::node_kind::sub_module_decl: {
    const auto &decl = dynamic_cast<const ast::sub_module_decl &>(item);
    for (const auto &child : decl.items) {
      if (child != nullptr) {
        walk_item(*child, checked, diag, file_id);
      }
    }
    return;
  }
  default:
    return;
  }
}

} // namespace

auto check_moves(const std::vector<parsed_module> &inputs,
                 const checked_types &checked, diagnostic_bag &diag,
                 std::vector<bool> &file_has_errors,
                 unsigned skip_from_fileid) -> void {
  for (const auto &input : inputs) {
    if (static_cast<unsigned>(input.file_id) >= skip_from_fileid) {
      continue;
    }
    if (input.ast_file == nullptr) {
      continue;
    }
    if (static_cast<size_t>(input.file_id) < file_has_errors.size() &&
        file_has_errors[input.file_id]) {
      continue;
    }
    for (const auto &item : input.ast_file->items) {
      if (item != nullptr) {
        walk_item(*item, checked, diag, input.file_id);
      }
    }
  }
}

} // namespace kira::semantic
