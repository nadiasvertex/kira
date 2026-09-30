#include "ownership_cfg.h"

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "src/semantic/binding_walk.h"

namespace cinder::semantic::ownership {
namespace {

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
};

auto strip_groups(const ast::expr &expr) -> const ast::expr & {
  const auto *current = &expr;
  while (current->kind == ast::node_kind::group_expr) {
    const auto &group = dynamic_cast<const ast::group_expr &>(*current);
    if (group.inner == nullptr) {
      break;
    }
    current = group.inner.get();
  }
  return *current;
}

/// The name a place expression is rooted at.
struct root_name {
  std::string_view name;
  source_span span = source_span::dummy();
};

/// One step from a place's root toward the place: a field, an element, or a
/// dereference, with the type it reaches.
struct place_step {
  enum class kind : std::uint8_t { field, index, deref };
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
auto place_root(const ast::expr &expr, bool &projected)
    -> std::optional<root_name> {
  const auto *current = &expr;
  while (true) {
    switch (current->kind) {
    case ast::node_kind::ident_expr:
      if (current->has_error) {
        return std::nullopt;
      }
      return root_name{.name =
                           dynamic_cast<const ast::ident_expr &>(*current).name,
                       .span = current->span};
    case ast::node_kind::module_path_expr: {
      const auto &path = dynamic_cast<const ast::module_path_expr &>(*current);
      if (path.segments.empty()) {
        return std::nullopt;
      }
      projected = projected || path.segments.size() > 1;
      return root_name{.name = path.segments.front(), .span = current->span};
    }
    case ast::node_kind::field_expr: {
      const auto &field = dynamic_cast<const ast::field_expr &>(*current);
      if (field.object == nullptr) {
        return std::nullopt;
      }
      projected = true;
      current = field.object.get();
      break;
    }
    case ast::node_kind::index_expr: {
      const auto &index = dynamic_cast<const ast::index_expr &>(*current);
      if (index.object == nullptr) {
        return std::nullopt;
      }
      projected = true;
      current = index.object.get();
      break;
    }
    case ast::node_kind::group_expr: {
      const auto &group = dynamic_cast<const ast::group_expr &>(*current);
      if (group.inner == nullptr) {
        return std::nullopt;
      }
      current = group.inner.get();
      break;
    }
    case ast::node_kind::unary_expr: {
      const auto &unary = dynamic_cast<const ast::unary_expr &>(*current);
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
        declare_param(*param.pattern);
      }
    }
    finish(decl.body_expr.get(), decl.body_stmts);
  }

  auto build_lambda(const ast::lambda_expr &lambda) -> void {
    begin(&lambda);
    for (const auto &param : lambda.params) {
      if (param.pattern != nullptr) {
        declare_param(*param.pattern);
      }
    }
    finish(lambda.body_expr.get(), lambda.body_stmts);
  }

  [[nodiscard]] auto captures() const -> const std::vector<implicit_capture> & {
    return captures_;
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
  };

  const checked_types &checked_;
  std::vector<function_cfg> &out_;
  cfg_builder *outer_;
  function_cfg cfg_;
  std::vector<scope> scopes_;
  std::vector<implicit_capture> captures_;
  block_id current_ = 0;
  block_id exit_ = 0;
  local_id return_slot_ = 0;
  /// The `field_path` local for each (parent, field) pair made so far.
  std::map<std::pair<local_id, std::string>, local_id> field_paths_;

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
  }

  /// Lowers the body in a scope of its own, sends its tail value to the
  /// return slot, ends the body and parameter scopes, and publishes the
  /// graph.
  auto finish(const ast::expr *body_expr,
              const std::vector<ast::ptr<ast::node>> &body_stmts) -> void {
    auto tail = value{};
    if (body_expr != nullptr) {
      tail = eval(*body_expr, use_mode::move);
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
              loan_id exempt = k_no_loan) -> void {
    emit(access_event{
        .local = local, .kind = kind, .span = span, .exempt = exempt});
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
           checked_.borrow_bearing_types.contains(type);
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
                            .break_target = break_target});
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
  auto mark_jump(const ast::node &jump, std::size_t depth) -> void {
    auto event = scope_exit_event{.key = &jump, .groups = {}};
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
               bool whole = false, bool owns = false) -> local_id {
    const auto local =
        new_local(name, local_role::binding, type, span, movable(type));
    cfg_.locals[local].whole = whole;
    cfg_.locals[local].owns = (whole || owns) && name != "self" &&
                              owns_storage(type, name) &&
                              (checked_.types.is_unknown(type) ||
                               checked_.types.entry(type).kind !=
                                   type_kind::ptr_kind);
    if (owns_storage(type, name)) {
      scopes_.back().owned.push_back(local);
    }
    scopes_.back().names.emplace_back(std::move(name), local);
    return local;
  }

