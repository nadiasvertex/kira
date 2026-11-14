#include "ownership_cfg.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "src/parser/ast_traversal.h"
#include "src/semantic/binding_walk.h"

namespace cinder::semantic::ownership {
namespace {

// The ownership predicates. They are file-local: the drop decisions they
// make reach `hir::lowerer` only through the CFG's records
// (`function_cfg::owning_patterns`, `owning_loops`, `loop_handles`, ...).

/// Whether the bindings of `pattern`, matched against a value of
/// `subject_type` that no expression names (such as a by-value destructuring
/// parameter), own the parts they bind, so each is dropped when its scope
/// ends: true unless the bindings overlap (`pattern_bindings_are_disjoint`)
/// or the type is unknown, a reference or a view.
[[nodiscard]] auto owns_pattern_bindings(type_id subject_type,
                                         const ast::node &pattern,
                                         const checked_types &checked) -> bool;

/// Whether the loop variables of `stmt` own the element the loop hands them,
/// so what they bind is dropped at the end of each iteration. True when the
/// loop calls `next` on an iterator or generator that yields values (not
/// references or views), including the iterator `into_iter` makes of a
/// collection (`for x in xs` consumes `xs`), and the patterns' bindings do
/// not overlap.
[[nodiscard]] auto for_variable_owns(const ast::for_stmt &stmt,
                                     const checked_types &checked) -> bool;

/// Whether a loop over `iterable` reaches `into_iter`, which consumes the
/// collection, through a reference (`for x in xs` where `xs: &list[T]`).
/// The loop then only copies the collection: nothing it yields is owned and
/// its iterator is not dropped, and the ownership checker rejects the loop
/// unless the elements are `copy`.
[[nodiscard]] auto loop_consumes_borrow(const ast::expr *iterable,
                                        const iterator_loop_dispatch *dispatch,
                                        const checked_types &checked) -> bool;

/// The type of the iterator a loop over `iterable` holds and drops, when it
/// owns one that needs a drop: what `into_iter` made of a collection, an
/// iterator or generator the loop was handed by value. `dispatch` is the
/// loop's `next` dispatch, if it has one. The ownership checker declares it
/// as a `<for iterator>` local in a scope around the loop, keyed by the
/// address of the loop's `iterable` member, so it drops after the loop and
/// with every scope a jump leaves.
[[nodiscard]] auto loop_handle_type(const ast::expr *iterable,
                                    const iterator_loop_dispatch *dispatch,
                                    const checked_types &checked)
    -> std::optional<type_id>;

/// The same question as `for_variable_owns` for one clause of a
/// comprehension.
[[nodiscard]] auto
clause_variable_owns(const ast::for_expr::iter_clause &clause,
                     const checked_types &checked) -> bool;

/// Whether matching `subject` against `patterns` (every arm of a `match`,
/// or the one pattern of a `let`, `if let` or `while let`) moves it. A
/// value that is not a place always moves. A place moves only when some
/// pattern binds a non-`copy` part of it by value; otherwise the bindings
/// are copies and the place keeps its value, to be dropped by its owner.
[[nodiscard]] auto subject_moves(const ast::expr &subject,
                                 const std::vector<const ast::node *> &patterns,
                                 const checked_types &checked) -> bool;

/// Whether `pattern`'s bindings own the parts of `subject` they bind: the
/// subject is a fresh value or a local or a field of one (not a reference
/// or view, `self`, or a global), the match moves it
/// (`subject_moves` over `patterns`), and the bindings do not overlap.
/// `is_local` says whether a name is a local of the function.
[[nodiscard]] auto
owns_pattern_bindings(const ast::expr &subject, const ast::pattern &pattern,
                      const std::vector<const ast::node *> &patterns,
                      const checked_types &checked,
                      const std::function<bool(std::string_view)> &is_local)
    -> bool;

/// Whether the arm of `pattern` owns the whole of `subject` and drops it at
/// the end of the arm: the match takes the subject's value, but the
/// pattern's bindings overlap (an `as` alias, a `|` or an array pattern), so
/// they cannot own its parts. The ownership checker rejects such a pattern
/// unless every name it binds is `copy`.
[[nodiscard]] auto
arm_owns_subject(const ast::expr &subject, const ast::pattern &pattern,
                 const std::vector<const ast::node *> &patterns,
                 const checked_types &checked,
                 const std::function<bool(std::string_view)> &is_local) -> bool;

/// The group pattern whose alias names the whole of what `pattern` matches
/// (`p as whole`), if `pattern` is one. When the arm owns the whole subject
/// (`arm_owns_subject`), this alias is its owner.
[[nodiscard]] auto owning_alias(const ast::pattern &pattern)
    -> const ast::group_pattern *;

auto owning_alias(const ast::pattern &pattern) -> const ast::group_pattern * {
  if (pattern.kind != ast::node_kind::group_pattern) {
    return nullptr;
  }
  const auto &group = ast::cast<ast::group_pattern>(pattern);
  return group.alias.has_value() ? &group : nullptr;
}

/// The borrows an evaluated expression carries: loans it made itself, plus
/// whatever the `sources` holders carry at this point.
struct value {
  std::vector<loan_id> loans;
  std::vector<local_id> sources;

  [[nodiscard]] auto empty() const -> bool {
    return loans.empty() && sources.empty();
  }
};

/// Whether a bare place in value position moves or only reads.
enum class use_mode : std::uint8_t { move, read };

/// How a call's receiver is passed to the callee.
enum class receiver_passing : std::uint8_t { move, read, shared, mut };

/// A name captured from an enclosing function by a lambda without a capture
/// list, and whether the lambda's body moves it.
struct implicit_capture {
  std::string name;
  bool moved = false;
  source_span span = source_span::dummy();
  /// The capture's first reference, when the use that noted it has one.
  const void *node = nullptr;
};

auto strip_groups(const ast::expr &expr) -> const ast::expr & {
  const auto *current = &expr;
  while (current->kind == ast::node_kind::group_expr) {
    const auto &group = ast::cast<ast::group_expr>(*current);
    if (group.inner == nullptr) {
      break;
    }
    current = group.inner.get();
  }
  return *current;
}

/// The name a place expression is rooted at.
struct root_name {
  /// A local's name, or a generic lambda copy's key (`use_name`).
  std::string name;
  source_span span = source_span::dummy();
  /// The `ident_expr` or `module_path_expr` the name was read from.
  const ast::expr *node = nullptr;
};

/// The key of `copy`, one of the copies of the generic lambda `name` binds
/// (`checked_types::generic_lambda_instances`). Each copy is a closure of
/// its own, moved and dropped on its own, so each is a local of its own
/// under this key. The `$` keeps it apart from every name a program spells.
[[nodiscard]] auto copy_key(std::string_view name,
                            const ast::lambda_expr *copy) -> std::string {
  return std::format("{}${}", name, static_cast<const void *>(copy));
}

/// The name the use `use` of `name` resolves to: the key of the copy the
/// checker made for it when `name` is a generic lambda
/// (`checked_types::generic_lambda_refs`), otherwise `name` itself.
[[nodiscard]] auto use_name(std::string_view name, const void *use,
                            const checked_types *checked) -> std::string {
  if (checked != nullptr) {
    if (const auto copy = checked->ownership.generic_lambda_refs.find(use);
        copy != checked->ownership.generic_lambda_refs.end()) {
      return copy_key(name, copy->second);
    }
  }
  return std::string(name);
}

/// The name `key` spells in a diagnostic: a copy's key without its suffix.
[[nodiscard]] auto spelled(std::string_view key) -> std::string_view {
  return key.substr(0, key.find('$'));
}

/// One step from a place's root toward the place: a field, an element, or a
/// dereference, with the type it reaches.
struct place_step {
  /// `through_deref` is an implicit `deref()` that member lookup inserted
  /// (`checked_types::deref_adjustments`): `h.x` reads `x` from
  /// `h.deref()`, so it is reached through a shared reference, but it is
  /// spelled as written.
  enum class kind : std::uint8_t { field, index, deref, through_deref };
  kind step = kind::field;
  std::string field; ///< For a field step.
  type_id type = k_unknown_type;
};

/// The name a place expression is rooted at — the `x` in `x`, `x.f`,
/// `x[i]`, `*x`, `(x).f[i]` — or nothing when the expression is not a place
/// rooted in a name (a call result, a literal, ...). `projected` is set when
/// the place is a part of the root rather than the root itself. A plain
/// dotted chain (`x.f.g`, not followed by a call or index) is parsed as a
/// `module_path_expr`; it is a place whenever its first segment names a
/// local, which the caller decides by looking the root up.
/// The checked type of what `binding` binds, or unknown.
auto binding_type(const pattern_binding &binding, const checked_types &checked)
    -> type_id {
  if (binding.field != nullptr) {
    const auto found =
        checked.lowering.struct_pattern_field_types.find(binding.field);
    return found != checked.lowering.struct_pattern_field_types.end()
               ? found->second
               : k_unknown_type;
  }
  const ast::node *node = binding.node;
  if (node == nullptr) {
    node = binding.alias_of;
  }
  if (node == nullptr) {
    return k_unknown_type;
  }
  const auto found = checked.node_types.find(node);
  return found != checked.node_types.end() ? found->second : k_unknown_type;
}

auto place_root(const ast::expr &expr, bool &projected,
                const checked_types *checked = nullptr)
    -> std::optional<root_name> {
  const auto *current = &expr;
  while (true) {
    switch (current->kind) {
    case ast::node_kind::ident_expr:
      if (current->has_error) {
        return std::nullopt;
      }
      return root_name{
          .name = use_name(ast::cast<ast::ident_expr>(*current).name, current,
                           checked),
          .span = current->span,
          .node = current};
    case ast::node_kind::module_path_expr: {
      const auto &path = ast::cast<ast::module_path_expr>(*current);
      if (path.segments.empty()) {
        return std::nullopt;
      }
      projected = projected || path.segments.size() > 1;
      return root_name{.name = std::string(path.segments.front()),
                       .span = current->span,
                       .node = current};
    }
    case ast::node_kind::field_expr: {
      const auto &field = ast::cast<ast::field_expr>(*current);
      if (field.object == nullptr) {
        return std::nullopt;
      }
      // `t.m.xs` written before `[` is a field access on the module `t.m`
      // that names a static (`checker::infer_module_static_field`). The
      // static is the root. It has no name a local could share.
      if (checked != nullptr && checked->static_refs.contains(current)) {
        return root_name{.name = {}, .span = current->span, .node = current};
      }
      projected = true;
      current = field.object.get();
      break;
    }
    case ast::node_kind::index_expr: {
      const auto &index = ast::cast<ast::index_expr>(*current);
      if (index.object == nullptr) {
        return std::nullopt;
      }
      projected = true;
      current = index.object.get();
      break;
    }
    case ast::node_kind::group_expr: {
      const auto &group = ast::cast<ast::group_expr>(*current);
      if (group.inner == nullptr) {
        return std::nullopt;
      }
      current = group.inner.get();
      break;
    }
    case ast::node_kind::unary_expr: {
      const auto &unary = ast::cast<ast::unary_expr>(*current);
      if (unary.op != ast::unary_op::deref || unary.operand == nullptr) {
        return std::nullopt;
      }
      projected = true;
      current = unary.operand.get();
      break;
    }
    default:
      return std::nullopt;
    }
  }
}

auto is_place(const ast::expr &expr) -> bool {
  auto projected = false;
  return place_root(expr, projected).has_value();
}

/// Builds one `function_cfg`. A lambda body gets its own builder whose
/// `outer_` is the enclosing function's, so names the body borrows from the
/// enclosing scope can be recorded as captures rather than as locals.
class cfg_builder {
public:
  cfg_builder(const checked_types &checked, std::vector<function_cfg> &out,
              cfg_builder *outer)
      : checked_(checked), out_(out), outer_(outer) {}

  auto build_function(const ast::func_decl &decl) -> void {
    begin(&decl);
    for (const auto &param : decl.params) {
      if (param.pattern != nullptr) {
        declare_param(*param.pattern, type_of(param.pattern.get()));
      }
    }
    in_task_ = decl.modifiers.is_async;
    if (decl.modifiers.is_generator || decl.modifiers.is_async) {
      // A generator or task dropped before it first runs returns at once.
      cancel_point(generator_start(decl));
    }
    finish(decl.body_expr.get(), decl.body_stmts);
  }

  auto build_lambda(const ast::lambda_expr &lambda) -> void {
    begin(&lambda);
    // A lambda's parameter patterns carry no type of their own when it is
    // inferred from the use site: the lambda's `fn` type does, and
    // `hir::lowerer` reads it from there too.
    const auto fn_type = type_of(&lambda);
    const auto *fn =
        fn_type != k_unknown_type &&
                checked_.types.entry(fn_type).kind == type_kind::fn_kind
            ? &checked_.types.entry(fn_type)
            : nullptr;
    for (std::size_t i = 0; i < lambda.params.size(); ++i) {
      const auto &param = lambda.params[i];
      if (param.pattern != nullptr) {
        declare_param(*param.pattern, fn != nullptr && i < fn->args.size()
                                          ? fn->args[i]
                                          : type_of(param.pattern.get()));
      }
    }
    finish(lambda.body_expr.get(), lambda.body_stmts);
  }

  /// An `async:` or `on(...)` block, keyed by `key`, as the function its
  /// task runs: the captures it owns (`task_capture::moved`) are its
  /// parameters, and the others stay names of the enclosing function.
  auto build_task_block(const ast::expr &key,
                        const std::vector<ast::ptr<ast::node>> &body,
                        const std::vector<task_capture> &captures) -> void {
    task_block_ = true;
    in_task_ = true;
    begin(&key);
    for (const auto &capture : captures) {
      if (capture.moved) {
        declare(capture.name, capture.type, key.span, /*whole=*/true,
                /*owns=*/false, capture.node);
      }
    }
    // A task dropped before it first runs returns at once.
    cancel_point(task_block_start(key));
    finish(nullptr, body);
  }

  /// Finds what a task block's body captures and which captures it
  /// consumes, without reporting anything: the graph is thrown away.
  auto probe_task_block(const ast::expr &key,
                        const std::vector<ast::ptr<ast::node>> &body) -> void {
    task_probe_ = true;
    report_captures_ = false;
    begin(&key);
    finish(nullptr, body);
  }

  [[nodiscard]] auto captures() const -> const std::vector<implicit_capture> & {
    return captures_;
  }

  /// Whether this (lambda) body changes `name`, a name it captures: a write
  /// to or into it, a `&mut` of it, or a `mut self` call on it.
  [[nodiscard]] auto writes_capture(std::string_view name) const -> bool {
    return written_captures_.contains(std::string(name));
  }

private:
  struct scope {
    /// The statement vector this scope is the body of, as
    /// `scope_exit_event::key`; null for a scope with no statements of its
    /// own (a comprehension clause); a `where` is keyed by its expression.
    const void *key = nullptr;
    std::vector<std::pair<std::string, local_id>> names;
    std::vector<local_id> owned; ///< Locals whose storage ends with the scope.
    bool is_loop = false;
    block_id continue_target = 0;
    block_id break_target = 0;
    /// For a loop, how many temporary frames were open around it: a
    /// `break` or `continue` ends the ones opened inside it.
    std::size_t temporary_depth = 0;
  };