  /// Declares every binding `pattern` introduces. Parameters start out
  /// holding nothing; the caller gives any other binding its value.
  auto declare_pattern(const ast::node &pattern, bool owned = false)
      -> std::vector<local_id> {
    auto locals = std::vector<local_id>{};
    const auto *pat = dynamic_cast<const ast::pattern *>(&pattern);
    if (pat == nullptr) {
      return locals;
    }
    for (const auto &binding : collect_pattern_bindings(*pat)) {
      const auto type = type_of(binding.node);
      // An owned binding that needs a drop is dropped with its scope; the
      // others are left to `storage_dead`, like every pattern binding.
      const auto whole = owned && binding.node != nullptr &&
                         checked_.drop_plans.contains(type);
      locals.push_back(
          declare(binding.name, type, binding.span, whole, owned));
    }
    return locals;
  }

  /// Declares a parameter: whole when it is a single name.
  auto declare_param(const ast::node &pattern) -> void {
    if (pattern.kind == ast::node_kind::binding_pattern) {
      const auto &binding = dynamic_cast<const ast::binding_pattern &>(pattern);
      declare(binding.name, type_of(&pattern), binding.span, /*whole=*/true);
      return;
    }
    // A by-value destructuring parameter owns the parts it binds.
    declare_pattern(pattern,
                    owns_pattern_bindings(type_of(&pattern), pattern, checked_));
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
  auto note_capture(std::string_view name, bool moved, source_span span)
      -> void {
    if (outer_ == nullptr) {
      return;
    }
    for (auto &capture : captures_) {
      if (capture.name == name) {
        capture.moved = capture.moved || moved;
        return;
      }
    }
    captures_.push_back(implicit_capture{
        .name = std::string(name), .moved = moved, .span = span});
  }

  // ------------------------------------------------------------------
  //  Places.
  // ------------------------------------------------------------------

  /// Evaluates the subscripts along a place's projection chain, innermost
  /// first — the only parts of a place that run code.
  auto eval_subscripts(const ast::expr &expr) -> void {
    switch (expr.kind) {
    case ast::node_kind::field_expr: {
      const auto &field = dynamic_cast<const ast::field_expr &>(expr);
      if (field.object != nullptr) {
        eval_subscripts(*field.object);
      }
      return;
    }
    case ast::node_kind::index_expr: {
      const auto &index = dynamic_cast<const ast::index_expr &>(expr);
      if (index.object != nullptr) {
        eval_subscripts(*index.object);
      }
      if (index.index != nullptr) {
        static_cast<void>(eval(*index.index, use_mode::read));
      }
      return;
    }
    case ast::node_kind::group_expr: {
      const auto &group = dynamic_cast<const ast::group_expr &>(expr);
      if (group.inner != nullptr) {
        eval_subscripts(*group.inner);
      }
      return;
    }
    case ast::node_kind::unary_expr: {
      const auto &unary = dynamic_cast<const ast::unary_expr &>(expr);
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
      const auto &path = dynamic_cast<const ast::module_path_expr &>(expr);
      const auto types = checked_.value_path_types.find(&path);
      for (std::size_t i = 1; i < path.segments.size(); ++i) {
        const auto type = types != checked_.value_path_types.end() &&
                                  i < types->second.size()
                              ? types->second[i]
                              : k_unknown_type;
        out.push_back(place_step{.step = place_step::kind::field,
                                 .field = path.segments[i],
                                 .type = type});
      }
      return;
    }
    case ast::node_kind::field_expr: {
      const auto &field = dynamic_cast<const ast::field_expr &>(expr);
      place_steps(*field.object, out);
      out.push_back(place_step{.step = place_step::kind::field,
                               .field = field.field_name,
                               .type = type_of(&expr)});
      return;
    }
    case ast::node_kind::index_expr: {
      const auto &index = dynamic_cast<const ast::index_expr &>(expr);
      place_steps(*index.object, out);
      out.push_back(
          place_step{.step = place_step::kind::index, .type = type_of(&expr)});
      return;
    }
    case ast::node_kind::group_expr:
      place_steps(*dynamic_cast<const ast::group_expr &>(expr).inner, out);
      return;
    case ast::node_kind::unary_expr:
      place_steps(*dynamic_cast<const ast::unary_expr &>(expr).operand, out);
      out.push_back(
          place_step{.step = place_step::kind::deref, .type = type_of(&expr)});
      return;
    default:
      return;
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
    const auto it = checked_.drop_plans.find(type);
    return it != checked_.drop_plans.end() && it->second.own_drop.has_value();
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
      // A raw read (`p[i]`, `*p`) is a bitwise copy whose ownership is the
      // `machine` code's to track (ch. 38, Raw memory and ownership).
      if (!checked_.types.is_unknown(through) &&
          checked_.types.entry(through).kind == type_kind::ptr_kind) {
        return std::nullopt;
      }
      return invalid_move_event{
          .span = span,
          .reason = steps[i].step == place_step::kind::index
                        ? move_block::element
                        : move_block::deref,
          .place = spell_place(root_name, steps, i + 1),
          .owner = spell_place(root_name, steps, i)};
    }
    if (!cfg_.locals[root].owns) {
      return invalid_move_event{.span = span,
                                .reason = move_block::borrowed,
                                .place = spell_place(root_name, steps,
                                                     steps.size()),
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
    const auto root = place_root(expr, projected);
    if (evaluate_subscripts) {
      eval_subscripts(expr);
    }
    const auto local = lookup(root->name);
    if (!local.has_value()) {
      note_capture(root->name, kind == access_kind::move && !projected,
                   root->span);
      return {};
    }
    if (!projected) {
      if (kind == access_kind::move && !cfg_.locals[*local].movable) {
        kind = access_kind::read;
      }
      access(*local, kind, expr.span);
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
    const auto moving =
        kind == access_kind::move && movable(type_of(&expr));
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
                   expr.span);
  }

  /// Accesses the `field_path` local for `root` followed by `steps`, when
  /// every step is a field of a value `root` owns — the only parts whose
  /// moves are tracked.
  auto use_field_path(local_id root, const std::vector<place_step> &steps,
                      access_kind kind, source_span span) -> void {
    const auto fields_only =
        std::ranges::all_of(steps, [](const place_step &step) -> bool {
          return step.step == place_step::kind::field;
        });
    if (fields_only && !steps.empty() && cfg_.locals[root].owns) {
      access(field_path_local(root, steps), kind, span);
    }
  }

  /// Borrows the place `expr` — or, when it is not a place, evaluates it as
  /// a temporary, which nothing else can alias.
  auto borrow_place(const ast::expr &expr, bool is_mut, loan_origin origin,
                    source_span span, loan_id exempt = k_no_loan,
                    bool evaluate_subscripts = true) -> value {
    if (!is_place(expr)) {
      return eval(expr, use_mode::read);
    }
    auto projected = false;
    const auto root = place_root(expr, projected);
    if (evaluate_subscripts) {
      eval_subscripts(expr);
    }
    const auto local = lookup(root->name);
    if (!local.has_value()) {
      note_capture(root->name, /*moved=*/false, root->span);
      return {};
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
        return eval_field_of_value(dynamic_cast<const ast::field_expr &>(expr));
      }
      return {};

    case ast::node_kind::index_expr:
      return eval_index(dynamic_cast<const ast::index_expr &>(expr), mode);

    case ast::node_kind::group_expr: {
      const auto &group = dynamic_cast<const ast::group_expr &>(expr);
      return eval_opt(group.inner.get(), mode);
    }

    case ast::node_kind::unary_expr:
      return eval_unary(dynamic_cast<const ast::unary_expr &>(expr), mode);

    case ast::node_kind::binary_expr: {
      const auto &binary = dynamic_cast<const ast::binary_expr &>(expr);
      const auto temp = new_temp(local_role::call_temp);
      stash(temp, eval_opt(binary.lhs.get(), use_mode::read));
      stash(temp, eval_opt(binary.rhs.get(), use_mode::read));
      return compound_result(expr, temp);
    }

    case ast::node_kind::cast_expr: {
      const auto &cast = dynamic_cast<const ast::cast_expr &>(expr);
      return eval_opt(cast.operand.get(), mode);
    }

    case ast::node_kind::try_expr: {
      const auto &tri = dynamic_cast<const ast::try_expr &>(expr);
      auto result = eval_opt(tri.operand.get(), mode);
      // `?` may return from the function right here, carrying the operand.
      const auto early = new_block();
      const auto rest = new_block();
      fork(rest, early);
      current_ = early;
      emit_return(result, nullptr);
      current_ = rest;
      return result;
    }

    case ast::node_kind::call_expr:
      return eval_call(dynamic_cast<const ast::call_expr &>(expr));

    case ast::node_kind::tuple_expr: {
      const auto &tuple = dynamic_cast<const ast::tuple_expr &>(expr);
      const auto temp = new_temp(local_role::call_temp);
      for (const auto &element : tuple.elements) {
        stash(temp, eval_opt(element.get(), use_mode::move));
      }
      return compound_result(expr, temp);
    }

    case ast::node_kind::array_expr: {
      const auto &array = dynamic_cast<const ast::array_expr &>(expr);
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
      const auto &literal = dynamic_cast<const ast::struct_expr &>(expr);
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
      const auto &interp =
          dynamic_cast<const ast::interpolated_string_expr &>(expr);
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

    case ast::node_kind::lambda_expr:
      return eval_lambda(dynamic_cast<const ast::lambda_expr &>(expr));

    case ast::node_kind::if_expr: {
      const auto &if_e = dynamic_cast<const ast::if_expr &>(expr);
      return lower_if(if_e.branches, if_e.else_body, /*want_value=*/true);
    }

    case ast::node_kind::match_expr: {
      const auto &match_e = dynamic_cast<const ast::match_expr &>(expr);
      return lower_match(match_e.subject.get(), match_e.arms,
                         /*want_value=*/true);
    }

    case ast::node_kind::block_expr:
      return lower_scoped_body(
          dynamic_cast<const ast::block_expr &>(expr).stmts,
          /*want_value=*/true);

    case ast::node_kind::for_expr:
      return lower_comprehension(dynamic_cast<const ast::for_expr &>(expr));

    case ast::node_kind::where_expr:
      return lower_where(dynamic_cast<const ast::where_expr &>(expr));

    case ast::node_kind::await_expr: {
      const auto &await_e = dynamic_cast<const ast::await_expr &>(expr);
      return eval_opt(await_e.operand.get(), use_mode::move);
    }

    case ast::node_kind::yield_expr: {
      const auto &yield_e = dynamic_cast<const ast::yield_expr &>(expr);
      static_cast<void>(eval_opt(yield_e.value.get(), use_mode::move));
      return {};
    }

    case ast::node_kind::par_expr:
    case ast::node_kind::race_expr: {
      const auto &branches =
          expr.kind == ast::node_kind::par_expr
              ? dynamic_cast<const ast::par_expr &>(expr).branches
              : dynamic_cast<const ast::race_expr &>(expr).branches;
      const auto temp = new_temp(local_role::call_temp);
      for (const auto &b : branches) {
        stash(temp, eval_opt(b.get(), use_mode::move));
      }
      return compound_result(expr, temp);
    }

    case ast::node_kind::async_expr:
      static_cast<void>(lower_scoped_body(
          dynamic_cast<const ast::async_expr &>(expr).body, false));
      return {};

    case ast::node_kind::crew_expr:
      static_cast<void>(lower_scoped_body(
          dynamic_cast<const ast::crew_expr &>(expr).body, false));
      return {};

    case ast::node_kind::on_expr: {
      const auto &on = dynamic_cast<const ast::on_expr &>(expr);
      static_cast<void>(eval_opt(on.sender.get(), use_mode::read));
      static_cast<void>(lower_scoped_body(on.body, false));
      return {};
    }

    case ast::node_kind::splice_expr: {
      const auto it = checked_.spliced_fragments.find(&expr);
      if (it != checked_.spliced_fragments.end()) {
        if (const auto *fragment =
                dynamic_cast<const ast::expr *>(it->second)) {
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
  auto eval_field_of_value(const ast::field_expr &field) -> value {
    auto object = eval_opt(field.object.get(), use_mode::read);
    return bears(type_of(&field)) ? object : value{};
  }

  /// A struct literal's shorthand field `{x}`, which reads as `{x: x}`.
  auto eval_shorthand_field(const ast::struct_field_init &field) -> value {
    const auto local = lookup(field.name);
    if (!local.has_value()) {
      note_capture(field.name, /*moved=*/true, field.span);
      return {};
    }
    const auto type = cfg_.locals[*local].type;
    access(*local,
           cfg_.locals[*local].movable ? access_kind::move : access_kind::read,
           field.span);
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
    stash(temp, eval_opt(index.object.get(), use_mode::read));
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
            ? &dynamic_cast<const ast::binding_pattern &>(*front.pattern)
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

  /// A call: the callee and receiver, then each argument in order, then the
  /// call itself. Every borrow made along the way is held by the call's
  /// temporary until the call runs, so later arguments see it. A `mut self`
  /// (or `&mut` first-parameter) receiver is two-phase: only a shared
  /// reservation while the arguments run, activated as a real `&mut` when the
  /// call does — which lets `xs.set(i, xs.get(j))` through while
  /// `xs.set(i, take(&mut xs))` still conflicts.
  auto eval_call(const ast::call_expr &call) -> value {
    const auto temp = new_temp(local_role::call_temp);
    const ast::expr *activate = nullptr;
    auto reservation = k_no_loan;

    const auto resolved = checked_.resolved_callees.find(&call);
    if (resolved != checked_.resolved_callees.end() &&
        resolved->second.receiver != nullptr) {
      const auto &receiver = *resolved->second.receiver;
      switch (receiver_mode(resolved->second)) {
      case receiver_passing::move:
        stash(temp, eval(receiver, use_mode::move));
        break;
      case receiver_passing::read:
        stash(temp, eval(receiver, use_mode::read));
        break;
      case receiver_passing::shared:
        stash(temp, borrow_place(receiver, false, loan_origin::receiver,
                                 receiver.span));
        break;
      case receiver_passing::mut: {
        auto reserved =
            borrow_place(receiver, false, loan_origin::receiver, receiver.span);
        if (!reserved.loans.empty()) {
          reservation = reserved.loans.front();
          cfg_.loans[reservation].reserved_mut = true;
          activate = &receiver;
        }
        stash(temp, reserved);
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
      const auto is_explicit_borrow =
          argument.kind == ast::node_kind::unary_expr &&
          (dynamic_cast<const ast::unary_expr &>(argument).op ==
               ast::unary_op::addr_of ||
           dynamic_cast<const ast::unary_expr &>(argument).op ==
               ast::unary_op::addr_of_mut);
      if (passing != param_passing::by_value && !is_explicit_borrow &&
          is_place(argument)) {
        // Implicit autoref: a bare place passed to a `&`/`&mut` parameter.
        stash(temp, borrow_place(argument, passing == param_passing::mut_ref,
                                 loan_origin::borrow, argument.span));
      } else {
        stash(temp, eval(*arg.value, use_mode::move));
      }
    }

    if (activate != nullptr) {
      stash(temp, borrow_place(*activate, true, loan_origin::receiver,
                               activate->span, reservation,
                               /*evaluate_subscripts=*/false));
    }
    return compound_result(call, temp);
  }

  /// A lambda: its body is a function of its own; here, only what creating
  /// it does to the enclosing function's locals. A `&`/`&mut` capture-list
  /// entry borrows for as long as the closure lives. Everything else is
  /// captured by value — moved if the body (or `move`) consumes it, copied
  /// otherwise — and a captured value's own borrows travel with it.
  auto eval_lambda(const ast::lambda_expr &lambda) -> value {
    auto nested = cfg_builder(checked_, out_, this);
    nested.build_lambda(lambda);

    auto result = value{};
    const auto capture_by_value = [&](std::string_view name, bool moved,
                                      source_span span) -> void {
      const auto local = lookup(name);
      if (!local.has_value()) {
        note_capture(name, moved, span);
        return;
      }
      const auto &info = cfg_.locals[*local];
      access(*local,
             moved && info.movable ? access_kind::move : access_kind::read,
             span);
      if (bears(info.type)) {
        result.sources.push_back(*local);
      }
    };

    if (lambda.captures.has_value()) {
      for (const auto &entry : *lambda.captures) {
        switch (entry.mode) {
        case ast::capture_mode::by_ref:
        case ast::capture_mode::by_mut_ref: {
          const auto local = lookup(entry.name);
          if (!local.has_value()) {
            note_capture(entry.name, false, entry.span);
            continue;
          }
          const auto is_mut = entry.mode == ast::capture_mode::by_mut_ref;
          access(*local,
                 is_mut ? access_kind::borrow_mut : access_kind::borrow_shared,
                 entry.span);
          result.loans.push_back(
              new_loan(*local, is_mut, loan_origin::capture, entry.span));
          break;
        }
        case ast::capture_mode::by_move:
          capture_by_value(entry.name, true, entry.span);
          break;
        case ast::capture_mode::by_value:
          capture_by_value(entry.name, lambda.is_move, entry.span);
          break;
        }
      }
      return result;
    }
    for (const auto &capture : nested.captures()) {
      capture_by_value(capture.name, capture.moved || lambda.is_move,
                       capture.span);
    }
    return result;
  }

  /// Leaves the function from the current block, returning `returned`:
  /// every open scope ends, then the function exits. `jump` is the `return`
  /// statement, marked for drop scheduling; null for a `?` exit.
  auto emit_return(const value &returned, const ast::node *jump) -> void {
    flow(return_slot_, returned, /*replace=*/true);
    if (jump != nullptr) {
      mark_jump(*jump, 0);
    }
    end_scopes(0);
    goto_block(exit_);
  }

  // ------------------------------------------------------------------
  //  Control flow.
  // ------------------------------------------------------------------

  /// Whether `pattern`, matched against `subject`, binds owned parts.
  [[nodiscard]] auto binds_owned(const ast::expr *subject,
                                 const ast::node *pattern) const -> bool {
    const auto *pat = dynamic_cast<const ast::pattern *>(pattern);
    return subject != nullptr && pat != nullptr &&
           owns_pattern_bindings(*subject, *pat, checked_,
                                 [this](std::string_view name) -> bool {
                                   return lookup(name).has_value();
                                 });
  }

  /// How a `match`/`if let`/`while let`/`let` subject is used. A part of a
  /// local (a field, element, or what a reference points at) is moved out
  /// only when some pattern binds a non-`copy` part of it by value; a
  /// pattern that binds nothing that moves leaves it in place. Any other
  /// subject moves, as before.
  [[nodiscard]] auto subject_mode(const ast::expr *subject,
                                  const std::vector<const ast::node *> &patterns)
      const -> use_mode {
    auto projected = false;
    if (subject == nullptr || !place_root(*subject, projected).has_value() ||
        !projected) {
      return use_mode::move;
    }
    for (const auto *node : patterns) {
      const auto *pat = dynamic_cast<const ast::pattern *>(node);
      if (pat == nullptr) {
        continue;
      }
      for (const auto &binding : collect_pattern_bindings(*pat)) {
        if (binding.node == nullptr || movable(type_of(binding.node))) {
          return use_mode::move;
        }
      }
    }
    return use_mode::read;
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
      auto owned = false;
      if (branch.let_expr != nullptr) {
        // `if let`: the parser leaves a placeholder in `condition`.
        owned = binds_owned(branch.let_expr.get(), branch.let_pattern.get());
        subject = hold_subject(
            eval(*branch.let_expr, subject_mode(branch.let_expr.get(),
                                                {branch.let_pattern.get()})));
      } else {
        static_cast<void>(eval_opt(branch.condition.get(), use_mode::read));
      }
      const auto then = new_block();
      const auto next = new_block();
      fork(then, next);
      current_ = then;
      push_scope(&branch.body);
      if (subject.has_value()) {
        bind_pattern(branch.let_pattern.get(), *subject, owned);
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
      bind_pattern(arm.pattern.get(), subject,
                   binds_owned(owned_subject, arm.pattern.get()));
      if (arm.guard != nullptr) {
        static_cast<void>(eval(*arm.guard, use_mode::read));
        const auto guarded = new_block();
        fork(guarded, next);
        current_ = guarded;
      }
      auto tail = value{};
      if (arm.body_expr != nullptr) {
        tail = eval(*arm.body_expr, use_mode::move);
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
  auto lower_for(const ast::for_stmt &stmt) -> void {
    const auto source = start_loop_source(
        stmt.iterable.get(),
        stmt.iterable != nullptr ? stmt.iterable->span : stmt.span);
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
    for (const auto &pattern : stmt.patterns) {
      bind_pattern(pattern.get(), source, owned);
    }
    if (stmt.guard != nullptr) {
      static_cast<void>(eval(*stmt.guard, use_mode::read));
      const auto kept = new_block();
      fork(kept, head);
      current_ = kept;
    }
    static_cast<void>(lower_body(stmt.body, false));
    pop_scope();
    goto_block(head);
    current_ = exit;
  }

  auto start_loop_source(const ast::expr *iterable, source_span span)
      -> local_id {
    auto name = std::string{};
    if (iterable != nullptr) {
      auto projected = false;
      const auto &stripped = strip_groups(*iterable);
      const auto *operand = &stripped;
      if (stripped.kind == ast::node_kind::unary_expr) {
        const auto &unary = dynamic_cast<const ast::unary_expr &>(stripped);
        if (unary.operand != nullptr) {
          operand = unary.operand.get();
        }
      }
      if (const auto root = place_root(*operand, projected)) {
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
    auto subject = std::optional<local_id>{};
    auto owned = false;
    if (stmt.let_expr != nullptr) {
      owned = binds_owned(stmt.let_expr.get(), stmt.let_pattern.get());
      subject = hold_subject(eval(
          *stmt.let_expr,
          subject_mode(stmt.let_expr.get(), {stmt.let_pattern.get()})));
    } else {
      static_cast<void>(eval_opt(stmt.condition.get(), use_mode::read));
    }
    const auto body = new_block();
    const auto exit = new_block();
    fork(body, exit);
    current_ = body;
    push_loop_scope(head, exit, &stmt.body);
    if (subject.has_value()) {
      bind_pattern(stmt.let_pattern.get(), *subject, owned);
    }
    static_cast<void>(lower_body(stmt.body, false));
    pop_scope();
    goto_block(head);
    current_ = exit;
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
        static_cast<void>(eval(*comp.guard, use_mode::read));
      }
      flow(join, eval_opt(comp.yield_expr.get(), use_mode::move),
           /*replace=*/false);
      return;
    }
    const auto &clause = comp.clauses[index];
    const auto source = start_loop_source(
        clause.iterable.get(),
        clause.iterable != nullptr ? clause.iterable->span : comp.span);
    const auto head = new_block();
    goto_block(head);
    current_ = head;
    emit(use_event{.local = source});
    const auto body = new_block();
    const auto exit = new_block();
    fork(body, exit);
    current_ = body;
    push_loop_scope(head, exit, nullptr);
    for (const auto &pattern : clause.patterns) {
      bind_pattern(pattern.get(), source);
    }
    lower_clause(comp, index + 1, join);
    pop_scope();
    goto_block(head);
    current_ = exit;
  }

  auto lower_where(const ast::where_expr &where) -> value {
    push_scope(&where);
    for (const auto &binding : where.bindings) {
      auto v = eval_opt(binding.value.get(), use_mode::move);
      const auto local = declare(binding.name, type_of(binding.value.get()),
                                 binding.span, /*whole=*/true);
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
      tail = lower_stmt(*stmts[i], want_value && i + 1 == stmts.size());
    }
    return tail;
  }

  auto lower_stmt(const ast::node &node, bool want_value) -> value {
    if (node.has_error) {
      return {};
    }
    switch (node.kind) {
    case ast::node_kind::let_stmt:
      lower_let(dynamic_cast<const ast::let_stmt &>(node));
      return {};

    case ast::node_kind::var_stmt: {
      const auto &stmt = dynamic_cast<const ast::var_stmt &>(node);
      auto v = eval_opt(stmt.initializer.get(), use_mode::move);
      const auto local =
          declare(stmt.name, type_of(&stmt), stmt.span, /*whole=*/true);
      bind_value(local, v);
      return {};
    }

    case ast::node_kind::assign_stmt:
      lower_assign(dynamic_cast<const ast::assign_stmt &>(node));
      return {};

    case ast::node_kind::expr_stmt: {
      const auto &stmt = dynamic_cast<const ast::expr_stmt &>(node);
      auto v = eval_opt(stmt.expr.get(), use_mode::move);
      return want_value ? v : value{};
    }

    case ast::node_kind::return_stmt: {
      const auto &stmt = dynamic_cast<const ast::return_stmt &>(node);
      emit_return(eval_opt(stmt.value.get(), use_mode::move), &node);
      start_unreachable();
      return {};
    }

    case ast::node_kind::break_stmt:
    case ast::node_kind::continue_stmt: {
      const auto is_break = node.kind == ast::node_kind::break_stmt;
      for (auto i = scopes_.size(); i > 0; --i) {
        if (scopes_[i - 1].is_loop) {
          mark_jump(node, i - 1);
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
      const auto &stmt = dynamic_cast<const ast::if_stmt &>(node);
      return lower_if(stmt.branches, stmt.else_body, want_value);
    }

    case ast::node_kind::while_stmt:
      lower_while(dynamic_cast<const ast::while_stmt &>(node));
      return {};

    case ast::node_kind::scope_stmt:
      static_cast<void>(
          lower_scoped_body(dynamic_cast<const ast::scope_stmt &>(node).body,
                            /*want_value=*/false));
      return {};

    case ast::node_kind::for_stmt:
      lower_for(dynamic_cast<const ast::for_stmt &>(node));
      return {};

    case ast::node_kind::match_stmt: {
      const auto &stmt = dynamic_cast<const ast::match_stmt &>(node);
      return lower_match(stmt.subject.get(), stmt.arms, want_value);
    }

    case ast::node_kind::crew_stmt:
      static_cast<void>(lower_scoped_body(
          dynamic_cast<const ast::crew_stmt &>(node).body, false));
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
      const auto &decl = dynamic_cast<const ast::func_decl &>(node);
      if (!decl.modifiers.is_intrinsic) {
        auto nested = cfg_builder(checked_, out_, nullptr);
        nested.build_function(decl);
      }
      return {};
    }

    case ast::node_kind::asm_stmt:
      return {};

    default:
      if (const auto *expr = dynamic_cast<const ast::expr *>(&node)) {
        auto v = eval(*expr, use_mode::move);
        return want_value ? v : value{};
      }
      // A nested item (type, `use`, `static`, ...) has no runtime effect on
      // this function's locals.
      return {};
    }
  }

  auto lower_let(const ast::let_stmt &stmt) -> void {
    auto v = eval_opt(stmt.initializer.get(),
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
      bind_pattern(stmt.pattern.get(), subject,
                   binds_owned(stmt.initializer.get(), stmt.pattern.get()));
      return;
    }
    if (stmt.pattern->kind == ast::node_kind::binding_pattern) {
      const auto &binding =
          dynamic_cast<const ast::binding_pattern &>(*stmt.pattern);
      const auto local =
          declare(binding.name, type_of(stmt.pattern.get()), binding.span,
                  /*whole=*/true);
      bind_value(local, v);
      return;
    }
    bind_pattern(stmt.pattern.get(), hold_subject(v),
                 binds_owned(stmt.initializer.get(), stmt.pattern.get()));
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
    const auto root = place_root(target, projected);
    eval_subscripts(target);
    const auto local = lookup(root->name);
    if (!local.has_value()) {
      note_capture(root->name, false, root->span);
      return;
    }
    if (!projected && stmt.op == ast::assign_op::assign) {
      access(*local, access_kind::write_whole, target.span);
      bind_value(*local, v);
      return;
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

} // namespace

auto owns_pattern_bindings(type_id subject_type, const ast::node &pattern,
                           const checked_types &checked) -> bool {
  const auto *pat = dynamic_cast<const ast::pattern *>(&pattern);
  return pat != nullptr && pattern_bindings_are_disjoint(*pat) &&
         !checked.types.is_unknown(subject_type) &&
         subject_type != k_error_type &&
         checked.types.entry(subject_type).kind != type_kind::ref_kind &&
         !checked.types.is_view(subject_type);
}

auto for_variable_owns(const ast::for_stmt &stmt, const checked_types &checked)
    -> bool {
  if (stmt.iterable == nullptr || stmt.patterns.size() != 1 ||
      stmt.patterns.front() == nullptr ||
      stmt.patterns.front()->kind != ast::node_kind::binding_pattern) {
    return false;
  }
  auto element = k_unknown_type;
  if (const auto it = checked.for_iterator_dispatches.find(&stmt);
      it != checked.for_iterator_dispatches.end()) {
    if (it->second.adapter_decl != nullptr) {
      return false;
    }
    element = it->second.element_type;
  } else {
    const auto type_it = checked.node_types.find(stmt.iterable.get());
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
  return owns_pattern_bindings(element, *stmt.patterns.front(), checked);
}

auto owns_pattern_bindings(const ast::expr &subject, const ast::pattern &pattern,
                           const checked_types &checked,
                           const std::function<bool(std::string_view)> &is_local)
    -> bool {
  const auto type_it = checked.node_types.find(&subject);
  if (type_it == checked.node_types.end() ||
      !owns_pattern_bindings(type_it->second, pattern, checked)) {
    return false;
  }
  auto projected = false;
  const auto root = place_root(subject, projected);
  if (!root.has_value()) {
    return true; // a call result, constructor or literal: a fresh value
  }
  return !projected && root->name != "self" && is_local(root->name);
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