  const checked_types &checked_;
  std::vector<function_cfg> &out_;
  cfg_builder *outer_;
  /// False in every copy of a generic lambda but the first, so an error
  /// about its captures is reported once rather than once per copy.
  bool report_captures_ = true;
  /// Set while probing a task block (`probe_task_block`): a capture the body
  /// consumes is noted as moved, since the task owns what it consumes.
  bool task_probe_ = false;
  /// Set while building a task block's own graph: a name it uses from the
  /// enclosing function is one the task only borrows, so it cannot change.
  bool task_block_ = false;
  /// Set while building a task's graph (an `async def` or a task block): a
  /// wait in it is a point where the task may be cancelled.
  bool in_task_ = false;
  function_cfg cfg_;
  std::vector<scope> scopes_;
  std::vector<implicit_capture> captures_;
  /// A generic lambda's binding, by the local of its first copy (which its
  /// plain name finds), to the locals of all its copies: a capture list
  /// naming it captures every one (`declare_generic_lambda`).
  std::unordered_map<local_id, std::vector<local_id>> generic_groups_;
  /// The captured names this (lambda) body changes (`writes_capture`).
  std::unordered_set<std::string> written_captures_;
  block_id current_ = 0;
  block_id exit_ = 0;
  local_id return_slot_ = 0;
  /// One `crew_tasks` local per enclosing `crew`, innermost last. A task
  /// spawned into the crew keeps what it borrows until the crew ends.
  std::vector<local_id> crew_holders_;
  /// The `field_path` local for each (parent, field) pair made so far.
  std::map<std::pair<local_id, std::string>, local_id> field_paths_;
  /// The temporaries made in one statement or full expression, which end
  /// with it (`close_temporaries`). `key` is its AST node.
  struct temporary_frame {
    const void *key = nullptr;
    std::vector<local_id> temps;
  };
  /// The enclosing frames, innermost last.
  std::vector<temporary_frame> temporaries_;

  // ------------------------------------------------------------------
  //  Graph construction primitives.
  // ------------------------------------------------------------------

  /// Starts the graph with the parameters' scope, keyed by the function or
  /// lambda itself (`hir::lowerer` drops parameters under that key).
  auto begin(const void *key) -> void {
    current_ = new_block();
    exit_ = new_block();
    return_slot_ = new_local("the returned value", local_role::return_slot,
                             k_unknown_type, source_span::dummy(), false);
    push_scope(key);
    temporaries_.clear();
  }

  /// Lowers the body in a scope of its own, sends its tail value to the
  /// return slot, ends the body and parameter scopes, and publishes the
  /// graph.
  auto finish(const ast::expr *body_expr,
              const std::vector<ast::ptr<ast::node>> &body_stmts) -> void {
    auto tail = value{};
    if (body_expr != nullptr) {
      tail = eval_full(*body_expr);
    }
    push_scope(&body_stmts);
    auto stmts_tail = lower_body(body_stmts, /*want_value=*/true);
    if (body_expr == nullptr) {
      tail = std::move(stmts_tail);
    }
    flow(return_slot_, tail, /*replace=*/true);
    for (auto i = scopes_.size(); i > 0; --i) {
      mark_scope_exit(scopes_[i - 1]);
    }
    end_scopes(0);
    scopes_.clear();
    goto_block(exit_);
    current_ = exit_;
    emit(use_event{.local = return_slot_});
    out_.push_back(std::move(cfg_));
  }

  /// A point where a generator may be resumed only to be cancelled: it then
  /// returns, keyed by `key`, dropping what it owns.
  auto cancel_point(const void *key) -> void {
    const auto early = new_block();
    const auto rest = new_block();
    fork(rest, early);
    current_ = early;
    emit_return(value{}, key);
    current_ = rest;
  }

  auto new_block() -> block_id {
    cfg_.blocks.emplace_back();
    return static_cast<block_id>(cfg_.blocks.size() - 1);
  }

  auto emit(event e) -> void {
    cfg_.blocks[current_].events.push_back(std::move(e));
  }

  auto goto_block(block_id target) -> void {
    cfg_.blocks[current_].successors.push_back(target);
  }

  auto fork(block_id a, block_id b) -> void {
    goto_block(a);
    goto_block(b);
  }

  /// Starts a fresh block no edge reaches — what follows a `return`,
  /// `break`, or `continue`.
  auto start_unreachable() -> void { current_ = new_block(); }

  auto new_local(std::string name, local_role role, type_id type,
                 source_span span, bool movable) -> local_id {
    cfg_.locals.push_back(local_info{.name = std::move(name),
                                     .role = role,
                                     .type = type,
                                     .span = span,
                                     .movable = movable});
    return static_cast<local_id>(cfg_.locals.size() - 1);
  }

  /// A fresh temporary. It starts out holding nothing each time evaluation
  /// reaches it — inside a loop, what it held last iteration is gone.
  auto new_temp(local_role role, std::string description = {}) -> local_id {
    const auto temp = new_local(std::move(description), role, k_unknown_type,
                                source_span::dummy(), false);
    flow(temp, value{}, /*replace=*/true);
    return temp;
  }

  auto new_loan(local_id root, bool is_mut, loan_origin origin,
                source_span span) -> loan_id {
    cfg_.loans.push_back(loan_info{
        .root = root, .is_mut = is_mut, .origin = origin, .span = span});
    return static_cast<loan_id>(cfg_.loans.size() - 1);
  }

  auto access(local_id local, access_kind kind, source_span span,
              loan_id exempt = k_no_loan, const void *node = nullptr) -> void {
    emit(access_event{.local = local,
                      .kind = kind,
                      .span = span,
                      .exempt = exempt,
                      .node = node});
  }

  /// An access to part of `local` (see `access_event::projected`).
  auto access_part_of(local_id local, access_kind kind, source_span span)
      -> void {
    emit(access_event{
        .local = local, .kind = kind, .span = span, .projected = true});
  }

  auto flow(local_id dest, const value &v, bool replace) -> void {
    if (!replace && v.empty()) {
      return;
    }
    emit(flow_event{.dest = dest,
                    .loans = v.loans,
                    .sources = v.sources,
                    .replace = replace});
  }

  /// Keeps `v`'s loans held by `temp` while the rest of an enclosing
  /// expression is evaluated.
  auto stash(local_id temp, const value &v) -> void {
    flow(temp, v, /*replace=*/false);
  }

  /// Gives a named binding its value. A binding whose type cannot carry a
  /// borrow holds none, whatever its initializer's parts did.
  auto bind_value(local_id local, const value &v) -> void {
    flow(local, bears(cfg_.locals[local].type) ? v : value{},
         /*replace=*/true);
  }

  // ------------------------------------------------------------------
  //  Types.
  // ------------------------------------------------------------------

  [[nodiscard]] auto type_of(const ast::node *node) const -> type_id {
    if (node == nullptr) {
      return k_unknown_type;
    }
    const auto it = checked_.node_types.find(node);
    return it != checked_.node_types.end() ? it->second : k_unknown_type;
  }

  /// Whether a value of `type` can carry a borrow. An unknown type might,
  /// so it is assumed to.
  [[nodiscard]] auto bears(type_id type) const -> bool {
    return checked_.types.is_unknown(type) ||
           checked_.ownership.borrow_bearing_types.contains(type);
  }

  /// The view type behind `type`, looking through references — `&mut
  /// xs[a..b]` is typed as a reference to the view it makes.
  [[nodiscard]] auto referent(type_id type) const -> type_id {
    while (!checked_.types.is_unknown(type) &&
           checked_.types.entry(type).kind == type_kind::ref_kind) {
      type = checked_.types.entry(type).result;
    }
    return type;
  }

  [[nodiscard]] auto is_view(type_id type) const -> bool {
    return checked_.types.is_view(referent(type));
  }

  [[nodiscard]] auto is_mut_view(type_id type) const -> bool {
    return checked_.types.is_mut_view(referent(type));
  }

  /// Whether moving a value of `type` transfers ownership: anything that is
  /// not `copy` (ch. 14, Copy and move), including a type parameter without
  /// a `copy` bound.
  [[nodiscard]] auto movable(type_id type) const -> bool {
    if (checked_.types.is_unknown(type)) {
      return false;
    }
    return !checked_.is_copy(type);
  }

  /// Whether a local's storage ends with its scope, so that nothing may
  /// still borrow it there. A reference or view local only points at storage
  /// owned elsewhere, and `self` is always passed by reference.
  [[nodiscard]] auto owns_storage(type_id type, std::string_view name) const
      -> bool {
    if (name == "self") {
      return false;
    }
    if (!checked_.types.is_unknown(type) &&
        checked_.types.entry(type).kind == type_kind::ref_kind) {
      return false;
    }
    return !is_view(type);
  }

  // ------------------------------------------------------------------
  //  Scopes and names.
  // ------------------------------------------------------------------

  auto push_scope(const void *key) -> void {
    scopes_.push_back(scope{.key = key});
  }

  auto push_loop_scope(block_id continue_target, block_id break_target,
                       const void *key) -> void {
    scopes_.push_back(scope{.key = key,
                            .is_loop = true,
                            .continue_target = continue_target,
                            .break_target = break_target,
                            .temporary_depth = temporaries_.size()});
  }

  /// Ends every scope from the innermost down to (and including) index
  /// `depth`, without popping them: the path leaving them is one of several.
  auto end_scopes(std::size_t depth) -> void {
    for (auto i = scopes_.size(); i > depth; --i) {
      const auto &owned = scopes_[i - 1].owned;
      for (unsigned int it : std::views::reverse(owned)) {
        access(it, access_kind::storage_dead, cfg_.locals[it].span);
      }
    }
  }

  /// The `whole` locals `s` owns storage for, in reverse declaration order.
  [[nodiscard]] auto exit_group(const scope &s) const -> std::vector<local_id> {
    auto group = std::vector<local_id>{};
    for (unsigned int it : std::views::reverse(s.owned)) {
      if (cfg_.locals[it].whole) {
        group.push_back(it);
      }
    }
    return group;
  }

  /// Marks `s` closing normally, under its own key.
  auto mark_scope_exit(const scope &s) -> void {
    if (s.key != nullptr) {
      emit(scope_exit_event{.key = s.key, .groups = {exit_group(s)}});
    }
  }

  /// Marks the early exit `jump` leaving every scope from the innermost
  /// down to (and including) index `depth`.
  auto mark_jump(const void *jump, std::size_t depth) -> void {
    auto event = scope_exit_event{.key = jump, .groups = {}};
    for (auto i = scopes_.size(); i > depth; --i) {
      event.groups.push_back(exit_group(scopes_[i - 1]));
    }
    emit(std::move(event));
  }

  auto pop_scope() -> void {
    mark_scope_exit(scopes_.back());
    end_scopes(scopes_.size() - 1);
    scopes_.pop_back();
  }

  /// Declares a binding. `owns` says it owns its value (see
  /// `local_info::owns`); a `whole` binding always does.
  auto declare(std::string name, type_id type, source_span span,
               bool whole = false, bool owns = false,
               const void *node = nullptr) -> local_id {
    const auto local =
        new_local(name, local_role::binding, type, span, movable(type));
    cfg_.locals[local].whole = whole;
    cfg_.locals[local].node = node;
    if (node != nullptr) {
      if (const auto found = checked_.immutable_pattern_bindings.find(node);
          found != checked_.immutable_pattern_bindings.end()) {
        cfg_.locals[local].immutable = true;
        cfg_.locals[local].by_pattern = !found->second;
      }
    }
    cfg_.locals[local].owns =
        (whole || owns) && name != "self" && owns_storage(type, name) &&
        (checked_.types.is_unknown(type) ||
         checked_.types.entry(type).kind != type_kind::ptr_kind);
    if (owns_storage(type, name)) {
      scopes_.back().owned.push_back(local);
    }
    scopes_.back().names.emplace_back(std::move(name), local);
    return local;
  }

  /// Records the parts of an owned `pattern` that nothing binds but that
  /// still need a drop: a `_` at a droppable position, and the droppable
  /// fields a struct pattern's `..` skips.
  auto record_leftovers(const ast::pattern &pattern) -> void {
    ast::for_each_child(pattern, [this](const ast::node &child) -> void {
      if (const auto *inner = ast::as<ast::pattern>(&child)) {
        record_leftovers(*inner);
      }
    });
    switch (pattern.kind) {
    case ast::node_kind::wildcard_pattern:
      if (checked_.drop.drop_plans.contains(type_of(&pattern))) {
        cfg_.leftover_drops.insert(&pattern);
      }
      break;
    case ast::node_kind::struct_pattern: {
      const auto &fields = ast::cast<ast::struct_pattern>(pattern).fields;
      if (!std::ranges::any_of(fields, &ast::field_pattern::is_rest)) {
        break;
      }
      const auto plan = checked_.drop.drop_plans.find(type_of(&pattern));
      if (plan == checked_.drop.drop_plans.end()) {
        break;
      }
      for (const auto &[name, type] : plan->second.droppable_fields) {
        if (!std::ranges::any_of(fields, [&](const auto &f) -> bool {
              return !f.is_rest && f.name == name;
            })) {
          cfg_.leftover_drops.insert(&pattern);
          break;
        }
      }
      break;
    }
    default:
      break;
    }
  }

  /// Declares every binding `pattern` introduces. Parameters start out
  /// holding nothing; the caller gives any other binding its value.
  auto declare_pattern(const ast::node &pattern, bool owned = false)
      -> std::vector<local_id> {
    auto locals = std::vector<local_id>{};
    const auto *pat = ast::as<ast::pattern>(&pattern);
    if (pat == nullptr) {
      return locals;
    }
    if (owned) {
      cfg_.owning_patterns.insert(&pattern);
      record_leftovers(*pat);
    }
    for (const auto &binding : collect_pattern_bindings(*pat)) {
      const auto type = binding_type(binding, checked_);
      // An owned binding that needs a drop is dropped with its scope; the
      // others are left to `storage_dead`, like every pattern binding.
      const auto whole = owned && checked_.drop.drop_plans.contains(type);
      locals.push_back(declare(binding.name, type, binding.span, whole, owned,
                               binding.key()));
    }
    return locals;
  }

  /// Declares a parameter: whole when it is a single name.
  auto declare_param(const ast::node &pattern, type_id type) -> void {
    if (pattern.kind == ast::node_kind::binding_pattern) {
      const auto &binding = ast::cast<ast::binding_pattern>(pattern);
      declare(binding.name, type, binding.span, /*whole=*/true,
              /*owns=*/false, &pattern);
      return;
    }
    // A by-value destructuring parameter owns the parts it binds.
    declare_pattern(pattern, owns_pattern_bindings(type, pattern, checked_));
  }

  /// Declares `pattern`'s bindings, each receiving what `subject` holds.
  /// `owned` says the bindings own their parts of the value (see
  /// `owns_pattern_bindings`).
  auto bind_pattern(const ast::node *pattern, local_id subject,
                    bool owned = false) -> void {
    if (pattern == nullptr) {
      return;
    }
    for (const auto local : declare_pattern(*pattern, owned)) {
      bind_value(local, value{.loans = {}, .sources = {subject}});
    }
  }

  [[nodiscard]] auto lookup(std::string_view name) const
      -> std::optional<local_id> {
    for (const auto &scope : std::views::reverse(scopes_)) {
      for (const auto &n : std::views::reverse(scope.names)) {
        if (n.first == name) {
          return n.second;
        }
      }
    }
    return std::nullopt;
  }

  /// Records that this (lambda) body uses `name` from an enclosing function.
  /// The enclosing function performs the matching access when the lambda is
  /// created — see `eval_lambda`.
  auto note_capture(std::string_view name, bool moved, source_span span,
                    const void *node = nullptr) -> void {
    if (outer_ == nullptr) {
      return;
    }
    if (moved && !task_probe_) {
      const auto type = captured_type(name);
      if (type.has_value() && movable(*type)) {
        reject_capture_move(std::string(spelled(name)), spelled(name), span);
        moved = false;
      }
    }
    for (auto &capture : captures_) {
      if (capture.name == name) {
        capture.moved = capture.moved || moved;
        if (capture.node == nullptr) {
          capture.node = node;
        }
        return;
      }
    }
    captures_.push_back(implicit_capture{
        .name = std::string(name), .moved = moved, .span = span, .node = node});
  }

  /// The type of `name` in the nearest enclosing function that declares it.
  [[nodiscard]] auto captured_type(std::string_view name) const
      -> std::optional<type_id> {
    for (const auto *builder = outer_; builder != nullptr;
         builder = builder->outer_) {
      if (const auto local = builder->lookup(name)) {
        return builder->cfg_.locals[*local].type;
      }
    }
    return std::nullopt;
  }

  /// Rejects moving `place`, part of the capture `name`, out of this
  /// closure's environment. The closure can be called again and still owns
  /// its captures, so a move would give the value two owners.
  auto reject_capture_move(std::string place, std::string_view name,
                           source_span span) -> void {
    if (!report_captures_) {
      return;
    }
    emit(invalid_move_event{.span = span,
                            .reason = move_block::captured,
                            .place = std::move(place),
                            .owner = std::string(name)});
  }

  /// Rejects `kind` through `root` when it names a `static`, and answers
  /// whether it did. A static has no local, so nothing else here sees it.
  auto reject_static_write(const root_name &root, static_write kind,
                           source_span span, std::string_view method = {})
      -> bool {
    if (root.node == nullptr) {
      return false;
    }
    const auto it = checked_.static_refs.find(root.node);
    if (it == checked_.static_refs.end()) {
      return false;
    }
    // A generic lambda's further copies would report the same write again.
    if (report_captures_) {
      emit(static_write_event{.span = span,
                              .kind = kind,
                              .name = it->second,
                              .method = std::string(method)});
    }
    return true;
  }

  /// Whether changing `place`, rooted at a local of `root_type`, changes
  /// what the local holds behind a reference, pointer, or view rather than
  /// the local itself. Writing through a `&mut` held by a `let` changes the
  /// pointee, never the binding (ch. 3). `through_place` counts `place`'s
  /// own type too: a method receiver is reached through it (`h.r.push(1)`
  /// with `r: &mut list`), while `&mut xs[a..b]` makes the view it is typed
  /// with.
  [[nodiscard]] auto through_indirection(type_id root_type,
                                         const ast::expr *place,
                                         bool through_place) const -> bool {
    const auto indirect = [this](type_id type) -> bool {
      if (checked_.types.is_unknown(type)) {
        return false;
      }
      const auto kind = checked_.types.entry(type).kind;
      return kind == type_kind::ref_kind || kind == type_kind::ptr_kind ||
             checked_.types.is_view(type);
    };
    if (indirect(root_type)) {
      return true;
    }
    if (place == nullptr) {
      return false;
    }
    auto steps = std::vector<place_step>{};
    place_steps(*place, steps);
    for (std::size_t i = 0; i < steps.size(); ++i) {
      if (steps[i].step == place_step::kind::deref ||
          steps[i].step == place_step::kind::through_deref ||
          ((through_place || i + 1 < steps.size()) &&
           indirect(steps[i].type))) {
        return true;
      }
    }
    return false;
  }

  /// The read-only indirection — a `&T`, a `*T`, or a shared view — that
  /// changing `place`, rooted at a local of `root_type`, would write
  /// through, if there is one. `through_place` is as for
  /// `through_indirection`. A `&mut` reached through a `&` is still
  /// read-only; a raw pointer is the `machine` code's to account for, so
  /// what a `*mut` points at is writable however the pointer was reached. A
  /// `*T` is left out for an assignment, which the checker rejects itself.
  [[nodiscard]] auto
  shared_indirection(type_id root_type, const ast::expr *place,
                     bool through_place, bool include_pointers) const
      -> std::optional<type_id> {
    auto shared = std::optional<type_id>{};
    const auto cross = [&](type_id type) -> void {
      if (checked_.types.is_unknown(type)) {
        return;
      }
      const auto &entry = checked_.types.entry(type);
      if (entry.kind == type_kind::ptr_kind) {
        shared = include_pointers && !entry.is_mut
                     ? std::optional<type_id>{type}
                     : std::nullopt;
      } else if (entry.kind == type_kind::ref_kind) {
        if (!entry.is_mut && !shared.has_value()) {
          shared = type;
        }
      } else if (checked_.types.is_view(type) &&
                 !checked_.types.is_mut_view(type) && !shared.has_value()) {
        shared = type;
      }
    };
    cross(root_type);
    if (place != nullptr) {
      auto steps = std::vector<place_step>{};
      place_steps(*place, steps);
      for (std::size_t i = 0; i < steps.size(); ++i) {
        if (through_place || i + 1 < steps.size() ||
            steps[i].step == place_step::kind::through_deref) {
          cross(steps[i].type);
        }
      }
    }
    return shared;
  }

  /// Rejects `kind` on `place`, rooted at `info`, when it would change
  /// something behind a read-only reference, or change `info` itself when
  /// it is a `let` binding (whose assignments the checker rejects). `method`
  /// names the called method for `receiver`; a null `place` is the binding
  /// itself.
  auto reject_place_write(const local_info &info, const ast::expr *place,
                          static_write kind, source_span span,
                          std::string_view method = {}) -> void {
    if (!report_captures_) {
      return;
    }
    if (const auto shared =
            shared_indirection(info.type, place, kind == static_write::receiver,
                               kind != static_write::assign)) {
      auto steps = std::vector<place_step>{};
      if (place != nullptr) {
        place_steps(*place, steps);
      }
      emit(shared_write_event{.span = span,
                              .kind = kind,
                              .place =
                                  spell_place(info.name, steps, steps.size()),
                              .method = std::string(method),
                              .through = checked_.types.display(*shared)});
      return;
    }
    if (kind == static_write::assign || !info.immutable ||
        through_indirection(info.type, place, kind == static_write::receiver)) {
      return;
    }
    emit(let_write_event{.span = span,
                         .kind = kind,
                         .name = info.name,
                         .method = std::string(method),
                         .declared = info.span,
                         .by_pattern = info.by_pattern});
  }

  /// The local `name` names in the nearest enclosing function, for a name
  /// this lambda body captures.
  [[nodiscard]] auto captured_local(std::string_view name) const
      -> const local_info * {
    for (const auto *builder = outer_; builder != nullptr;
         builder = builder->outer_) {
      if (const auto local = builder->lookup(name)) {
        return &builder->cfg_.locals[*local];
      }
    }
    return nullptr;
  }

  /// `reject_place_write` for a place rooted at a name this lambda body
  /// captures.
  /// Rejects `kind` on `root` inside a task block that only borrows it, and
  /// answers whether it did.
  auto reject_task_capture_write(const root_name &root, static_write kind,
                                 source_span span, std::string_view method = {})
      -> bool {
    // A task owns what its body changes, as it owns what it consumes: a
    // capture it would change is moved in. A `copy` one is not, since the
    // change would be lost on the task's copy; that write is rejected.
    if (task_probe_) {
      if (const auto type = captured_type(root.name);
          type.has_value() && movable(*type)) {
        note_capture(root.name, /*moved=*/true, span, root.node);
      }
      return false;
    }
    if (!task_block_ || !captured_type(root.name).has_value()) {
      return false;
    }
    if (report_captures_) {
      emit(task_capture_write_event{.span = span,
                                    .kind = kind,
                                    .name = std::string(spelled(root.name)),
                                    .method = std::string(method)});
    }
    return true;
  }

  auto reject_captured_write(const root_name &root, const ast::expr &place,
                             static_write kind, source_span span,
                             std::string_view method = {}) -> void {
    written_captures_.insert(root.name);
    if (const auto *info = captured_local(root.name)) {
      reject_place_write(*info, &place, kind, span, method);
    }
  }

  // ------------------------------------------------------------------
  //  Places.
  // ------------------------------------------------------------------

  /// Evaluates the subscripts along a place's projection chain, innermost
  /// first — the only parts of a place that run code.
  auto eval_subscripts(const ast::expr &expr) -> void {
    switch (expr.kind) {
    case ast::node_kind::field_expr: {
      const auto &field = ast::cast<ast::field_expr>(expr);
      if (field.object != nullptr) {
        eval_subscripts(*field.object);
      }
      return;
    }
    case ast::node_kind::index_expr: {
      const auto &index = ast::cast<ast::index_expr>(expr);
      if (index.object != nullptr) {
        eval_subscripts(*index.object);
      }
      if (index.index != nullptr) {
        static_cast<void>(eval(*index.index, use_mode::read));
      }
      return;
    }
    case ast::node_kind::group_expr: {
      const auto &group = ast::cast<ast::group_expr>(expr);
      if (group.inner != nullptr) {
        eval_subscripts(*group.inner);
      }
      return;
    }
    case ast::node_kind::unary_expr: {
      const auto &unary = ast::cast<ast::unary_expr>(expr);
      if (unary.operand != nullptr) {
        eval_subscripts(*unary.operand);
      }
      return;
    }
    default:
      return;
    }
  }

  /// The steps from `expr`'s root to `expr` (which must satisfy
  /// `is_place`), outermost last.
  auto place_steps(const ast::expr &expr, std::vector<place_step> &out) const
      -> void {
    switch (expr.kind) {
    case ast::node_kind::module_path_expr: {
      const auto &path = ast::cast<ast::module_path_expr>(expr);
      const auto types = checked_.value_path_types.find(&path);
      const auto derefs = checked_.path_deref_adjustments.find(&path);
      for (std::size_t i = 1; i < path.segments.size(); ++i) {
        const auto type =
            types != checked_.value_path_types.end() && i < types->second.size()
                ? types->second[i]
                : k_unknown_type;
        if (derefs != checked_.path_deref_adjustments.end() &&
            i < derefs->second.size()) {
          push_deref_steps(derefs->second[i], out);
        }
        out.push_back(place_step{.step = place_step::kind::field,
                                 .field = path.segments[i],
                                 .type = type});
      }
      return;
    }
    case ast::node_kind::field_expr: {
      const auto &field = ast::cast<ast::field_expr>(expr);
      place_steps(*field.object, out);
      if (const auto derefs =
              checked_.deref_adjustments.find(field.object.get());
          derefs != checked_.deref_adjustments.end()) {
        push_deref_steps(derefs->second, out);
      }
      out.push_back(place_step{.step = place_step::kind::field,
                               .field = field.field_name,
                               .type = type_of(&expr)});
      return;
    }
    case ast::node_kind::index_expr: {
      const auto &index = ast::cast<ast::index_expr>(expr);
      place_steps(*index.object, out);
      out.push_back(
          place_step{.step = place_step::kind::index, .type = type_of(&expr)});
      return;
    }
    case ast::node_kind::group_expr:
      place_steps(*ast::cast<ast::group_expr>(expr).inner, out);
      return;
    case ast::node_kind::unary_expr: {
      const auto &unary = ast::cast<ast::unary_expr>(expr);
      place_steps(*unary.operand, out);
      // `*h` on a `deref` handle reads through `h.deref()`.
      if (const auto derefs =
              checked_.deref_adjustments.find(unary.operand.get());
          derefs != checked_.deref_adjustments.end()) {
        push_deref_steps(derefs->second, out);
      }
      out.push_back(
          place_step{.step = place_step::kind::deref, .type = type_of(&expr)});
      return;
    }
    default:
      return;
    }
  }

  /// One `through_deref` step per implicit `deref()` call, each typed as
  /// the `&target` it returns.
  static auto push_deref_steps(const std::vector<deref_step> &derefs,
                               std::vector<place_step> &out) -> void {
    for (const auto &step : derefs) {
      out.push_back(place_step{.step = place_step::kind::through_deref,
                               .type = step.result});
    }
  }

  /// The `field_path` local for `root` followed by `steps`, all of which are
  /// fields; made on first use, along with every shorter path.
  auto field_path_local(local_id root, const std::vector<place_step> &steps)
      -> local_id {
    auto parent = root;
    for (const auto &step : steps) {
      const auto key = std::pair{parent, step.field};
      auto it = field_paths_.find(key);
      if (it == field_paths_.end()) {
        const auto path = new_local(
            std::format("{}.{}", cfg_.locals[parent].name, step.field),
            local_role::field_path, step.type, cfg_.locals[root].span,
            /*movable=*/true);
        cfg_.locals[path].parent = parent;
        cfg_.locals[parent].children.push_back(path);
        it = field_paths_.emplace(key, path).first;
      }
      parent = it->second;
    }
    return parent;
  }

  [[nodiscard]] auto has_own_drop(type_id type) const -> bool {
    const auto it = checked_.drop.drop_plans.find(type);
    return it != checked_.drop.drop_plans.end() &&
           it->second.own_drop.has_value();
  }

  /// A short spelling of a value that is not a place, for a diagnostic:
  /// `make()`, `a.make()`, or `(...)`.
  [[nodiscard]] static auto spell_value(const ast::expr &expr) -> std::string {
    const auto &inner = strip_groups(expr);
    switch (inner.kind) {
    case ast::node_kind::ident_expr:
      return ast::cast<ast::ident_expr>(inner).name;
    case ast::node_kind::field_expr: {
      const auto &field = ast::cast<ast::field_expr>(inner);
      return (field.object != nullptr ? spell_value(*field.object) : "") + "." +
             field.field_name;
    }
    case ast::node_kind::call_expr: {
      const auto &call = ast::cast<ast::call_expr>(inner);
      return (call.callee != nullptr ? spell_value(*call.callee) : "") + "()";
    }
    default:
      return "(...)";
    }
  }

  /// Spells `root` followed by the first `count` of `steps`.
  [[nodiscard]] auto spell_place(std::string_view root,
                                 const std::vector<place_step> &steps,
                                 std::size_t count) const -> std::string {
    auto out = std::string(root);
    for (std::size_t i = 0; i < count && i < steps.size(); ++i) {
      switch (steps[i].step) {
      case place_step::kind::field:
        out += '.';
        out += steps[i].field;
        break;
      case place_step::kind::index:
        out += "[…]";
        break;
      case place_step::kind::deref:
        out.insert(0, "*");
        break;
      case place_step::kind::through_deref:
        break;
      }
    }
    return out;
  }

  /// Why the part of `root` that `steps` reach cannot be moved out, if it
  /// cannot (ch. 14, Moving out of places).
  [[nodiscard]] auto move_blocker(local_id root,
                                  const std::vector<place_step> &steps,
                                  source_span span) const
      -> std::optional<invalid_move_event> {
    const auto &root_name = cfg_.locals[root].name;
    auto reached = cfg_.locals[root].type;
    for (std::size_t i = 0; i < steps.size(); ++i) {
      const auto through = reached;
      reached = steps[i].type;
      if (steps[i].step == place_step::kind::field) {
        continue;
      }
      if (steps[i].step == place_step::kind::through_deref) {
        return invalid_move_event{
            .span = span,
            .reason = move_block::through_deref,
            .place = spell_place(root_name, steps, steps.size()),
            .owner = checked_.types.display(through)};
      }
      // A raw read (`p[i]`, `*p`) is a bitwise copy whose ownership is the
      // `machine` code's to track (ch. 38, Raw memory and ownership).
      if (!checked_.types.is_unknown(through) &&
          checked_.types.entry(through).kind == type_kind::ptr_kind) {
        return std::nullopt;
      }
      return invalid_move_event{.span = span,
                                .reason =
                                    steps[i].step == place_step::kind::index
                                        ? move_block::element
                                        : move_block::deref,
                                .place = spell_place(root_name, steps, i + 1),
                                .owner = spell_place(root_name, steps, i)};
    }
    if (!cfg_.locals[root].owns) {
      return invalid_move_event{.span = span,
                                .reason = move_block::borrowed,
                                .place =
                                    spell_place(root_name, steps, steps.size()),
                                .owner = root_name};
    }
    auto type = cfg_.locals[root].type;
    for (std::size_t i = 0; i < steps.size(); ++i) {
      if (has_own_drop(type)) {
        return invalid_move_event{.span = span,
                                  .reason = move_block::own_drop,
                                  .place = spell_place(root_name, steps, i + 1),
                                  .owner = checked_.types.display(type)};
      }
      type = steps[i].type;
    }
    return std::nullopt;
  }

  /// Accesses the place `expr` (which must satisfy `is_place`). A move of a
  /// value that copies is a read. The result carries the root's borrows when
  /// the place's own type can hold one.
  auto access_place(const ast::expr &expr, access_kind kind,
                    bool evaluate_subscripts = true) -> value {
    auto projected = false;
    const auto root = place_root(expr, projected, &checked_);
    if (evaluate_subscripts) {
      eval_subscripts(expr);
    }
    const auto local = lookup(root->name);
    if (!local.has_value()) {
      if (projected && kind == access_kind::move && movable(type_of(&expr)) &&
          reject_static_write(*root, static_write::move, expr.span)) {
        return {};
      }
      if (!projected && kind == access_kind::move &&
          checked_.static_global_refs.contains(&expr)) {
        cfg_.owned_static_reads.insert(&expr);
      }
      if (projected && kind == access_kind::move && outer_ != nullptr &&
          !task_probe_ && captured_type(root->name).has_value() &&
          movable(type_of(&expr))) {
        auto steps = std::vector<place_step>{};
        place_steps(expr, steps);
        reject_capture_move(
            spell_place(spelled(root->name), steps, steps.size()),
            spelled(root->name), expr.span);
      }
      // A task owns what its body consumes, a part of a capture included.
      note_capture(root->name,
                   kind == access_kind::move &&
                       (!projected || (task_probe_ && movable(type_of(&expr)))),
                   root->span, root->node);
      return {};
    }
    if (!projected) {
      if (kind == access_kind::move && !cfg_.locals[*local].movable) {
        kind = access_kind::read;
      }
      access(*local, kind, expr.span, k_no_loan, &expr);
    } else {
      access_part(*local, expr, kind);
    }
    auto result = value{};
    if (bears(type_of(&expr))) {
      result.sources.push_back(*local);
    }
    return result;
  }

  /// Accesses the part of `root` that `expr` names. Moving a non-`copy` part
  /// is a partial move when every step is a field of a value `root` owns, and
  /// is rejected otherwise; any other access checks that the part (and
  /// everything above and below it) is still there.
  auto access_part(local_id root, const ast::expr &expr, access_kind kind)
      -> void {
    auto steps = std::vector<place_step>{};
    place_steps(expr, steps);
    const auto moving = kind == access_kind::move && movable(type_of(&expr));
    if (!moving && kind == access_kind::move) {
      kind = access_kind::read;
    }
    if (moving) {
      if (auto blocked = move_blocker(root, steps, expr.span)) {
        emit(std::move(*blocked));
        access_part_of(root, access_kind::read, expr.span);
        return;
      }
    }
    access_part_of(root, kind, expr.span);
    use_field_path(root, steps, moving ? access_kind::move : access_kind::read,
                   expr.span, &expr);
  }

  /// Accesses the `field_path` local for `root` followed by `steps`, when
  /// every step is a field of a value `root` owns — the only parts whose
  /// moves are tracked.
  auto use_field_path(local_id root, const std::vector<place_step> &steps,
                      access_kind kind, source_span span,
                      const void *node = nullptr) -> void {
    const auto fields_only =
        std::ranges::all_of(steps, [](const place_step &step) -> bool {
          return step.step == place_step::kind::field;
        });
    if (fields_only && !steps.empty() && cfg_.locals[root].owns) {
      access(field_path_local(root, steps), kind, span, k_no_loan, node);
    }
  }

  /// Borrows the place `expr` — or, when it is not a place, evaluates it as
  /// a temporary, which nothing else can alias.
  auto borrow_place(const ast::expr &expr, bool is_mut, loan_origin origin,
                    source_span span, loan_id exempt = k_no_loan,
                    bool evaluate_subscripts = true) -> value {
    if (!is_place(expr)) {
      return borrow_temporary(expr, is_mut, origin, span);
    }
    auto projected = false;
    const auto root = place_root(expr, projected, &checked_);
    if (evaluate_subscripts) {
      eval_subscripts(expr);
    }
    const auto local = lookup(root->name);
    if (!local.has_value()) {
      if (is_mut &&
          reject_static_write(*root, static_write::borrow_mut, span)) {
        return {};
      }
      // A receiver is checked where the call names its method.
      if (is_mut && origin != loan_origin::receiver &&
          !reject_task_capture_write(*root, static_write::borrow_mut, span)) {
        reject_captured_write(*root, expr, static_write::borrow_mut, span);
      }
      note_capture(root->name, /*moved=*/false, root->span, root->node);
      return {};
    }
    if (is_mut && origin != loan_origin::receiver) {
      reject_place_write(cfg_.locals[*local], &expr, static_write::borrow_mut,
                         span);
    }
    emit(access_event{.local = *local,
                      .kind = is_mut ? access_kind::borrow_mut
                                     : access_kind::borrow_shared,
                      .span = span,
                      .exempt = exempt,
                      .projected = projected});
    if (projected) {
      auto steps = std::vector<place_step>{};
      place_steps(expr, steps);
      use_field_path(*local, steps, access_kind::read, span);
    }
    auto result = value{};
    result.loans.push_back(new_loan(*local, is_mut, origin, span));
    if (bears(type_of(&expr))) {
      result.sources.push_back(*local);
    }
    return result;
  }

  /// Borrows `expr`, which is not a place: its value is stored in a
  /// temporary that lives until the end of the enclosing statement, the
  /// point lowering drops it (`hir::make_references_explicit`). A borrow
  /// still in use after that is reported against the temporary.
  auto borrow_temporary(const ast::expr &expr, bool is_mut, loan_origin origin,
                        source_span span) -> value {
    const auto held = eval(expr, use_mode::move);
    const auto type = type_of(&expr);
    const auto temp = hold_temporary(expr, held);
    auto result = value{};
    result.loans.push_back(new_loan(temp, is_mut, origin, span));
    if (bears(type)) {
      result.sources.push_back(temp);
    }
    return result;
  }

  /// Stores `held`, the value of `expr`, in a temporary of the innermost
  /// frame, and records where that temporary ends.
  auto hold_temporary(const ast::expr &expr, const value &held) -> local_id {
    const auto temp = new_local("temporary", local_role::temporary,
                                type_of(&expr), expr.span, false);
    flow(temp, held, /*replace=*/true);
    if (temporaries_.empty()) {
      temporaries_.emplace_back();
    }
    temporaries_.back().temps.push_back(temp);
    cfg_.temporary_ends[&expr] = temporaries_.back().key;
    return temp;
  }

  auto open_temporaries(const void *key) -> void {
    temporaries_.push_back(temporary_frame{.key = key, .temps = {}});
  }

  /// Ends the temporaries of every open frame from index `depth` on,
  /// innermost first, on a path that leaves them early.
  auto end_temporaries(std::size_t depth) -> void {
    for (auto i = temporaries_.size(); i > depth; --i) {
      for (const auto temp : std::views::reverse(temporaries_[i - 1].temps)) {
        access(temp, access_kind::storage_dead, cfg_.locals[temp].span);
      }
    }
  }

  /// Ends the temporaries of the innermost frame. A value the frame produced
  /// is held across their end in a join temporary, so a value that still
  /// borrows one of them is caught where it is used.
  auto close_temporaries(const value &produced = {}) -> value {
    auto result = produced;
    if (!temporaries_.back().temps.empty() && !produced.empty()) {
      const auto hold = new_temp(local_role::join_temp);
      flow(hold, produced, /*replace=*/true);
      result = value{.loans = {}, .sources = {hold}};
    }
    for (const auto temp : std::views::reverse(temporaries_.back().temps)) {
      access(temp, access_kind::storage_dead, cfg_.locals[temp].span);
    }
    temporaries_.pop_back();
    return result;
  }

  /// Evaluates `expr` as a full expression: the temporaries it makes end
  /// with it (a lambda's or match arm's expression body).
  auto eval_full(const ast::expr &expr, use_mode mode = use_mode::move)
      -> value {
    open_temporaries(&expr);
    return close_temporaries(eval(expr, mode));
  }

  // ------------------------------------------------------------------
  //  Expressions.
  // ------------------------------------------------------------------

  auto eval_opt(const ast::expr *expr, use_mode mode) -> value {
    return expr != nullptr ? eval(*expr, mode) : value{};
  }

  /// The value of a compound expression whose parts were stashed in `temp`:
  /// the parts' borrows if the result can carry one, else nothing.
  auto compound_result(const ast::expr &expr, local_id temp) -> value {
    emit(use_event{.local = temp});
    return bears(type_of(&expr)) ? value{.loans = {}, .sources = {temp}}
                                 : value{};
  }

  /// Evaluates `expr`. Whatever its parts borrowed, a value whose type
  /// cannot carry a borrow carries none: `*r`, `s[0]`, `xs.len()` are plain
  /// values even when computing them borrowed something.
  auto eval(const ast::expr &expr, use_mode mode) -> value {
    auto result = eval_parts(expr, mode);
    return bears(type_of(&expr)) ? result : value{};
  }

  auto eval_parts(const ast::expr &expr, use_mode mode) -> value {
    if (expr.has_error) {
      return {};
    }
    switch (expr.kind) {
    case ast::node_kind::ident_expr:
    case ast::node_kind::field_expr:
    case ast::node_kind::module_path_expr:
      // A module path rooted in a module rather than a local finds no local
      // in `access_place` and has no effect here.
      if (is_place(expr)) {
        return access_place(expr, mode == use_mode::move ? access_kind::move
                                                         : access_kind::read);
      }
      if (expr.kind == ast::node_kind::field_expr) {
        return eval_field_of_value(ast::cast<ast::field_expr>(expr), mode);
      }
      return {};

    case ast::node_kind::index_expr:
      return eval_index(ast::cast<ast::index_expr>(expr), mode);

    case ast::node_kind::group_expr: {
      const auto &group = ast::cast<ast::group_expr>(expr);
      return eval_opt(group.inner.get(), mode);
    }

    case ast::node_kind::unary_expr:
      return eval_unary(ast::cast<ast::unary_expr>(expr), mode);

    case ast::node_kind::binary_expr: {
      const auto &binary = ast::cast<ast::binary_expr>(expr);
      const auto temp = new_temp(local_role::call_temp);
      stash(temp, eval_opt(binary.lhs.get(), use_mode::read));
      // The right operand of `and`/`or` runs only sometimes: a full
      // expression of its own.
      const auto short_circuits = binary.op == ast::binary_op::logical_and ||
                                  binary.op == ast::binary_op::logical_or;
      if (short_circuits && binary.rhs != nullptr) {
        stash(temp, eval_full(*binary.rhs, use_mode::read));
      } else {
        stash(temp, eval_opt(binary.rhs.get(), operand_mode(binary)));
      }
      return compound_result(expr, temp);
    }

    case ast::node_kind::cast_expr: {
      const auto &cast = ast::cast<ast::cast_expr>(expr);
      return eval_opt(cast.operand.get(), mode);
    }

    case ast::node_kind::try_expr: {
      const auto &tri = ast::cast<ast::try_expr>(expr);
      auto result = eval_opt(tri.operand.get(), mode);
      // `?` may return from the function right here, carrying the operand.
      const auto early = new_block();
      const auto rest = new_block();
      fork(rest, early);
      current_ = early;
      emit_return(result, &tri);
      current_ = rest;
      return result;
    }

    case ast::node_kind::or_else_expr:
      return lower_or_else(ast::cast<ast::or_else_expr>(expr));

    case ast::node_kind::call_expr: {
      const auto &call = ast::cast<ast::call_expr>(expr);
      auto result = eval_call(call);
      if (in_task_ && is_task_get(call)) {
        // A task waits for `t.get()`, and may be cancelled while it does.
        cancel_point(&call);
      }
      return result;
    }

    case ast::node_kind::tuple_expr: {
      const auto &tuple = ast::cast<ast::tuple_expr>(expr);
      const auto temp = new_temp(local_role::call_temp);
      for (const auto &element : tuple.elements) {
        stash(temp, eval_opt(element.get(), use_mode::move));
      }
      return compound_result(expr, temp);
    }

    case ast::node_kind::array_expr: {
      const auto &array = ast::cast<ast::array_expr>(expr);
      const auto temp = new_temp(local_role::call_temp);
      for (const auto &element : array.elements) {
        stash(temp, eval_opt(element.get(), use_mode::move));
      }
      stash(temp, eval_opt(array.fill_value.get(), use_mode::move));
      if (array.fill_value != nullptr &&
          movable(type_of(array.fill_value.get()))) {
        emit(invalid_move_event{
            .span = array.fill_value->span,
            .reason = move_block::fill,
            .place = {},
            .owner = checked_.types.display(type_of(array.fill_value.get()))});
      }
      stash(temp, eval_opt(array.fill_count.get(), use_mode::read));
      return compound_result(expr, temp);
    }

    case ast::node_kind::struct_expr: {
      const auto &literal = ast::cast<ast::struct_expr>(expr);
      const auto temp = new_temp(local_role::call_temp);
      for (const auto &field : literal.fields) {
        if (field.value != nullptr) {
          stash(temp, eval(*field.value, use_mode::move));
        } else {
          stash(temp, eval_shorthand_field(field));
        }
      }
      return compound_result(expr, temp);
    }

    case ast::node_kind::interpolated_string_expr: {
      const auto &interp = ast::cast<ast::interpolated_string_expr>(expr);
      const auto temp = new_temp(local_role::call_temp);
      for (const auto &segment : interp.segments) {
        if (segment.is_literal) {
          continue;
        }
        stash(temp, eval_opt(segment.value.get(), use_mode::read));
        if (!segment.has_spec) {
          continue;
        }
        for (const auto *dynamic :
             {&segment.spec.width, &segment.spec.precision}) {
          if (const auto *part = std::get_if<ast::ptr<ast::expr>>(dynamic);
              part != nullptr) {
            stash(temp, eval_opt(part->get(), use_mode::read));
          }
        }
      }
      return compound_result(expr, temp);
    }

    case ast::node_kind::lambda_expr: {
      const auto &lambda = ast::cast<ast::lambda_expr>(expr);
      // A generic lambda is never created itself: one closure per checked
      // copy is, right here (`checked_types::generic_lambda_instances`).
      const auto copies =
          checked_.ownership.generic_lambda_instances.find(&lambda);
      if (copies == checked_.ownership.generic_lambda_instances.end()) {
        return eval_lambda(lambda);
      }
      auto result = value{};
      auto first = true;
      for (const auto *copy : copies->second) {
        // The copies are one closure value, so only the first takes the
        // capture borrows (`eval_lambda`).
        auto made = eval_lambda(*copy, first, /*borrow=*/first);
        first = false;
        result.loans.insert(result.loans.end(), made.loans.begin(),
                            made.loans.end());
        result.sources.insert(result.sources.end(), made.sources.begin(),
                              made.sources.end());
      }
      return result;
    }

    case ast::node_kind::if_expr: {
      const auto &if_e = ast::cast<ast::if_expr>(expr);
      return lower_if(if_e.branches, if_e.else_body, /*want_value=*/true);
    }

    case ast::node_kind::match_expr: {
      const auto &match_e = ast::cast<ast::match_expr>(expr);
      return lower_match(match_e.subject.get(), match_e.arms,
                         /*want_value=*/true);
    }

    case ast::node_kind::block_expr:
      return lower_scoped_body(ast::cast<ast::block_expr>(expr).stmts,
                               /*want_value=*/true);

    case ast::node_kind::for_expr:
      return lower_comprehension(ast::cast<ast::for_expr>(expr));

    case ast::node_kind::where_expr:
      return lower_where(ast::cast<ast::where_expr>(expr));

    case ast::node_kind::await_expr: {
      const auto &await_e = ast::cast<ast::await_expr>(expr);
      auto result = eval_opt(await_e.operand.get(), use_mode::move);
      // Dropping the task while it waits here resumes it only to return.
      cancel_point(&await_e);
      return result;
    }

    case ast::node_kind::yield_expr: {
      const auto &yield_e = ast::cast<ast::yield_expr>(expr);
      if (yield_e.value != nullptr) {
        static_cast<void>(eval_full(*yield_e.value));
      }
      // Dropping the generator while it is suspended here resumes it only
      // to return.
      cancel_point(&yield_e);
      return {};
    }

    case ast::node_kind::par_expr:
    case ast::node_kind::race_expr: {
      const auto &branches = expr.kind == ast::node_kind::par_expr
                                 ? ast::cast<ast::par_expr>(expr).branches
                                 : ast::cast<ast::race_expr>(expr).branches;
      const auto temp = new_temp(local_role::call_temp);
      for (const auto &b : branches) {
        stash(temp, eval_opt(b.get(), use_mode::move));
      }
      return compound_result(expr, temp);
    }

    case ast::node_kind::async_expr:
      return eval_task_block(expr, ast::cast<ast::async_expr>(expr).body);

    case ast::node_kind::crew_expr:
      lower_crew_body(&expr, ast::cast<ast::crew_expr>(expr).body);
      return {};

    case ast::node_kind::on_expr: {
      const auto &on = ast::cast<ast::on_expr>(expr);
      if (on.sender != nullptr) {
        // `on(ctx, task)` runs a task that already exists: it moves in.
        return eval(*on.sender, use_mode::move);
      }
      return eval_task_block(expr, on.body);
    }

    case ast::node_kind::splice_expr: {
      const auto it = checked_.spliced_fragments.find(&expr);
      if (it != checked_.spliced_fragments.end()) {
        if (const auto *fragment = ast::as<ast::expr>(it->second)) {
          return eval(*fragment, mode);
        }
      }
      return {};
    }

    default:
      // Every other expression kind has no runtime effect on a local:
      // `literal_expr`, `static_expr` (compile-time
      // only), `quote_expr` (syntax, not a runtime value), and
      // `postfix_expr` (declared, never constructed).
      return {};
    }
  }

  /// `obj.f` where `obj` is not itself a place (a call result, ...).
  /// `make().f`: a field of a value that is not a place. Moving a field
  /// that is not `copy` out of it leaves the rest to be dropped with the
  /// temporary, which a type with its own `drop` does not allow.
  auto eval_field_of_value(const ast::field_expr &field, use_mode mode)
      -> value {
    if (mode == use_mode::move && movable(type_of(&field)) &&
        field.object != nullptr &&
        checked_.deref_adjustments.contains(field.object.get())) {
      emit(invalid_move_event{
          .span = field.span,
          .reason = move_block::through_deref,
          .place = spell_value(*field.object) + "." + field.field_name,
          .owner = checked_.types.display(type_of(field.object.get()))});
    } else if (mode == use_mode::move && movable(type_of(&field))) {
      auto path = "." + field.field_name;
      for (const ast::expr *at = field.object.get(); at != nullptr;) {
        if (has_own_drop(type_of(at))) {
          emit(
              invalid_move_event{.span = field.span,
                                 .reason = move_block::own_drop,
                                 .place = spell_value(*at) + path,
                                 .owner = checked_.types.display(type_of(at))});
          break;
        }
        const auto &inner = strip_groups(*at);
        if (inner.kind != ast::node_kind::field_expr) {
          break;
        }
        const auto &outer = ast::cast<ast::field_expr>(inner);
        path.insert(0, "." + outer.field_name);
        at = outer.object.get();
      }
    }
    auto object = eval_opt(field.object.get(), use_mode::read);
    if (field.object != nullptr) {
      // The object is stored so its field can be read, and the rest of it
      // drops with the temporary.
      static_cast<void>(hold_temporary(*field.object, object));
    }
    return bears(type_of(&field)) ? object : value{};
  }

  /// A struct literal's shorthand field `{x}`, which reads as `{x: x}`.
  auto eval_shorthand_field(const ast::struct_field_init &field) -> value {
    const auto name = use_name(field.name, &field, &checked_);
    const auto local = lookup(name);
    if (!local.has_value()) {
      note_capture(name, /*moved=*/true, field.span, &field);
      return {};
    }
    const auto type = cfg_.locals[*local].type;
    access(*local,
           cfg_.locals[*local].movable ? access_kind::move : access_kind::read,
           field.span, k_no_loan, &field);
    return bears(type) ? value{.loans = {}, .sources = {*local}} : value{};
  }

  /// `c[i]`: an element read, or — when the result is a `slice`/`cell` —
  /// a view that borrows `c`.
  auto eval_index(const ast::index_expr &index, use_mode mode) -> value {
    const auto type = type_of(&index);
    if (index.object != nullptr && is_place(*index.object)) {
      if (is_view(type)) {
        return borrow_place(index, is_mut_view(type), loan_origin::view,
                            index.span);
      }
      return access_place(index, mode == use_mode::move ? access_kind::move
                                                        : access_kind::read);
    }
    const auto temp = new_temp(local_role::call_temp);
    const auto object = eval_opt(index.object.get(), use_mode::read);
    if (index.object != nullptr) {
      // The object is stored so it can be indexed, and drops with the
      // temporary.
      static_cast<void>(hold_temporary(*index.object, object));
    }
    stash(temp, object);
    stash(temp, eval_opt(index.index.get(), use_mode::read));
    return compound_result(index, temp);
  }

  auto eval_unary(const ast::unary_expr &unary, use_mode mode) -> value {
    if (unary.operand == nullptr) {
      return {};
    }
    switch (unary.op) {
    case ast::unary_op::addr_of:
    case ast::unary_op::addr_of_mut: {
      const auto &operand = strip_groups(*unary.operand);
      const auto is_mut = unary.op == ast::unary_op::addr_of_mut;
      const auto origin =
          operand.kind == ast::node_kind::index_expr && is_view(type_of(&unary))
              ? loan_origin::view
              : loan_origin::borrow;
      return borrow_place(operand, is_mut, origin, unary.span);
    }
    case ast::unary_op::deref:
      if (is_place(unary)) {
        return access_place(unary, mode == use_mode::move ? access_kind::move
                                                          : access_kind::read);
      }
      return eval(*unary.operand, use_mode::read);
    case ast::unary_op::neg:
    case ast::unary_op::bit_not:
    case ast::unary_op::logical_not:
      static_cast<void>(eval(*unary.operand, use_mode::read));
      return {};
    }
    return {};
  }

  /// How `call`'s receiver reaches the callee: moved into a by-value first
  /// parameter, or lent to `self`/a `&`/`&mut` first parameter.
  [[nodiscard]] auto receiver_mode(const resolved_callee &callee) const
      -> receiver_passing {
    if (callee.decl == nullptr || callee.decl->params.empty()) {
      return receiver_passing::read;
    }
    const auto &front = callee.decl->params.front();
    const auto *binding =
        front.pattern != nullptr &&
                front.pattern->kind == ast::node_kind::binding_pattern
            ? &ast::cast<ast::binding_pattern>(*front.pattern)
            : nullptr;
    if (binding != nullptr && binding->name == "self") {
      // `self` is always taken by reference, except by `into_iterator`'s
      // `into_iter` — the one spec-documented consuming `self`.
      if (callee.trait_name == "into_iterator") {
        return receiver_passing::move;
      }
      return binding->is_mut ? receiver_passing::mut : receiver_passing::shared;
    }
    const auto type = type_of(front.pattern.get());
    if (checked_.types.is_unknown(type)) {
      return receiver_passing::read;
    }
    const auto &entry = checked_.types.entry(type);
    if (entry.kind != type_kind::ref_kind) {
      return receiver_passing::move;
    }
    return entry.is_mut ? receiver_passing::mut : receiver_passing::shared;
  }

  /// How an operator's right operand is used: moved into an overload's
  /// by-value parameter (`add(self, other: self)`), as any argument is, and
  /// only read for a built-in operator or a `&` parameter. The left operand
  /// is the overload's `self`, which is lent.
  [[nodiscard]] auto operand_mode(const ast::binary_expr &binary) const
      -> use_mode {
    const auto found = checked_.lowering.operator_dispatches.find(&binary);
    if (found == checked_.lowering.operator_dispatches.end() ||
        found->second.decl == nullptr || found->second.decl->params.size() < 2) {
      return use_mode::read;
    }
    const auto type = type_of(found->second.decl->params[1].pattern.get());
    if (checked_.types.is_unknown(type) ||
        checked_.types.entry(type).kind == type_kind::ref_kind) {
      return use_mode::read;
    }
    return use_mode::move;
  }

  [[nodiscard]] auto argument_passing(const ast::call_expr &call,
                                      const ast::expr *arg) const
      -> param_passing {
    const auto it = checked_.call_argument_mappings.find(&call);
    if (it == checked_.call_argument_mappings.end()) {
      return param_passing::by_value;
    }
    const auto &mapping = it->second;
    for (std::size_t i = 0; i < mapping.args_by_param.size() &&
                            i < mapping.passing_by_param.size();
         ++i) {
      if (mapping.args_by_param[i] == arg) {
        return mapping.passing_by_param[i];
      }
    }
    return param_passing::by_value;
  }

  /// The declaration whose signature describes a call to `decl`: its
  /// template if `decl` is a generic instance, whose signature still names
  /// the type parameters the instance's has substituted away.
  [[nodiscard]] auto signature_decl(const ast::func_decl *decl) const
      -> const ast::func_decl * {
    const auto it = checked_.ownership.instance_templates.find(decl);
    return it != checked_.ownership.instance_templates.end() ? it->second
                                                             : decl;
  }

  /// The type parameters through which a call to `decl` can hand back a
  /// borrow it was given, read off `decl`'s declared signature: a result
  /// typed `T` can only return a borrow that arrived in a value mentioning
  /// `T`. `nullopt` when any input's borrows may reach the result — the
  /// result carries a borrow of its own (`&T`, a view, a closure, an
  /// existential), or the signature is not known.
  [[nodiscard]] auto result_borrow_params(const ast::func_decl *decl) const
      -> std::optional<std::unordered_set<type_id>> {
    if (decl == nullptr) {
      return std::nullopt;
    }
    auto result = k_unknown_type;
    if (decl->return_type != nullptr) {
      result = type_of(decl->return_type.get());
    } else if (const auto implicit =
                   checked_.ownership.implicit_signatures.find(decl);
               implicit != checked_.ownership.implicit_signatures.end()) {
      result = implicit->second.result;
    } else if (const auto it = checked_.inferred_return_types.find(decl);
               it != checked_.inferred_return_types.end()) {
      result = it->second;
    }
    if (unknown_signature_type(result) || result == k_error_type ||
        checked_.ownership.own_borrow_bearing_types.contains(result)) {
      return std::nullopt;
    }
    auto params = std::unordered_set<type_id>{};
    if (!collect_type_params(result, params)) {
      return std::nullopt;
    }
    return params;
  }

  /// Whether `type`, read from a callee's signature, says nothing — unknown
  /// outright. An implicit generic's leaf is a parameter, not unknown.
  [[nodiscard]] auto unknown_signature_type(type_id type) const -> bool {
    return checked_.types.is_unknown(type) &&
           checked_.types.entry(type).kind != type_kind::type_var_kind;
  }

  /// Whether `kind` is a type parameter: a declared one, or an implicit
  /// generic's leaf (`implicit_signature`).
  [[nodiscard]] static auto is_type_parameter(type_kind kind) -> bool {
    return kind == type_kind::type_param_kind ||
           kind == type_kind::type_var_kind;
  }

  /// Adds every type parameter `type` mentions to `params`; false if `type`
  /// has a part whose borrows cannot be traced to a type parameter.
  [[nodiscard]] auto
  collect_type_params(type_id type, std::unordered_set<type_id> &params) const
      -> bool {
    if (unknown_signature_type(type)) {
      return false;
    }
    const auto &e = checked_.types.entry(type);
    if (is_type_parameter(e.kind)) {
      params.insert(type);
      return true;
    }
    if (e.kind == type_kind::param_app_kind ||
        e.kind == type_kind::existential_kind) {
      return false;
    }
    if (e.result != k_unknown_type && !collect_type_params(e.result, params)) {
      return false;
    }
    return std::ranges::all_of(
        e.args, [&](type_id arg) { return collect_type_params(arg, params); });
  }

  /// Whether a parameter of type `type` can pass a borrow on to a result
  /// that carries borrows only through `params` (`result_borrow_params`).
  [[nodiscard]] auto
  reaches_result(type_id type, const std::unordered_set<type_id> &params) const
      -> bool {
    if (unknown_signature_type(type)) {
      return true;
    }
    const auto &e = checked_.types.entry(type);
    if (is_type_parameter(e.kind)) {
      return params.contains(type);
    }
    if (e.kind == type_kind::param_app_kind ||
        e.kind == type_kind::existential_kind) {
      return true;
    }
    if (e.result != k_unknown_type && reaches_result(e.result, params)) {
      return true;
    }
    return std::ranges::any_of(
        e.args, [&](type_id arg) { return reaches_result(arg, params); });
  }

  /// Whether `decl`'s parameter named `name` can pass a borrow on to the
  /// call's result; true when the callee's signature does not say.
  [[nodiscard]] auto param_reaches_result(
      const ast::func_decl *decl, std::string_view name,
      const std::optional<std::unordered_set<type_id>> &flow) const -> bool {
    if (!flow.has_value()) {
      return true;
    }
    const auto implicit = checked_.ownership.implicit_signatures.find(decl);
    for (std::size_t i = 0; i < decl->params.size(); ++i) {
      const auto &param = decl->params[i];
      if (param.pattern == nullptr ||
          param.pattern->kind != ast::node_kind::binding_pattern ||
          ast::cast<ast::binding_pattern>(*param.pattern).name != name) {
        continue;
      }
      auto type = type_of(param.pattern.get());
      if (implicit != checked_.ownership.implicit_signatures.end() &&
          i < implicit->second.params.size() &&
          implicit->second.params[i] != k_unknown_type) {
        type = implicit->second.params[i];
      }
      return reaches_result(type, *flow);
    }
    return true;
  }

  /// The name of the parameter `arg` was passed to, or empty if unknown.
  [[nodiscard]] auto argument_param_name(const ast::call_expr &call,
                                         const ast::expr *arg) const
      -> std::string_view {
    const auto it = checked_.call_argument_mappings.find(&call);
    if (it == checked_.call_argument_mappings.end()) {
      return {};
    }
    const auto &mapping = it->second;
    for (std::size_t i = 0;
         i < mapping.args_by_param.size() && i < mapping.param_names.size();
         ++i) {
      if (mapping.args_by_param[i] == arg) {
        return mapping.param_names[i];
      }
    }
    return {};
  }

  /// A call: the callee and receiver, then each argument in order, then the
  /// call itself. Every borrow made along the way is held by the call's
  /// temporary until the call runs, so later arguments see it. A `mut self`
  /// (or `&mut` first-parameter) receiver is two-phase: only a shared
  /// reservation while the arguments run, activated as a real `&mut` when the
  /// call does — which lets `xs.set(i, xs.get(j))` through while
  /// `xs.set(i, take(&mut xs))` still conflicts.
  ///
  /// The result carries the borrows of only those inputs the callee's
  /// signature lets reach it (`result_borrow_params`); the rest are held by a
  /// second temporary that ends with the call.
  auto eval_call(const ast::call_expr &call) -> value {
    const auto temp = new_temp(local_role::call_temp);
    const auto held = new_temp(local_role::call_temp);
    const ast::expr *activate = nullptr;
    auto activate_temp = temp;
    auto reservation = k_no_loan;

    const auto into_crew = !crew_holders_.empty() && is_crew_spawn(call);
    const auto resolved = checked_.resolved_callees.find(&call);
    const auto *decl = signature_decl(
        resolved != checked_.resolved_callees.end() ? resolved->second.decl
                                                    : nullptr);
    const auto flow = result_borrow_params(decl);
    const auto target_of = [&](std::string_view param_name) -> local_id {
      return param_name.empty() || param_reaches_result(decl, param_name, flow)
                 ? temp
                 : held;
    };
    if (resolved != checked_.resolved_callees.end() &&
        resolved->second.receiver != nullptr) {
      const auto &receiver = *resolved->second.receiver;
      const auto receiver_temp =
          decl != nullptr && !decl->params.empty() &&
                  decl->params.front().pattern != nullptr &&
                  decl->params.front().pattern->kind ==
                      ast::node_kind::binding_pattern
              ? target_of(ast::cast<ast::binding_pattern>(
                              *decl->params.front().pattern)
                              .name)
              : temp;
      switch (receiver_mode(resolved->second)) {
      case receiver_passing::move:
        if (resolved->second.trait_name == "into_iterator") {
          cfg_.consuming_calls.insert(&call);
        }
        stash(receiver_temp, eval(receiver, use_mode::move));
        break;
      case receiver_passing::read:
        stash(receiver_temp, eval(receiver, use_mode::read));
        break;
      case receiver_passing::shared:
        stash(receiver_temp,
              borrow_place(receiver, false, loan_origin::receiver,
                           receiver.span));
        break;
      case receiver_passing::mut: {
        if (!is_place(receiver)) {
          // A temporary receiver has no name an argument could alias, so
          // it needs no two-phase reservation — and activating one would
          // evaluate the receiver expression a second time.
          stash(receiver_temp,
                borrow_temporary(receiver, true, loan_origin::receiver,
                                 receiver.span));
          break;
        }
        auto reserved =
            borrow_place(receiver, false, loan_origin::receiver, receiver.span);
        const auto method =
            decl != nullptr ? std::string_view(decl->name) : std::string_view{};
        if (!reserved.loans.empty()) {
          reservation = reserved.loans.front();
          cfg_.loans[reservation].reserved_mut = true;
          activate = &receiver;
          activate_temp = receiver_temp;
          reject_place_write(cfg_.locals[cfg_.loans[reservation].root],
                             &receiver, static_write::receiver, receiver.span,
                             method);
        } else {
          // No loan means no local: a `mut self` call on a `static`, or on
          // a name this lambda body captures.
          auto projected = false;
          if (const auto root = place_root(receiver, projected, &checked_);
              root.has_value() &&
              !reject_static_write(*root, static_write::receiver, receiver.span,
                                   method) &&
              !reject_task_capture_write(*root, static_write::receiver,
                                         receiver.span, method)) {
            reject_captured_write(*root, receiver, static_write::receiver,
                                  receiver.span, method);
          }
        }
        stash(receiver_temp, reserved);
        break;
      }
      }
    } else if (call.callee != nullptr) {
      stash(temp, eval(*call.callee, use_mode::read));
    }

    for (const auto &arg : call.args) {
      if (arg.value == nullptr) {
        continue;
      }
      const auto &argument = strip_groups(*arg.value);
      const auto passing = argument_passing(call, arg.value.get());
      const auto arg_temp =
          target_of(argument_param_name(call, arg.value.get()));
      const auto is_explicit_borrow =
          argument.kind == ast::node_kind::unary_expr &&
          (ast::cast<ast::unary_expr>(argument).op == ast::unary_op::addr_of ||
           ast::cast<ast::unary_expr>(argument).op ==
               ast::unary_op::addr_of_mut);
      if (passing != param_passing::by_value && !is_explicit_borrow &&
          is_place(argument)) {
        // Implicit autoref: a bare place passed to a `&`/`&mut` parameter.
        stash(arg_temp,
              borrow_place(argument, passing == param_passing::mut_ref,
                           loan_origin::borrow, argument.span));
      } else {
        auto passed = eval(*arg.value, use_mode::move);
        if (into_crew) {
          stash(crew_holders_.back(), passed);
        }
        stash(arg_temp, std::move(passed));
      }
    }

    if (activate != nullptr) {
      const auto activated =
          borrow_place(*activate, true, loan_origin::receiver, activate->span,
                       reservation, /*evaluate_subscripts=*/false);
      stash(activate_temp, activated);
    }
    emit(use_event{.local = held});
    return compound_result(call, temp);
  }

  /// Whether `call` is `t.get()` on a `task` (through references too).
  [[nodiscard]] auto is_task_get(const ast::call_expr &call) const -> bool {
    if (call.callee == nullptr ||
        call.callee->kind != ast::node_kind::field_expr || !call.args.empty()) {
      return false;
    }
    const auto &field = ast::cast<ast::field_expr>(*call.callee);
    if (field.field_name != "get" || field.object == nullptr) {
      return false;
    }
    auto type = type_of(field.object.get());
    while (!checked_.types.is_unknown(type) &&
           checked_.types.entry(type).kind == type_kind::ref_kind) {
      type = checked_.types.entry(type).result;
    }
    return !checked_.types.is_unknown(type) &&
           checked_.types.entry(type).kind == type_kind::builtin_generic_kind &&
           checked_.types.entry(type).name == "task";
  }

  /// Whether `call` is `c.spawn(task)` on a `crew`.
  [[nodiscard]] auto is_crew_spawn(const ast::call_expr &call) const -> bool {
    if (call.callee == nullptr ||
        call.callee->kind != ast::node_kind::field_expr ||
        call.args.size() != 1) {
      return false;
    }
    const auto &field = ast::cast<ast::field_expr>(*call.callee);
    if (field.field_name != "spawn" || field.object == nullptr) {
      return false;
    }
    const auto type = type_of(field.object.get());
    return !checked_.types.is_unknown(type) &&
           checked_.types.entry(type).name == "crew";
  }

  /// A `crew` body, keyed by `key`, whose spawned tasks all end with it. In
  /// a task the wait for them is a point where the task may be cancelled.
  auto lower_crew_body(const void *key,
                       const std::vector<ast::ptr<ast::node>> &body) -> void {
    const auto holder = new_temp(local_role::crew_tasks);
    crew_holders_.push_back(holder);
    static_cast<void>(lower_scoped_body(body, false));
    crew_holders_.pop_back();
    emit(use_event{.local = holder});
    if (in_task_) {
      cancel_point(key);
    }
  }

  /// An `async:` or `on(...)` block, `key`: its body runs as a task of its
  /// own, a function whose parameters are the block's captures. A capture
  /// the body consumes moves into the task. Any other is lent to it: the
  /// task value holds a shared loan of each non-`copy` one, so the owner
  /// cannot change, move or drop it while the task may still read it.
  /// Writing to a capture the task only borrows is rejected as it is in a
  /// closure.
  auto eval_task_block(const ast::expr &key,
                       const std::vector<ast::ptr<ast::node>> &body) -> value {
    // Which names the body uses from outside it, and which it consumes.
    auto scratch = std::vector<function_cfg>{};
    auto probe = cfg_builder(checked_, scratch, this);
    probe.probe_task_block(key, body);

    auto captures = std::vector<task_capture>{};
    for (const auto &capture : probe.captures()) {
      const auto local = lookup(capture.name);
      if (!local.has_value()) {
        // A name from further out: the enclosing closure or task captures
        // it, and this task then borrows or takes it from there.
        note_capture(capture.name, capture.moved, capture.span, capture.node);
        continue;
      }
      const auto &info = cfg_.locals[*local];
      captures.push_back(task_capture{.name = capture.name,
                                      .type = info.type,
                                      .moved = capture.moved && info.movable &&
                                               capture.node != nullptr,
                                      .node = capture.node,
                                      .span = capture.span});
    }

    auto nested = cfg_builder(checked_, out_, this);
    nested.report_captures_ = report_captures_;
    nested.build_task_block(key, body, captures);

    auto result = value{};
    for (const auto &capture : captures) {
      const auto local = *lookup(capture.name);
      const auto &info = cfg_.locals[local];
      if (capture.moved) {
        access(local, access_kind::move, capture.span, k_no_loan, &key);
      } else if (info.movable) {
        access(local, access_kind::borrow_shared, capture.span);
        result.loans.push_back(
            new_loan(local, false, loan_origin::capture, capture.span));
      } else {
        access(local, access_kind::read, capture.span);
      }
      if (bears(info.type)) {
        result.sources.push_back(local);
      }
    }
    cfg_.task_captures[&key] = std::move(captures);
    return result;
  }

  /// The locals `local` stands for in a capture list: every copy of a
  /// generic lambda, or `local` alone.
  [[nodiscard]] auto group_of(local_id local) const -> std::vector<local_id> {
    if (const auto group = generic_groups_.find(local);
        group != generic_groups_.end()) {
      return group->second;
    }
    return {local};
  }

  /// A lambda: its body is a function of its own; here, only what creating
  /// it does to the enclosing function's locals. A `&`/`&mut` capture-list
  /// entry borrows for as long as the closure lives. Everything else is
  /// captured by value — moved if the body (or `move`) consumes it, copied
  /// otherwise — and a captured value's own borrows travel with it.
  ///
  /// Without `borrow`, the closure is a later copy of a generic lambda whose
  /// first copy already took the capture borrows, and the caller hands it
  /// those loans: its by-reference captures are recorded for lowering but
  /// borrow nothing again.
  auto eval_lambda(const ast::lambda_expr &lambda, bool report_captures = true,
                   bool borrow = true) -> value {
    auto nested = cfg_builder(checked_, out_, this);
    nested.report_captures_ = report_captures_ && report_captures;
    nested.build_lambda(lambda);

    auto result = value{};
    const auto capture_by_value = [&](std::string_view name, bool moved,
                                      source_span span,
                                      const void *node) -> void {
      const auto local = lookup(name);
      if (!local.has_value()) {
        note_capture(name, moved, span, node);
        return;
      }
      auto owned = std::optional<type_id>{};
      for (const auto part : group_of(*local)) {
        const auto &info = cfg_.locals[part];
        const auto moves = moved && info.movable;
        access(part, moves ? access_kind::move : access_kind::read, span,
               k_no_loan, &lambda);
        if (moves && checked_.drop.drop_plans.contains(info.type)) {
          owned = info.type;
        }
        if (bears(info.type)) {
          result.sources.push_back(part);
        }
      }
      if (owned.has_value()) {
        cfg_.owned_captures[&lambda].push_back(owned_capture{
            .name = std::string(name), .type = *owned, .node = node});
      }
    };

    if (lambda.captures.has_value()) {
      for (const auto &entry : *lambda.captures) {
        switch (entry.mode) {
        case ast::capture_mode::by_ref:
        case ast::capture_mode::by_mut_ref: {
          const auto local = lookup(entry.name);
          if (!local.has_value()) {
            note_capture(entry.name, false, entry.span, &entry);
            continue;
          }
          const auto is_mut = entry.mode == ast::capture_mode::by_mut_ref;
          if (!borrow) {
            continue;
          }
          for (const auto part : group_of(*local)) {
            if (is_mut) {
              reject_place_write(cfg_.locals[part], nullptr,
                                 static_write::borrow_mut, entry.span);
            }
            access(part,
                   is_mut ? access_kind::borrow_mut
                          : access_kind::borrow_shared,
                   entry.span);
            result.loans.push_back(
                new_loan(part, is_mut, loan_origin::capture, entry.span));
          }
          break;
        }
        case ast::capture_mode::by_move:
          capture_by_value(entry.name, true, entry.span, &entry);
          break;
        case ast::capture_mode::by_value:
          capture_by_value(entry.name, lambda.is_move, entry.span, &entry);
          break;
        }
      }
      return result;
    }
    for (const auto &capture : nested.captures()) {
      const auto written = nested.writes_capture(capture.name);
      if (borrow_capture(lambda, capture, written, borrow, result)) {
        continue;
      }
      if (written && !lookup(capture.name).has_value()) {
        // Captured from further out: this body changes it too.
        written_captures_.insert(capture.name);
      }
      capture_by_value(capture.name, capture.moved || lambda.is_move,
                       capture.span, capture.node);
    }
    return result;
  }

  /// An implicit capture of a local that is not `copy`, which the body does
  /// not consume and `move` does not force to move: the closure borrows it
  /// (ch. 16), mutably when the body changes it. The closure carries the
  /// loan, so the local cannot be moved or dropped while the closure may
  /// still be called, and a closure that must outlive it is written `move`.
  /// `self` is left out: it is already a reference to the receiver.
  /// Without `borrow` (a later copy of a generic lambda), the capture is
  /// recorded but the first copy's loan stands for it.
  auto borrow_capture(const ast::lambda_expr &lambda,
                      const implicit_capture &capture, bool written,
                      bool borrow, value &result) -> bool {
    if (capture.moved || lambda.is_move || capture.name == "self") {
      return false;
    }
    const auto local = lookup(capture.name);
    if (!local.has_value()) {
      // Captured from further out: the enclosing closure or task holds it,
      // and this closure borrows it from there. The enclosing capture is
      // what the owner sees, through `note_capture`.
      const auto type = captured_type(capture.name);
      if (!type.has_value() || !movable(*type) || task_block_ ||
          task_probe_) {
        return false;
      }
      if (written) {
        written_captures_.insert(capture.name);
      }
      note_capture(capture.name, /*moved=*/false, capture.span, capture.node);
      cfg_.borrowed_captures[&lambda].push_back(borrowed_capture{
          .name = capture.name, .node = capture.node, .is_mut = written});
      return true;
    }
    if (!cfg_.locals[*local].movable) {
      return false;
    }
    if (!borrow) {
      cfg_.borrowed_captures[&lambda].push_back(borrowed_capture{
          .name = capture.name, .node = capture.node, .is_mut = written});
      return true;
    }
    access(*local, written ? access_kind::borrow_mut : access_kind::borrow_shared,
           capture.span);
    const auto loan =
        new_loan(*local, written, loan_origin::capture, capture.span);
    cfg_.loans[loan].implicit = true;
    result.loans.push_back(loan);
    cfg_.borrowed_captures[&lambda].push_back(borrowed_capture{
        .name = capture.name, .node = capture.node, .is_mut = written});
    return true;
  }

  /// Leaves the function from the current block, returning `returned`:
  /// every open scope ends, then the function exits. `jump` is the `return`
  /// statement, marked for drop scheduling; null for a `?` exit.
  auto emit_return(const value &returned, const void *jump) -> void {
    flow(return_slot_, returned, /*replace=*/true);
    if (jump != nullptr) {
      mark_jump(jump, 0);
    }
    end_temporaries(0);
    end_scopes(0);
    goto_block(exit_);
  }

  // ------------------------------------------------------------------
  //  Control flow.
  // ------------------------------------------------------------------

  /// Whether `pattern`, matched against `subject`, binds owned parts.
  /// `patterns` are every pattern matched against `subject`.
  [[nodiscard]] auto
  binds_owned(const ast::expr *subject, const ast::node *pattern,
              const std::vector<const ast::node *> &patterns) const -> bool {
    const auto *pat = ast::as<ast::pattern>(pattern);
    return subject != nullptr && pat != nullptr &&
           owns_pattern_bindings(*subject, *pat, patterns, checked_,
                                 [this](std::string_view name) -> bool {
                                   return lookup(name).has_value();
                                 });
  }

  /// Declares the bindings of `pattern`, one of `patterns` matched against
  /// `subject_expr`, whose value `subject` holds. An arm whose bindings
  /// overlap owns the whole subject instead (`arm_owns_subject`), and may
  /// bind only copies besides: through the alias of the whole pattern
  /// (`p as whole`) when it has one, otherwise as a `<subject>` local.
  auto bind_arm(const ast::node *pattern, local_id subject,
                const ast::expr *subject_expr,
                const std::vector<const ast::node *> &patterns) -> void {
    const auto *pat = ast::as<ast::pattern>(pattern);
    if (pat == nullptr || subject_expr == nullptr ||
        !arm_owns_subject(*subject_expr, *pat, patterns, checked_,
                          [this](std::string_view name) -> bool {
                            return lookup(name).has_value();
                          })) {
      const auto owned = binds_owned(subject_expr, pattern, patterns);
      bind_pattern(pattern, subject, owned);
      // A miss leaves the subject unbound, so the path that did not match
      // drops it.
      if (owned && checked_.drop.drop_plans.contains(type_of(subject_expr))) {
        cfg_.unmatched_drops.insert(pattern);
      }
      return;
    }
    const auto *owner = owning_alias(*pat);
    if (owner == nullptr) {
      cfg_.owning_subjects.insert(pat);
      const auto whole = declare("<subject>", type_of(subject_expr), pat->span,
                                 /*whole=*/true, /*owns=*/false, pat);
      bind_value(whole, value{.loans = {}, .sources = {subject}});
    }
    for (const auto &binding : collect_pattern_bindings(*pat)) {
      const auto type = binding_type(binding, checked_);
      const auto is_owner = owner != nullptr && binding.alias_of == owner;
      if (!is_owner && movable(type)) {
        emit(invalid_move_event{.span = binding.span,
                                .reason = move_block::overlapping,
                                .place = binding.name,
                                .owner = {}});
      }
      const auto local =
          declare(binding.name, type, binding.span,
                  /*whole=*/is_owner, /*owns=*/false, binding.key());
      bind_value(local, value{.loans = {}, .sources = {subject}});
    }
  }

  /// How a `match`/`if let`/`while let`/`let` subject is used: moved or
  /// read, as `subject_moves` decides.
  [[nodiscard]] auto
  subject_mode(const ast::expr *subject,
               const std::vector<const ast::node *> &patterns) const
      -> use_mode {
    return subject == nullptr || subject_moves(*subject, patterns, checked_)
               ? use_mode::move
               : use_mode::read;
  }

  /// A subject temporary holding `v`, for patterns to bind from.
  auto hold_subject(const value &v) -> local_id {
    const auto subject = new_temp(local_role::subject_temp);
    flow(subject, v, /*replace=*/true);
    return subject;
  }

  auto lower_if(const std::vector<ast::if_branch> &branches,
                const std::vector<ast::ptr<ast::node>> &else_body,
                bool want_value) -> value {
    const auto join = new_temp(local_role::join_temp);
    const auto end = new_block();
    for (const auto &branch : branches) {
      auto subject = std::optional<local_id>{};
      if (branch.let_expr != nullptr) {
        // `if let`: the parser leaves a placeholder in `condition`.
        subject = hold_subject(
            eval(*branch.let_expr, subject_mode(branch.let_expr.get(),
                                                {branch.let_pattern.get()})));
      } else {
        if (branch.condition != nullptr) {
          static_cast<void>(eval_full(*branch.condition, use_mode::read));
        }
      }
      const auto then = new_block();
      const auto next = new_block();
      fork(then, next);
      current_ = then;
      push_scope(&branch.body);
      if (subject.has_value()) {
        bind_arm(branch.let_pattern.get(), *subject, branch.let_expr.get(),
                 {branch.let_pattern.get()});
      }
      const auto tail = lower_body(branch.body, want_value);
      flow(join, tail, /*replace=*/true);
      pop_scope();
      goto_block(end);
      current_ = next;
    }
    push_scope(&else_body);
    const auto tail = lower_body(else_body, want_value);
    flow(join, tail, /*replace=*/true);
    pop_scope();
    goto_block(end);
    current_ = end;
    return want_value ? value{.loans = {}, .sources = {join}} : value{};
  }

  /// `operand or else fallback`: a fork. The operand is consumed. One path
  /// carries its payload on as the value; the other runs the fallback, which
  /// is a full expression of its own since it runs only sometimes. Nothing
  /// binds what the operand holds on the failure path (the `@err` payload),
  /// so that path drops it before the fallback runs (`unmatched_drops`,
  /// keyed by the node, as for a `let ... else` whose pattern missed).
  auto lower_or_else(const ast::or_else_expr &or_else) -> value {
    const auto operand = eval_opt(or_else.operand.get(), use_mode::move);
    const auto join = new_temp(local_role::join_temp);
    const auto present = new_block();
    const auto absent = new_block();
    const auto end = new_block();
    fork(present, absent);
    current_ = present;
    flow(join, operand, /*replace=*/true);
    goto_block(end);
    current_ = absent;
    if (or_else.operand != nullptr &&
        checked_.drop.drop_plans.contains(type_of(or_else.operand.get()))) {
      cfg_.unmatched_drops.insert(&or_else);
    }
    if (or_else.fallback != nullptr) {
      flow(join, eval_full(*or_else.fallback), /*replace=*/true);
    }
    goto_block(end);
    current_ = end;
    return value{.loans = {}, .sources = {join}};
  }

  auto lower_match(const ast::expr *subject_expr,
                   const std::vector<ast::match_arm> &arms, bool want_value)
      -> value {
    const auto owned_subject = subject_expr;
    auto patterns = std::vector<const ast::node *>{};
    for (const auto &arm : arms) {
      patterns.push_back(arm.pattern.get());
    }
    const auto subject = hold_subject(
        eval_opt(subject_expr, subject_mode(subject_expr, patterns)));
    const auto join = new_temp(local_role::join_temp);
    const auto end = new_block();
    for (std::size_t i = 0; i < arms.size(); ++i) {
      const auto &arm = arms[i];
      const auto is_last = i + 1 == arms.size();
      const auto body = new_block();
      const auto next = is_last ? end : new_block();
      if (is_last) {
        goto_block(body);
      } else {
        fork(body, next);
      }
      current_ = body;
      push_scope(&arm.body_stmts);
      bind_arm(arm.pattern.get(), subject, owned_subject, patterns);
      if (arm.guard != nullptr) {
        static_cast<void>(eval_full(*arm.guard, use_mode::read));
        const auto guarded = new_block();
        fork(guarded, next);
        current_ = guarded;
      }
      auto tail = value{};
      if (arm.body_expr != nullptr) {
        tail = eval_full(*arm.body_expr);
      }
      auto stmts_tail = lower_body(arm.body_stmts, want_value);
      if (arm.body_expr == nullptr) {
        tail = std::move(stmts_tail);
      }
      flow(join, want_value ? tail : value{}, /*replace=*/true);
      pop_scope();
      goto_block(end);
      current_ = next;
    }
    if (arms.empty()) {
      goto_block(end);
    }
    current_ = end;
    return want_value ? value{.loans = {}, .sources = {join}} : value{};
  }

  /// A `for` loop. The iterable's value is held by a loop-source temporary
  /// used at every iteration, so whatever it borrows — `&xs` in
  /// `for x in &xs`, or the collection behind `xs.iter()` — stays borrowed
  /// for the whole loop.
  /// `for x in r` where `into_iter` would consume what `r` only borrows:
  /// allowed when the elements are `copy`, since the loop then copies them.
  auto check_loop_borrow(const ast::expr *iterable,
                         const iterator_loop_dispatch *dispatch) -> void {
    if (!loop_consumes_borrow(iterable, dispatch, checked_) ||
        !movable(dispatch->element_type)) {
      return;
    }
    emit(
        invalid_move_event{.span = iterable->span,
                           .reason = move_block::iterated,
                           .place = spell_value(*iterable),
                           .owner = checked_.types.display(type_of(iterable))});
  }

  auto lower_for(const ast::for_stmt &stmt) -> void {
    const iterator_loop_dispatch *dispatch = nullptr;
    if (const auto it = checked_.for_iterator_dispatches.find(&stmt);
        it != checked_.for_iterator_dispatches.end()) {
      dispatch = &it->second;
      check_loop_borrow(stmt.iterable.get(), dispatch);
    }
    const auto source = start_loop_source(
        stmt.iterable.get(),
        stmt.iterable != nullptr ? stmt.iterable->span : stmt.span);
    hold_loop_handle(&stmt.iterable, stmt.iterable.get(), dispatch, source);
    const auto head = new_block();
    goto_block(head);
    current_ = head;
    emit(use_event{.local = source});
    const auto body = new_block();
    const auto exit = new_block();
    fork(body, exit);
    current_ = body;
    push_loop_scope(head, exit, &stmt.body);
    const auto owned = for_variable_owns(stmt, checked_);
    if (owned) {
      cfg_.owning_loops.insert(&stmt);
    }
    for (const auto &pattern : stmt.patterns) {
      bind_pattern(pattern.get(), source, owned);
    }
    if (stmt.guard != nullptr) {
      static_cast<void>(eval_full(*stmt.guard, use_mode::read));
      const auto kept = new_block();
      fork(kept, head);
      current_ = kept;
    }
    static_cast<void>(lower_body(stmt.body, false));
    pop_scope();
    goto_block(head);
    current_ = exit;
    pop_scope();
  }

  /// Opens the scope around a loop over `iterable`, keyed by `key` (the
  /// loop's `iterable` member), and declares the iterator the loop holds
  /// there when it owns one that needs a drop (`loop_handle_type`): dropped
  /// once the loop ends, and by a jump out of it before what the enclosing
  /// scopes own.
  auto hold_loop_handle(const void *key, const ast::expr *iterable,
                        const iterator_loop_dispatch *dispatch, local_id source)
      -> void {
    push_scope(key);
    if (const auto type = loop_handle_type(iterable, dispatch, checked_)) {
      cfg_.loop_handles.insert(key);
      const auto handle =
          declare("<for iterator>", *type, iterable->span, /*whole=*/true,
                  /*owns=*/false, key);
      bind_value(handle, value{.loans = {}, .sources = {source}});
    }
  }

  auto start_loop_source(const ast::expr *iterable, source_span span)
      -> local_id {
    auto name = std::string{};
    if (iterable != nullptr) {
      auto projected = false;
      const auto &stripped = strip_groups(*iterable);
      const auto *operand = &stripped;
      if (stripped.kind == ast::node_kind::unary_expr) {
        const auto &unary = ast::cast<ast::unary_expr>(stripped);
        if (unary.operand != nullptr) {
          operand = unary.operand.get();
        }
      }
      if (const auto root = place_root(*operand, projected, &checked_)) {
        name = root->name;
      }
    }
    const auto source = new_local(std::move(name), local_role::loop_source,
                                  k_unknown_type, span, false);
    flow(source, eval_opt(iterable, use_mode::move),
         /*replace=*/true);
    return source;
  }

  auto lower_while(const ast::while_stmt &stmt) -> void {
    const auto head = new_block();
    goto_block(head);
    current_ = head;
    // A condition is a full expression. A `while let` subject's
    // temporaries last one iteration: they end after the body, when the
    // pattern fails, and at a `break` or `continue`.
    const auto depth = temporaries_.size();
    open_temporaries(stmt.let_expr != nullptr
                         ? static_cast<const void *>(stmt.let_expr.get())
                         : stmt.condition.get());
    auto subject = std::optional<local_id>{};
    if (stmt.let_expr != nullptr) {
      subject = hold_subject(
          eval(*stmt.let_expr,
               subject_mode(stmt.let_expr.get(), {stmt.let_pattern.get()})));
    } else {
      if (stmt.condition != nullptr) {
        static_cast<void>(eval(*stmt.condition, use_mode::read));
      }
      static_cast<void>(close_temporaries());
    }
    const auto body = new_block();
    const auto exit = new_block();
    fork(body, exit);
    current_ = body;
    push_loop_scope(head, exit, &stmt.body);
    scopes_.back().temporary_depth = depth;
    if (subject.has_value()) {
      bind_arm(stmt.let_pattern.get(), *subject, stmt.let_expr.get(),
               {stmt.let_pattern.get()});
    }
    static_cast<void>(lower_body(stmt.body, false));
    pop_scope();
    if (subject.has_value()) {
      end_temporaries(depth);
    }
    goto_block(head);
    current_ = exit;
    if (subject.has_value()) {
      end_temporaries(depth);
      temporaries_.pop_back();
    }
  }

  /// `for x in xs, y in ys if g => e`: nested loops appending `e` to the
  /// result, which carries the borrows of every element yielded.
  auto lower_comprehension(const ast::for_expr &comp) -> value {
    const auto join = new_temp(local_role::join_temp);
    lower_clause(comp, 0, join);
    return bears(type_of(&comp)) ? value{.loans = {}, .sources = {join}}
                                 : value{};
  }

  auto lower_clause(const ast::for_expr &comp, std::size_t index, local_id join)
      -> void {
    if (index == comp.clauses.size()) {
      if (comp.guard != nullptr) {
        static_cast<void>(eval_full(*comp.guard, use_mode::read));
      }
      if (comp.yield_expr != nullptr) {
        flow(join, eval_full(*comp.yield_expr), /*replace=*/false);
      }
      return;
    }
    const auto &clause = comp.clauses[index];
    const iterator_loop_dispatch *dispatch = nullptr;
    if (const auto it = checked_.comprehension_iterator_dispatches.find(
            static_cast<const ast::node *>(clause.iterable.get()));
        it != checked_.comprehension_iterator_dispatches.end()) {
      dispatch = &it->second;
      check_loop_borrow(clause.iterable.get(), dispatch);
    }
    const auto source = start_loop_source(
        clause.iterable.get(),
        clause.iterable != nullptr ? clause.iterable->span : comp.span);
    hold_loop_handle(&clause.iterable, clause.iterable.get(), dispatch, source);
    const auto head = new_block();
    goto_block(head);
    current_ = head;
    emit(use_event{.local = source});
    const auto body = new_block();
    const auto exit = new_block();
    fork(body, exit);
    current_ = body;
    push_loop_scope(head, exit, &clause);
    const auto owned = clause_variable_owns(clause, checked_);
    if (owned) {
      cfg_.owning_loops.insert(&clause);
    }
    for (const auto &pattern : clause.patterns) {
      bind_pattern(pattern.get(), source, owned);
    }
    lower_clause(comp, index + 1, join);
    pop_scope();
    goto_block(head);
    current_ = exit;
    pop_scope();
  }

  /// An inline `static for pat in xs => e`: one copy of `e` per iteration,
  /// so `e` is a loop body appending to the result. Its loop bindings are
  /// constants written into each copy; they hold nothing of the function's.
  auto lower_static_for(const ast::static_decl &decl) -> value {
    const auto join = new_temp(local_role::join_temp);
    const auto head = new_block();
    goto_block(head);
    current_ = head;
    const auto body = new_block();
    const auto exit = new_block();
    fork(body, exit);
    current_ = body;
    push_scope(nullptr);
    for (const auto &pattern : decl.for_patterns) {
      if (pattern != nullptr) {
        static_cast<void>(declare_pattern(*pattern));
      }
    }
    flow(join, eval_full(*decl.for_yield), /*replace=*/false);
    pop_scope();
    goto_block(head);
    current_ = exit;
    return bears(type_of(decl.for_yield.get()))
               ? value{.loans = {}, .sources = {join}}
               : value{};
  }

  auto lower_where(const ast::where_expr &where) -> value {
    push_scope(&where);
    for (const auto &binding : where.bindings) {
      auto v = eval_opt(binding.value.get(), use_mode::move);
      const auto local = declare(binding.name, type_of(binding.value.get()),
                                 binding.span, /*whole=*/true,
                                 /*owns=*/false, &binding);
      bind_value(local, v);
    }
    const auto join = new_temp(local_role::join_temp);
    flow(join, eval_opt(where.inner.get(), use_mode::move),
         /*replace=*/true);
    pop_scope();
    return value{.loans = {}, .sources = {join}};
  }

  /// A statement list in a scope of its own; the tail value (when wanted) is
  /// held across the scope's end, so a tail borrowing one of the block's own
  /// locals is caught.
  auto lower_scoped_body(const std::vector<ast::ptr<ast::node>> &stmts,
                         bool want_value) -> value {
    push_scope(&stmts);
    const auto tail = lower_body(stmts, want_value);
    const auto join = new_temp(local_role::join_temp);
    flow(join, tail, /*replace=*/true);
    pop_scope();
    return want_value ? value{.loans = {}, .sources = {join}} : value{};
  }

  /// Lowers `stmts` in order; with `want_value`, the last statement's value
  /// is the list's value.
  auto lower_body(const std::vector<ast::ptr<ast::node>> &stmts,
                  bool want_value) -> value {
    auto tail = value{};
    for (std::size_t i = 0; i < stmts.size(); ++i) {
      if (stmts[i] == nullptr) {
        continue;
      }
      const auto last = want_value && i + 1 == stmts.size();
      open_temporaries(stmts[i].get());
      tail = close_temporaries(lower_stmt(*stmts[i], last));
    }
    return tail;
  }

  auto lower_stmt(const ast::node &node, bool want_value) -> value {
    if (node.has_error) {
      return {};
    }
    switch (node.kind) {
    case ast::node_kind::let_stmt:
      lower_let(ast::cast<ast::let_stmt>(node));
      return {};

    case ast::node_kind::var_stmt: {
      const auto &stmt = ast::cast<ast::var_stmt>(node);
      auto v = eval_opt(stmt.initializer.get(), use_mode::move);
      const auto local =
          declare(stmt.name, type_of(&stmt), stmt.span, /*whole=*/true,
                  /*owns=*/false, &stmt);
      bind_value(local, v);
      return {};
    }

    case ast::node_kind::assign_stmt:
      lower_assign(ast::cast<ast::assign_stmt>(node));
      return {};

    case ast::node_kind::expr_stmt: {
      const auto &stmt = ast::cast<ast::expr_stmt>(node);
      if (stmt.expr == nullptr) {
        return {};
      }
      if (want_value) {
        // The block's value: its temporaries end with it.
        return eval_full(*stmt.expr);
      }
      auto v = eval(*stmt.expr, use_mode::move);
      if (!is_place(*stmt.expr)) {
        // A value computed and discarded is a temporary of its statement.
        static_cast<void>(hold_temporary(*stmt.expr, v));
      }
      return {};
    }

    case ast::node_kind::return_stmt: {
      const auto &stmt = ast::cast<ast::return_stmt>(node);
      emit_return(stmt.value != nullptr ? eval_full(*stmt.value) : value{},
                  &node);
      start_unreachable();
      return {};
    }

    case ast::node_kind::break_stmt:
    case ast::node_kind::continue_stmt: {
      const auto is_break = node.kind == ast::node_kind::break_stmt;
      for (auto i = scopes_.size(); i > 0; --i) {
        if (scopes_[i - 1].is_loop) {
          mark_jump(&node, i - 1);
          end_temporaries(scopes_[i - 1].temporary_depth);
          end_scopes(i - 1);
          goto_block(is_break ? scopes_[i - 1].break_target
                              : scopes_[i - 1].continue_target);
          break;
        }
      }
      start_unreachable();
      return {};
    }

    case ast::node_kind::if_stmt: {
      const auto &stmt = ast::cast<ast::if_stmt>(node);
      return lower_if(stmt.branches, stmt.else_body, want_value);
    }

    case ast::node_kind::while_stmt:
      lower_while(ast::cast<ast::while_stmt>(node));
      return {};

    case ast::node_kind::scope_stmt:
      static_cast<void>(lower_scoped_body(ast::cast<ast::scope_stmt>(node).body,
                                          /*want_value=*/false));
      return {};

    case ast::node_kind::for_stmt:
      lower_for(ast::cast<ast::for_stmt>(node));
      return {};

    case ast::node_kind::match_stmt: {
      const auto &stmt = ast::cast<ast::match_stmt>(node);
      return lower_match(stmt.subject.get(), stmt.arms, want_value);
    }

    case ast::node_kind::crew_stmt:
      lower_crew_body(&node, ast::cast<ast::crew_stmt>(node).body);
      return {};

    case ast::node_kind::splice_stmt: {
      const auto it = checked_.spliced_fragments.find(&node);
      if (it != checked_.spliced_fragments.end() && it->second != nullptr) {
        return lower_stmt(*it->second, want_value);
      }
      return {};
    }

    case ast::node_kind::func_decl: {
      // A function nested in a body is checked on its own; it cannot
      // capture the enclosing function's locals.
      const auto &decl = ast::cast<ast::func_decl>(node);
      if (!decl.modifiers.is_intrinsic) {
        auto nested = cfg_builder(checked_, out_, nullptr);
        nested.build_function(decl);
      }
      return {};
    }

    case ast::node_kind::asm_stmt:
      return {};

    case ast::node_kind::static_decl: {
      const auto &decl = ast::cast<ast::static_decl>(node);
      if (decl.decl_kind != ast::static_decl_kind::for_inline ||
          decl.for_yield == nullptr) {
        return {};
      }
      return lower_static_for(decl);
    }

    default:
      if (const auto *expr = ast::as<ast::expr>(&node)) {
        auto v = eval(*expr, use_mode::move);
        return want_value ? v : value{};
      }
      // A nested item (type, `use`, `static`, ...) has no runtime effect on
      // this function's locals.
      return {};
    }
  }

  /// `let f = (x) => ...` whose parameter has no type: one closure per copy
  /// the checker made for a use of `f`, each a local of its own under its
  /// copy's key (`use_name`), so moving one leaves the others to drop. The
  /// plain name finds the first copy and, through `generic_groups_`, all of
  /// them, which is what a capture list naming `f` captures.
  auto declare_generic_lambda(const ast::let_stmt &stmt) -> bool {
    const auto *lambda =
        ast::as<const ast::lambda_expr>(stmt.initializer.get());
    if (lambda == nullptr || stmt.pattern == nullptr ||
        stmt.pattern->kind != ast::node_kind::binding_pattern ||
        !stmt.else_body.empty()) {
      return false;
    }
    const auto copies =
        checked_.ownership.generic_lambda_instances.find(lambda);
    if (copies == checked_.ownership.generic_lambda_instances.end()) {
      return false;
    }
    const auto &binding = ast::cast<ast::binding_pattern>(*stmt.pattern);
    auto group = std::vector<local_id>{};
    auto first = true;
    // The copies are one closure value: the first takes the capture borrows
    // and every copy carries them, so a `&mut` capture is one mutable borrow
    // however many copies there are (`eval_lambda`).
    auto borrows = std::vector<loan_id>{};
    for (const auto *copy : copies->second) {
      auto made = eval_lambda(*copy, first, /*borrow=*/first);
      if (first) {
        borrows = made.loans;
      } else {
        made.loans.insert(made.loans.end(), borrows.begin(), borrows.end());
      }
      first = false;
      const auto local = declare(binding.name, type_of(copy), binding.span,
                                 /*whole=*/true, /*owns=*/false, copy);
      scopes_.back().names.back().first = copy_key(binding.name, copy);
      cfg_.locals[local].immutable = true;
      bind_value(local, made);
      group.push_back(local);
    }
    if (!group.empty()) {
      scopes_.back().names.emplace_back(binding.name, group.front());
      generic_groups_.insert_or_assign(group.front(), std::move(group));
    }
    return true;
  }

  auto lower_let(const ast::let_stmt &stmt) -> void {
    if (declare_generic_lambda(stmt)) {
      return;
    }
    auto v =
        eval_opt(stmt.initializer.get(),
                 subject_mode(stmt.initializer.get(), {stmt.pattern.get()}));
    if (stmt.pattern == nullptr) {
      return;
    }
    if (!stmt.else_body.empty()) {
      // `let pattern = e else: ...`: the else path must leave the scope.
      const auto subject = hold_subject(v);
      const auto bound = new_block();
      const auto otherwise = new_block();
      fork(bound, otherwise);
      current_ = otherwise;
      static_cast<void>(lower_scoped_body(stmt.else_body, false));
      goto_block(bound);
      current_ = bound;
      bind_arm(stmt.pattern.get(), subject, stmt.initializer.get(),
               {stmt.pattern.get()});
      return;
    }
    if (stmt.pattern->kind == ast::node_kind::binding_pattern) {
      const auto &binding = ast::cast<ast::binding_pattern>(*stmt.pattern);
      const auto local =
          declare(binding.name, type_of(stmt.pattern.get()), binding.span,
                  /*whole=*/true, /*owns=*/false, stmt.pattern.get());
      cfg_.locals[local].immutable = !binding.is_mut;
      bind_value(local, v);
      return;
    }
    bind_arm(stmt.pattern.get(), hold_subject(v), stmt.initializer.get(),
             {stmt.pattern.get()});
  }

  auto lower_assign(const ast::assign_stmt &stmt) -> void {
    auto v = eval_opt(stmt.value.get(), use_mode::move);
    if (stmt.target == nullptr) {
      return;
    }
    const auto &target = strip_groups(*stmt.target);
    if (!is_place(target)) {
      static_cast<void>(eval(target, use_mode::read));
      return;
    }
    auto projected = false;
    const auto root = place_root(target, projected, &checked_);
    eval_subscripts(target);
    const auto local = lookup(root->name);
    if (!local.has_value()) {
      if (reject_static_write(*root, static_write::assign, target.span)) {
        return;
      }
      if (reject_task_capture_write(*root, static_write::assign, target.span)) {
        note_capture(root->name, false, root->span, root->node);
        return;
      }
      if (projected) {
        reject_captured_write(*root, target, static_write::assign, target.span);
      }
      written_captures_.insert(root->name);
      note_capture(root->name, false, root->span, root->node);
      return;
    }
    if (projected) {
      reject_place_write(cfg_.locals[*local], &target, static_write::assign,
                         target.span);
    }
    if (!projected && stmt.op == ast::assign_op::assign) {
      emit(assign_event{.key = &stmt, .local = *local});
      access(*local, access_kind::write_whole, target.span);
      bind_value(*local, v);
      return;
    }
    if (projected && stmt.op == ast::assign_op::assign) {
      auto steps = std::vector<place_step>{};
      place_steps(target, steps);
      const auto through_ref = [&]() -> bool {
        // A field of a local that owns its value is tracked by path; one
        // reached through a reference or `*r` always holds a value. An
        // element (`xs[i] = v`) or a raw pointer write is the collection's
        // or the `machine` code's to handle.
        auto reached = cfg_.locals[*local].type;
        for (const auto &step : steps) {
          if (step.step == place_step::kind::index) {
            return false;
          }
          if (!checked_.types.is_unknown(reached) &&
              checked_.types.entry(reached).kind == type_kind::ptr_kind) {
            return false;
          }
          reached = step.type;
        }
        return true;
      };
      if (through_ref()) {
        const auto fields_only =
            std::ranges::all_of(steps, [](const place_step &step) -> bool {
              return step.step == place_step::kind::field;
            });
        emit(assign_event{.key = &stmt,
                          .local = fields_only && cfg_.locals[*local].owns
                                       ? field_path_local(*local, steps)
                                       : k_no_local});
      }
    }
    if (!projected) {
      access(*local, access_kind::write_part, target.span);
    } else {
      access_part_of(*local, access_kind::write_part, target.span);
      // Assigning a field fills it again after a partial move; `+=` reads it.
      auto steps = std::vector<place_step>{};
      place_steps(target, steps);
      use_field_path(*local, steps,
                     stmt.op == ast::assign_op::assign
                         ? access_kind::write_whole
                         : access_kind::read,
                     target.span);
    }
    flow(*local, v, /*replace=*/false);
  }
};

auto owns_pattern_bindings(type_id subject_type, const ast::node &pattern,
                           const checked_types &checked) -> bool {
  const auto *pat = ast::as<ast::pattern>(&pattern);
  return pat != nullptr && pattern_bindings_are_disjoint(*pat) &&
         !checked.types.is_unknown(subject_type) &&
         subject_type != k_error_type &&
         checked.types.entry(subject_type).kind != type_kind::ref_kind &&
         !checked.types.is_view(subject_type);
}

auto loop_consumes_borrow(const ast::expr *iterable,
                          const iterator_loop_dispatch *dispatch,
                          const checked_types &checked) -> bool {
  if (iterable == nullptr || dispatch == nullptr ||
      dispatch->adapter_decl == nullptr || dispatch->adapter_borrows) {
    return false;
  }
  const auto found = checked.node_types.find(iterable);
  return found != checked.node_types.end() &&
         !checked.types.is_unknown(found->second) &&
         checked.types.entry(found->second).kind == type_kind::ref_kind;
}

auto loop_handle_type(const ast::expr *iterable,
                      const iterator_loop_dispatch *dispatch,
                      const checked_types &checked) -> std::optional<type_id> {
  if (iterable == nullptr ||
      loop_consumes_borrow(iterable, dispatch, checked)) {
    return std::nullopt;
  }
  const auto found = checked.node_types.find(iterable);
  if (found == checked.node_types.end() ||
      checked.types.is_unknown(found->second)) {
    return std::nullopt;
  }
  auto type = found->second;
  if (dispatch != nullptr) {
    if (dispatch->adapter_decl != nullptr) {
      type = dispatch->adapter_result_type;
    }
  } else {
    const auto &entry = checked.types.entry(type);
    if (entry.kind != type_kind::builtin_generic_kind ||
        entry.name != "generator") {
      return std::nullopt;
    }
  }
  if (!checked.drop.drop_plans.contains(type)) {
    return std::nullopt;
  }
  return type;
}

/// Whether `patterns` own the parts of each element a loop over `iterable`
/// hands them. `dispatch` is the loop's `next` dispatch, if it has one.
template <typename pattern_ptr>
auto loop_patterns_own(const std::vector<pattern_ptr> &patterns,
                       const ast::expr *iterable,
                       const iterator_loop_dispatch *dispatch,
                       const checked_types &checked) -> bool {
  if (iterable == nullptr || patterns.empty() ||
      loop_consumes_borrow(iterable, dispatch, checked)) {
    return false;
  }
  auto element = k_unknown_type;
  if (dispatch != nullptr) {
    element = dispatch->element_type;
  } else {
    const auto type_it = checked.node_types.find(iterable);
    if (type_it == checked.node_types.end()) {
      return false;
    }
    const auto &entry = checked.types.entry(type_it->second);
    if (entry.kind != type_kind::builtin_generic_kind ||
        entry.name != "generator" || entry.args.empty()) {
      return false;
    }
    element = entry.args[0];
  }
  return std::ranges::all_of(patterns, [&](const pattern_ptr &pattern) -> bool {
    return pattern != nullptr &&
           owns_pattern_bindings(element, *pattern, checked);
  });
}

auto for_variable_owns(const ast::for_stmt &stmt, const checked_types &checked)
    -> bool {
  const auto it = checked.for_iterator_dispatches.find(&stmt);
  return loop_patterns_own(
      stmt.patterns, stmt.iterable.get(),
      it != checked.for_iterator_dispatches.end() ? &it->second : nullptr,
      checked);
}

auto clause_variable_owns(const ast::for_expr::iter_clause &clause,
                          const checked_types &checked) -> bool {
  const auto it = checked.comprehension_iterator_dispatches.find(
      static_cast<const ast::node *>(clause.iterable.get()));
  return loop_patterns_own(clause.patterns, clause.iterable.get(),
                           it != checked.comprehension_iterator_dispatches.end()
                               ? &it->second
                               : nullptr,
                           checked);
}

auto subject_moves(const ast::expr &subject,
                   const std::vector<const ast::node *> &patterns,
                   const checked_types &checked) -> bool {
  auto projected = false;
  if (!place_root(subject, projected, &checked).has_value()) {
    return true;
  }
  for (const auto *node : patterns) {
    const auto *pat = ast::as<ast::pattern>(node);
    if (pat == nullptr) {
      continue;
    }
    for (const auto &binding : collect_pattern_bindings(*pat)) {
      const auto type = binding_type(binding, checked);
      if (!checked.types.is_unknown(type) && !checked.is_copy(type)) {
        return true;
      }
    }
  }
  return false;
}

/// Whether matching `subject` against `patterns` hands the match the
/// subject's value to own: the subject moves (`subject_moves`), its type is
/// a value rather than a reference or view, and it is a fresh value or a
/// local or a field of one, not `self` or a global.
auto match_owns_subject(const ast::expr &subject,
                        const std::vector<const ast::node *> &patterns,
                        const checked_types &checked,
                        const std::function<bool(std::string_view)> &is_local)
    -> bool {
  const auto type_it = checked.node_types.find(&subject);
  if (type_it == checked.node_types.end()) {
    return false;
  }
  const auto type = type_it->second;
  if (checked.types.is_unknown(type) || type == k_error_type ||
      checked.types.entry(type).kind == type_kind::ref_kind ||
      checked.types.is_view(type) ||
      !subject_moves(subject, patterns, checked)) {
    return false;
  }
  auto projected = false;
  const auto root = place_root(subject, projected, &checked);
  if (!root.has_value()) {
    return true; // a call result, constructor or literal: a fresh value
  }
  // A field of a local moves out of it like the whole does: the local then
  // drops everything but that field. A part the rules forbid moving out of
  // (an element, or anything behind a reference) is rejected on its own.
  return root->name != "self" && is_local(root->name);
}

auto owns_pattern_bindings(
    const ast::expr &subject, const ast::pattern &pattern,
    const std::vector<const ast::node *> &patterns,
    const checked_types &checked,
    const std::function<bool(std::string_view)> &is_local) -> bool {
  return pattern_bindings_are_disjoint(pattern) &&
         match_owns_subject(subject, patterns, checked, is_local);
}

auto arm_owns_subject(const ast::expr &subject, const ast::pattern &pattern,
                      const std::vector<const ast::node *> &patterns,
                      const checked_types &checked,
                      const std::function<bool(std::string_view)> &is_local)
    -> bool {
  const auto type_it = checked.node_types.find(&subject);
  return !pattern_bindings_are_disjoint(pattern) &&
         type_it != checked.node_types.end() &&
         checked.drop.drop_plans.contains(type_it->second) &&
         match_owns_subject(subject, patterns, checked, is_local);
}

} // namespace

auto generator_start(const ast::func_decl &decl) -> const void * {
  return &decl.modifiers;
}

auto task_block_start(const ast::expr &block) -> const void * {
  return &block.span;
}

auto build_function_cfgs(const ast::func_decl &decl,
                         const checked_types &checked)
    -> std::vector<function_cfg> {
  auto out = std::vector<function_cfg>{};
  auto builder = cfg_builder(checked, out, nullptr);
  builder.build_function(decl);
  return out;
}

} // namespace cinder::semantic::ownership
