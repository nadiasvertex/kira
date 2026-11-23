// The ownership graph built from the typed core (spec/unified-checker-plan.md,
// phase 7). See `ownership.h`.
//
// The graph records, in evaluation order, every event that matters to
// ownership: each access to a local, each flow of borrows from one holder to
// another, each scope exit and assignment drop scheduling reads, and each
// write or move the rules forbid. The rules are the ones `function_cfg`
// documents; this file only says which core construct makes which event.
//
// Keys. Drop scheduling and HIR lowering name scopes, bindings, moves and
// temporaries by the AST node a core node was elaborated from
// (`node::origin`), so every key recorded here is an origin read off the
// core. A task's moved capture is keyed by its `core::capture`.

#include "src/core/ownership.h"

#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "src/parser/ast.h"
#include "src/parser/ast_traversal.h"
#include "src/semantic/binding_walk.h"
#include "src/semantic/ownership_check.h"

namespace cinder::core {
namespace {

using semantic::type_kind;
using semantic::ownership::access_event;
using semantic::ownership::access_kind;
using semantic::ownership::assign_event;
using semantic::ownership::block_id;
using semantic::ownership::flow_event;
using semantic::ownership::function_cfg;
using semantic::ownership::invalid_move_event;
using semantic::ownership::k_no_loan;
using semantic::ownership::k_no_local;
using semantic::ownership::let_write_event;
using semantic::ownership::loan_id;
using semantic::ownership::loan_info;
using semantic::ownership::loan_origin;
using semantic::ownership::local_id;
using semantic::ownership::local_info;
using semantic::ownership::local_role;
using semantic::ownership::move_block;
using semantic::ownership::owned_capture;
using semantic::ownership::scope_exit_event;
using semantic::ownership::shared_write_event;
using semantic::ownership::static_write;
using semantic::ownership::static_write_event;
using semantic::ownership::task_capture;
using semantic::ownership::task_capture_write_event;
using semantic::ownership::use_event;

/// The borrows an evaluated expression carries: loans it made itself, plus
/// whatever the `sources` holders carry at this point.
struct value {
  std::vector<loan_id> loans;
  std::vector<local_id> sources;

  [[nodiscard]] auto empty() const -> bool {
    return loans.empty() && sources.empty();
  }
};

/// One step of a resolved place: a projection without its subscript.
struct step_info {
  projection::step kind = projection::step::field;
  type_id type = k_unknown_type;
  std::string name;
};

/// One step of a place, named as a field path names it: a tuple element
/// by its position (`u.1`).
[[nodiscard]] auto step_of(const projection &step) -> step_info {
  return step_info{.kind = step.kind,
                   .type = step.type,
                   .name = step.kind == projection::step::tuple_element &&
                                   step.name.empty()
                               ? std::to_string(step.index)
                               : step.name};
}

/// A place with aliases looked through: where it is rooted in this graph,
/// and every step from there.
struct resolved_place {
  enum class root : std::uint8_t {
    local,   ///< A local of this graph (a temporary included).
    capture, ///< A capture of the enclosing lambda or task.
    global,  ///< A reified `static`.
  };
  root kind = root::local;
  local_id local = k_no_local;
  symbol_id capture = semantic::k_invalid_symbol_id;
  std::string name; ///< The root's spelling.
  type_id root_type = k_unknown_type;
  std::vector<step_info> steps;
  /// The root is a temporary holding a user `index`/`deref` result.
  bool projection_temp = false;
  /// The callee name of that `index`/`deref` call.
  std::string projection_callee;
  /// The type whose `deref`/`index` call made the projection temporary.
  type_id projection_owner = semantic::k_unknown_type;
  /// The place was named through an alias binding: how it is spelled.
  std::string alias_name;
  std::size_t alias_depth = 0; ///< Steps that belong to the alias itself.
  /// The alias may be written through (`pattern_binding::writable`).
  bool alias_writable = true;
  source_span alias_span = source_span::dummy();
  bool alias_by_pattern = false;

  [[nodiscard]] auto type() const -> type_id {
    return steps.empty() ? root_type : steps.back().type;
  }
};

/// What a captured name stands for inside a lambda or task body.
struct capture_info {
  std::string name;
  capture::mode how = capture::mode::copy;
  type_id type = k_unknown_type;
  /// The local it captures, in the enclosing graph, when it is one: what a
  /// write is checked against (`let`, read-only indirections).
  std::optional<local_info> outer;
};

[[nodiscard]] auto base_name(std::string_view name) -> std::string {
  return std::string(name.substr(0, name.find('#')));
}

class graph_builder {
public:
  graph_builder(const semantic::checked_types &checked,
                std::vector<function_cfg> &out, const graph_builder *outer)
      : checked_(checked), out_(out), outer_(outer) {}

  auto build_function(const function &fn) -> void {
    begin(fn.decl);
    for (const auto &p : fn.params) {
      declare_param(p);
    }
    in_task_ = fn.coroutine == coroutine_kind::task;
    if (fn.coroutine != coroutine_kind::none && fn.start != nullptr) {
      // A generator or task dropped before it first runs returns at once.
      cancel_point(fn.start);
    }
    finish(*fn.body);
  }

  auto build_lambda(const lambda &made, bool report_captures) -> void {
    report_captures_ = report_captures;
    begin(made.closure_origin);
    for (const auto &c : made.captures) {
      symbols_.insert_or_assign(c.symbol, sym{.kind = sym::form::capture,
                                              .local = k_no_local,
                                              .alias = {},
                                              .capture = capture_of(c)});
    }
    for (const auto &p : made.params) {
      declare_param(p);
    }
    finish(*made.body);
  }

  /// A task block's body, as the function its task runs: a capture it owns
  /// is a local of its own, keyed by its `core::capture`; any other stays a
  /// name of the enclosing function, which the task only borrows.
  auto build_task(const lambda &made, bool report_captures) -> void {
    report_captures_ = report_captures;
    task_block_ = true;
    in_task_ = true;
    begin(made.origin);
    for (const auto &c : made.captures) {
      if (c.how == capture::mode::move) {
        const auto local =
            declare(c.from.root.name, c.type, made.span, /*whole=*/true, &c);
        symbols_.insert_or_assign(c.symbol, sym{.kind = sym::form::local,
                                                .local = local,
                                                .alias = {},
                                                .capture = {}});
        continue;
      }
      symbols_.insert_or_assign(c.symbol, sym{.kind = sym::form::capture,
                                              .local = k_no_local,
                                              .alias = {},
                                              .capture = capture_of(c)});
    }
    if (made.task_start != nullptr) {
      // A task dropped before it first runs returns at once.
      cancel_point(made.task_start);
    }
    finish(*made.body);
  }

private:
  struct sym {
    enum class form : std::uint8_t { local, alias, capture };
    form kind = form::local;
    local_id local = k_no_local;
    resolved_place alias;
    capture_info capture;
  };

  struct scope {
    const void *key = nullptr;
    std::vector<local_id> owned{};
    bool is_loop = false;
    block_id continue_target = 0;
    block_id break_target = 0;
    std::size_t temporary_depth = 0;
  };

  struct temporary_frame {
    const void *key = nullptr;
    std::vector<local_id> temps;
  };

  const semantic::checked_types &checked_;
  std::vector<function_cfg> &out_;
  const graph_builder *outer_;
  bool report_captures_ = true;
  bool task_block_ = false;
  bool in_task_ = false;
  function_cfg cfg_;
  std::vector<scope> scopes_;
  std::unordered_map<symbol_id, sym> symbols_;
  block_id current_ = 0;
  block_id exit_ = 0;
  local_id return_slot_ = 0;
  std::map<std::pair<local_id, std::string>, local_id> field_paths_;
  std::vector<temporary_frame> temporaries_;
  /// The `crew_tasks` holder of each open crew, by the crew's symbol.
  std::unordered_map<symbol_id, local_id> crews_;
  /// For each generic lambda already bound in this graph, the loans its
  /// first copy took: the later copies carry them instead of borrowing again.
  std::unordered_map<const ast::lambda_expr *, std::vector<loan_id>>
      generic_loans_;
  /// The temporary that holds each `user index/deref` result root.
  /// While an `index_set` call's receiver is checked.
  bool storing_ = false;
  /// While an index call's inserted `&` receiver is borrowed: the access
  /// is reported as a read of the collection.
  bool index_read_ = false;
  /// While a call's `&mut` receiver is evaluated: the call's method.
  std::string receiver_of_;
  std::set<local_id> projection_temps_;
  /// The subset whose call reads a `static` (`nested[0]`).
  std::set<local_id> projection_statics_;
  std::unordered_map<local_id, std::string> projection_callees_;

  // ------------------------------------------------------------------
  //  Graph primitives.
  // ------------------------------------------------------------------

  auto begin(const void *key) -> void {
    current_ = new_block();
    exit_ = new_block();
    return_slot_ = new_local("the returned value", local_role::return_slot,
                             k_unknown_type, source_span::dummy(), false);
    push_scope(key);
  }

  auto finish(const block &body) -> void {
    // The body is a scope of its own inside the parameters' scope, keyed as
    // lowering keys the body block.
    if (body.origin != nullptr && body.origin != scopes_.back().key) {
      push_scope(body.origin);
    }
    auto tail = eval_block(body);
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

  auto emit(semantic::ownership::event e) -> void {
    cfg_.blocks[current_].events.push_back(std::move(e));
  }

  auto goto_block(block_id target) -> void {
    cfg_.blocks[current_].successors.push_back(target);
  }

  auto fork(block_id a, block_id b) -> void {
    goto_block(a);
    goto_block(b);
  }

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

  auto new_temp(local_role role) -> local_id {
    const auto temp = new_local({}, role, k_unknown_type, source_span::dummy(),
                                false);
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
              loan_id exempt = k_no_loan, bool projected = false,
              const void *node = nullptr) -> void {
    emit(access_event{.local = local,
                      .kind = kind,
                      .span = span,
                      .exempt = exempt,
                      .projected = projected,
                      .node = node});
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

  auto stash(local_id temp, const value &v) -> void {
    flow(temp, v, /*replace=*/false);
  }

  auto bind_value(local_id local, const value &v) -> void {
    flow(local, bears(cfg_.locals[local].type) ? v : value{},
         /*replace=*/true);
  }

  // ------------------------------------------------------------------
  //  Types.
  // ------------------------------------------------------------------

  [[nodiscard]] auto known(type_id type) const -> bool {
    return !checked_.types.is_unknown(type);
  }

  [[nodiscard]] auto kind_of(type_id type) const -> std::optional<type_kind> {
    if (!known(type)) {
      return std::nullopt;
    }
    return checked_.types.entry(type).kind;
  }

  [[nodiscard]] auto bears(type_id type) const -> bool {
    return !known(type) ||
           checked_.ownership.borrow_bearing_types.contains(type);
  }

  [[nodiscard]] auto referent(type_id type) const -> type_id {
    while (kind_of(type) == type_kind::ref_kind) {
      type = checked_.types.entry(type).result;
    }
    return type;
  }

  [[nodiscard]] auto movable(type_id type) const -> bool {
    return known(type) && !checked_.is_copy(type);
  }

  [[nodiscard]] auto droppable(type_id type) const -> bool {
    return checked_.drop.drop_plans.contains(type);
  }

  [[nodiscard]] auto has_own_drop(type_id type) const -> bool {
    const auto it = checked_.drop.drop_plans.find(type);
    return it != checked_.drop.drop_plans.end() &&
           it->second.own_drop.has_value();
  }

  /// A local's storage ends with its scope: not `self`, a reference, a
  /// view or a raw pointer, which only point at storage owned elsewhere.
  [[nodiscard]] auto owns_storage(type_id type, std::string_view name) const
      -> bool {
    if (name == "self") {
      return false;
    }
    if (kind_of(type) == type_kind::ref_kind) {
      return false;
    }
    return !checked_.types.is_view(referent(type));
  }

  // ------------------------------------------------------------------
  //  Scopes and names.
  // ------------------------------------------------------------------

  auto push_scope(const void *key) -> void {
    scopes_.push_back(scope{.key = key});
  }

  auto end_scopes(std::size_t depth) -> void {
    for (auto i = scopes_.size(); i > depth; --i) {
      for (const auto local : std::views::reverse(scopes_[i - 1].owned)) {
        access(local, access_kind::storage_dead, cfg_.locals[local].span);
      }
    }
  }

  [[nodiscard]] auto exit_group(const scope &s) const -> std::vector<local_id> {
    auto group = std::vector<local_id>{};
    for (const auto local : std::views::reverse(s.owned)) {
      if (cfg_.locals[local].whole) {
        group.push_back(local);
      }
    }
    return group;
  }

  auto mark_scope_exit(const scope &s) -> void {
    if (s.key != nullptr) {
      emit(scope_exit_event{.key = s.key, .groups = {exit_group(s)}});
    }
  }

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

  /// Declares a local. A `whole` one owns its value outright and is
  /// dropped with its scope; `node` is its drop schedule key.
  auto declare(std::string_view name, type_id type, source_span span,
               bool whole, const void *node, bool owned_binding = false)
      -> local_id {
    const auto spelled = base_name(name);
    const auto local =
        new_local(spelled, local_role::binding, type, span, movable(type));
    auto &info = cfg_.locals[local];
    info.whole = whole;
    info.node = node;
    info.owns = (whole || owned_binding) && owns_storage(type, spelled) &&
                kind_of(type) != type_kind::ptr_kind;
    if (owns_storage(type, spelled)) {
      scopes_.back().owned.push_back(local);
    }
    return local;
  }

  auto declare_param(const param &p) -> void {
    const auto local = declare(p.name, p.type, p.span, /*whole=*/true,
                               p.origin);
    symbols_.insert_or_assign(p.symbol, sym{.kind = sym::form::local,
                                            .local = local,
                                            .alias = {},
                                            .capture = {}});
  }

  [[nodiscard]] auto capture_of(const capture &c) const -> capture_info {
    auto info = capture_info{.name = base_name(c.from.root.name),
                             .how = c.how,
                             .type = c.type,
                             .outer = std::nullopt};
    if (outer_ != nullptr) {
      info.outer = outer_->local_info_of(c.from.root.symbol);
    }
    return info;
  }

  /// The local `symbol` names in this graph, or what the capture it names
  /// was taken from.
  [[nodiscard]] auto local_info_of(symbol_id symbol) const
      -> std::optional<local_info> {
    const auto found = symbols_.find(symbol);
    if (found == symbols_.end()) {
      return std::nullopt;
    }
    switch (found->second.kind) {
    case sym::form::local:
      return cfg_.locals[found->second.local];
    case sym::form::alias:
      if (found->second.alias.kind == resolved_place::root::local) {
        return cfg_.locals[found->second.alias.local];
      }
      return std::nullopt;
    case sym::form::capture:
      return found->second.capture.outer;
    }
    return std::nullopt;
  }

  auto field_path_local(local_id root, std::span<const step_info> steps)
      -> local_id {
    auto parent = root;
    for (const auto &step : steps) {
      const auto key = std::pair{parent, step.name};
      auto it = field_paths_.find(key);
      if (it == field_paths_.end()) {
        const auto path = new_local(
            std::format("{}.{}", cfg_.locals[parent].name, step.name),
            local_role::field_path, step.type, cfg_.locals[root].span,
            /*movable=*/movable(step.type));
        cfg_.locals[path].parent = parent;
        cfg_.locals[parent].children.push_back(path);
        it = field_paths_.emplace(key, path).first;
      }
      parent = it->second;
    }
    return parent;
  }

  // ------------------------------------------------------------------
  //  Places.
  // ------------------------------------------------------------------

  /// Resolves `p`, evaluating its root value (a temporary) and its
  /// subscripts. `held` receives what a temporary root's value carries.
  auto resolve(const place &p, bool hold_root_temporary = true)
      -> resolved_place {
    // `*&v` is `v`: a value passed where a reference was coerced back.
    if (p.root.kind == place_root::origin::temporary &&
        p.root.value != nullptr && p.root.value->kind == node_kind::borrow &&
        !p.path.empty() && p.path.front().kind == projection::step::deref) {
      const auto &lent = static_cast<const borrow &>(*p.root.value);
      auto out = resolve(lent.of, hold_root_temporary);
      if (lent.is_mut) {
        static_cast<void>(check_write(out, static_write::borrow_mut, lent.span,
                                      {}, /*projected_only=*/false));
      }
      for (std::size_t i = 1; i < p.path.size(); ++i) {
        const auto &step = p.path[i];
        if (step.subscript != nullptr) {
          static_cast<void>(eval(*step.subscript));
        }
        out.steps.push_back(
            step_of(step));
      }
      return out;
    }
    auto out = resolved_place{};
    switch (p.root.kind) {
    case place_root::origin::global:
      out.kind = resolved_place::root::global;
      out.name = static_spelling(p.root.name);
      out.root_type = p.root.type;
      break;
    case place_root::origin::temporary: {
      out.kind = resolved_place::root::local;
      out.root_type = p.root.type;
      out.local = hold_root(*p.root.value, p.path.empty(), hold_root_temporary);
      out.name = cfg_.locals[out.local].name;
      if (cfg_.locals[out.local].role == local_role::temporary &&
          p.root.value->kind == node_kind::call) {
        // `make_guarded().inner`: the call, as the source spells it.
        const auto &called = static_cast<const call &>(*p.root.value);
        out.name = called.args.empty()
                       ? std::format("{}()", method_spelling(called.target))
                       : callee_spelling(called);
      }
      out.projection_temp = projection_temps_.contains(out.local);
      if (out.projection_temp) {
        out.projection_callee = projection_callees_[out.local];
        // An element is named by what it indexes (`xs`, not `at_ref(…)`).
        const auto &called = static_cast<const call &>(*p.root.value);
        if (!called.args.empty()) {
          out.projection_owner = referent(called.args.front()->type);
        }
        if (auto indexed = indexed_spelling(called);
            indexes(called) && !indexed.empty()) {
          out.name = std::move(indexed);
        }
      }
      break;
    }
    case place_root::origin::local: {
      const auto found = symbols_.find(p.root.symbol);
      if (found == symbols_.end()) {
        // A name this graph does not know: treat it as a local of its own,
        // so the rest of the graph stays sound.
        out.kind = resolved_place::root::local;
        out.root_type = p.root.type;
        out.local = declare(p.root.name, p.root.type, source_span::dummy(),
                            false, nullptr);
        symbols_.insert_or_assign(p.root.symbol,
                                  sym{.kind = sym::form::local,
                                      .local = out.local,
                                      .alias = {},
                                      .capture = {}});
        out.name = cfg_.locals[out.local].name;
        break;
      }
      const auto &entry = found->second;
      switch (entry.kind) {
      case sym::form::local:
        out.kind = resolved_place::root::local;
        out.local = entry.local;
        out.root_type = cfg_.locals[entry.local].type;
        out.name = cfg_.locals[entry.local].name;
        break;
      case sym::form::alias: {
        const auto writable = entry.alias.alias_writable;
        const auto span = entry.alias.alias_span;
        const auto by_pattern = entry.alias.alias_by_pattern;
        out = entry.alias;
        out.alias_name = base_name(p.root.name);
        out.alias_depth = out.steps.size();
        out.alias_writable = writable;
        out.alias_span = span;
        out.alias_by_pattern = by_pattern;
        break;
      }
      case sym::form::capture:
        out.kind = resolved_place::root::capture;
        out.capture = p.root.symbol;
        out.root_type = entry.capture.type;
        out.name = entry.capture.name;
        break;
      }
      break;
    }
    }
    for (const auto &step : p.path) {
      if (step.subscript != nullptr) {
        static_cast<void>(eval(*step.subscript));
      }
      out.steps.push_back(
          step_of(step));
    }
    return out;
  }

  /// The local a place is rooted at, found without evaluating it:
  /// `k_no_local` for a temporary, a capture or a global.
  [[nodiscard]] auto local_of(const place &p) const -> local_id {
    if (p.root.kind != place_root::origin::local) {
      return k_no_local;
    }
    const auto found = symbols_.find(p.root.symbol);
    if (found == symbols_.end()) {
      return k_no_local;
    }
    const auto &entry = found->second;
    if (entry.kind == sym::form::local) {
      return entry.local;
    }
    if (entry.kind == sym::form::alias &&
        entry.alias.kind == resolved_place::root::local) {
      return entry.alias.local;
    }
    return k_no_local;
  }

  /// Evaluates a temporary root and holds it in a local. A borrowed value
  /// that is not a place, or one a field is read from, is a temporary of
  /// the innermost statement (`hold_temporary`); a user `index`/`deref`
  /// result is a reference the place reads through.
  auto hold_root(const expr &root_value, bool whole, bool hold) -> local_id {
    const auto v = eval(root_value);
    if (const auto *called = projection_call(root_value)) {
      const auto temp = new_local(callee_spelling(*called),
                                  local_role::call_temp, root_value.type,
                                  root_value.span, false);
      flow(temp, v, /*replace=*/true);
      projection_temps_.insert(temp);
      if (!called->args.empty() &&
          called->args.front()->kind == node_kind::borrow &&
          static_cast<const borrow &>(*called->args.front()).of.root.kind ==
              place_root::origin::global) {
        projection_statics_.insert(temp);
      }
      projection_callees_.insert_or_assign(
          temp, indexes(*called) ? "index" : "deref");
      return temp;
    }
    if (!hold) {
      const auto temp = new_temp(local_role::subject_temp);
      cfg_.locals[temp].type = root_value.type;
      cfg_.locals[temp].span = root_value.span;
      flow(temp, v, /*replace=*/true);
      return temp;
    }
    static_cast<void>(whole);
    return hold_temporary(root_value, v);
  }

  /// What an index call indexes, as the source spells it; empty when its
  /// receiver is not a place.
  [[nodiscard]] auto indexed_spelling(const call &called) const
      -> std::string {
    if (called.args.empty()) {
      return {};
    }
    const auto &receiver = *called.args.front();
    if (receiver.kind == node_kind::borrow) {
      return place_spelling(static_cast<const borrow &>(receiver).of);
    }
    if (receiver.kind == node_kind::use) {
      // A reference passed as it is (`xs: &list`).
      return place_spelling(static_cast<const use &>(receiver).from);
    }
    return {};
  }

  /// How the source spells `p`, for a diagnostic: `xs`, `self.items`,
  /// `xs[…]`; a call's result as `name(…)`.
  [[nodiscard]] auto place_spelling(const place &p) const -> std::string {
    auto out = std::string{};
    switch (p.root.kind) {
    case place_root::origin::global:
      out = static_spelling(p.root.name);
      break;
    case place_root::origin::local:
      out = base_name(p.root.name);
      break;
    case place_root::origin::temporary:
      if (p.root.value == nullptr) {
        out = "temporary";
      } else if (p.root.value->kind == node_kind::borrow) {
        out = place_spelling(static_cast<const borrow &>(*p.root.value).of);
      } else if (const auto *called = projection_call(*p.root.value);
                 called != nullptr && indexes(*called) &&
                 !indexed_spelling(*called).empty()) {
        out = indexed_spelling(*called) + "[…]";
      } else if (p.root.value->kind == node_kind::call) {
        out = callee_spelling(static_cast<const call &>(*p.root.value));
      } else {
        out = "temporary";
      }
      break;
    }
    for (const auto &step : p.path) {
      if (step.kind == projection::step::field ||
          step.kind == projection::step::tuple_element) {
        out += '.';
        out += step_of(step).name;
      } else if (step.kind == projection::step::element) {
        out += "[…]";
      }
    }
    return out;
  }

  /// The call a temporary root holds when a place reads through its
  /// result: an `index`/`deref` call's `&T`, followed by a `deref` step.
  [[nodiscard]] auto projection_call(const expr &root_value) const
      -> const call * {
    if (root_value.kind != node_kind::call ||
        kind_of(root_value.type) != type_kind::ref_kind) {
      return nullptr;
    }
    return &static_cast<const call &>(root_value);
  }

  /// How the source spells a method or function: `push` for
  /// `list::push$list_int64_`.
  [[nodiscard]] static auto method_spelling(const callee &target)
      -> std::string {
    const auto *decl =
        target.instance_of != nullptr ? target.instance_of : target.decl;
    auto name = decl != nullptr ? decl->name : target.name;
    if (const auto at = name.rfind("::"); at != std::string::npos) {
      name = name.substr(at + 2);
    }
    return name.substr(0, name.find('$'));
  }

  /// How the source spells the reified static `name`.
  [[nodiscard]] auto static_spelling(const std::string &name) const
      -> std::string {
    for (const auto &[decl, def] : checked_.lowering.static_global_defs) {
      if (def.name == name && decl != nullptr) {
        return decl->name;
      }
    }
    return name;
  }

  [[nodiscard]] auto callee_spelling(const call &c) const -> std::string {
    if (!c.args.empty() && c.args.front()->kind == node_kind::borrow) {
      const auto &b = static_cast<const borrow &>(*c.args.front());
      if (b.of.root.kind == place_root::origin::local) {
        return base_name(b.of.root.name);
      }
      if (b.of.root.kind == place_root::origin::global) {
        return static_spelling(b.of.root.name);
      }
    }
    return std::format("{}(…)", method_spelling(c.target));
  }

  /// Whether `r` is reached through a reference it dereferences.
  [[nodiscard]] auto reborrows(const resolved_place &r) const -> bool {
    for (std::size_t i = 0; i < r.steps.size(); ++i) {
      if (r.steps[i].kind == projection::step::deref &&
          kind_of(type_before(r, i)) == type_kind::ref_kind) {
        return true;
      }
    }
    return false;
  }

  /// Whether `c` is `xs[i]` read through `index` or `index_ref`.
  [[nodiscard]] auto reads_element(const call &c) const -> bool {
    if (checked_.types.is_view(c.type)) {
      return false; // `xs[a..b]` makes a view, which borrows `xs`.
    }
    const auto method = method_spelling(c.target);
    return c.target.trait_name == "index" ||
           c.target.trait_name == "index_ref" || method == "at" ||
           method == "at_ref";
  }

  /// Whether a place reading through `c`'s result reads an element of a
  /// collection, rather than through a handle's `deref`.
  [[nodiscard]] static auto indexes(const call &c) -> bool {
    const auto name = method_spelling(c.target);
    return name != "deref" && name != "deref_mut";
  }

  auto hold_temporary(const expr &held_expr, const value &held) -> local_id {
    const auto temp = new_local("temporary", local_role::temporary,
                                held_expr.type, held_expr.span, false);
    flow(temp, held, /*replace=*/true);
    if (temporaries_.empty()) {
      temporaries_.emplace_back();
    }
    temporaries_.back().temps.push_back(temp);
    if (held_expr.origin != nullptr) {
      cfg_.temporary_ends[held_expr.origin] = temporaries_.back().key;
    }
    return temp;
  }

  auto open_temporaries(const void *key) -> void {
    temporaries_.push_back(temporary_frame{.key = key, .temps = {}});
  }

  auto end_temporaries(std::size_t depth) -> void {
    for (auto i = temporaries_.size(); i > depth; --i) {
      for (const auto temp : std::views::reverse(temporaries_[i - 1].temps)) {
        access(temp, access_kind::storage_dead, cfg_.locals[temp].span);
      }
    }
  }

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

  auto eval_full(const expr &e) -> value {
    open_temporaries(e.origin);
    return close_temporaries(eval(e));
  }

  /// The field-path prefix of `r`'s steps: those before any `deref`,
  /// element or variant payload.
  [[nodiscard]] static auto field_prefix(const resolved_place &r)
      -> std::size_t {
    auto count = std::size_t{0};
    for (const auto &step : r.steps) {
      if (step.kind != projection::step::field &&
          step.kind != projection::step::tuple_element) {
        break;
      }
      ++count;
    }
    return count;
  }

  /// The local an access to `r` touches: its deepest field path.
  auto target_of(const resolved_place &r) -> local_id {
    const auto prefix = field_prefix(r);
    if (prefix == 0) {
      return r.local;
    }
    return field_path_local(
        r.local, std::span<const step_info>(r.steps.data(), prefix));
  }

  /// Accesses the local place `r`. A part reached through fields alone is
  /// its own `field_path` local; any other part is a projected access to
  /// the deepest one. The root stays live, since it holds the borrows.
  auto access_place(const resolved_place &r, access_kind kind,
                    source_span span, const void *node = nullptr,
                    loan_id exempt = k_no_loan) -> local_id {
    const auto target = target_of(r);
    const auto projected = field_prefix(r) < r.steps.size();
    access(target, kind, span, exempt, projected, node);
    if (target != r.local) {
      emit(use_event{.local = r.local});
    }
    return target;
  }

  [[nodiscard]] auto spell(const resolved_place &r, std::size_t count) const
      -> std::string {
    auto out = std::string{};
    auto from = std::size_t{0};
    if (!r.alias_name.empty() && count >= r.alias_depth) {
      out = r.alias_name;
      from = r.alias_depth;
    } else {
      out = r.name;
    }
    for (auto i = from; i < count && i < r.steps.size(); ++i) {
      const auto &step = r.steps[i];
      switch (step.kind) {
      case projection::step::field:
      case projection::step::tuple_element:
        out += '.';
        out += step.name;
        break;
      case projection::step::element:
        out += "[…]";
        break;
      case projection::step::deref: {
        const auto next = i + 1 < count && i + 1 < r.steps.size()
                              ? std::optional(r.steps[i + 1].kind)
                              : std::nullopt;
        if (!next.has_value() || *next == projection::step::deref ||
            *next == projection::step::element) {
          // `xs[0]` reads through `*at_ref(xs, 0)`, which the source does
          // not spell; `*h` through `deref` it does.
          if (!(r.projection_temp && i == 0 && r.projection_callee != "deref")) {
            out.insert(0, "*");
          }
        }
        break;
      }
      case projection::step::variant_payload:
        break;
      }
    }
    return out;
  }

  [[nodiscard]] auto type_before(const resolved_place &r, std::size_t i) const
      -> type_id {
    return i == 0 ? r.root_type : r.steps[i - 1].type;
  }

  /// Why the part of `r` that its steps reach cannot be moved out, if it
  /// cannot (ch. 14, Moving out of places).
  [[nodiscard]] auto move_blocker(const resolved_place &r,
                                  source_span span) const
      -> std::optional<invalid_move_event> {
    const auto whole = spell(r, r.steps.size());
    // A raw read (`p[i]`, `*p`) is a bitwise copy whose ownership is the
    // `machine` code's to track, however `p` was reached (ch. 38, Raw
    // memory and ownership).
    for (std::size_t i = 0; i < r.steps.size(); ++i) {
      if ((r.steps[i].kind == projection::step::deref ||
           r.steps[i].kind == projection::step::element) &&
          kind_of(type_before(r, i)) == type_kind::ptr_kind) {
        return std::nullopt;
      }
    }
    for (std::size_t i = 0; i < r.steps.size(); ++i) {
      const auto &step = r.steps[i];
      if (step.kind == projection::step::field ||
          step.kind == projection::step::tuple_element ||
          step.kind == projection::step::variant_payload) {
        continue;
      }
      if (step.kind == projection::step::deref && i == 0 && r.projection_temp) {
        const auto indexed = r.projection_callee == "index" ||
                             r.projection_callee == "index_mut";
        if (indexed) {
          return invalid_move_event{.span = span,
                                    .reason = move_block::element,
                                    .place = whole,
                                    .owner = r.name};
        }
        return invalid_move_event{.span = span,
                                  .reason = move_block::through_deref,
                                  .place = whole,
                                  .owner = checked_.types.display(
                                      known(r.projection_owner)
                                          ? r.projection_owner
                                          : referent(r.root_type))};
      }
      if (step.kind == projection::step::element) {
        return invalid_move_event{.span = span,
                                  .reason = move_block::element,
                                  .place = whole,
                                  .owner = spell(r, i)};
      }
      // A `deref` of a reference: reached through it, the value belongs to
      // whoever lent it. A part reached through it (`self.f`, `r.f`) is a
      // part of a borrowed value.
      const auto next = i + 1 < r.steps.size()
                            ? std::optional(r.steps[i + 1].kind)
                            : std::nullopt;
      if (next.has_value() && *next != projection::step::deref &&
          *next != projection::step::element) {
        return invalid_move_event{.span = span,
                                  .reason = move_block::borrowed,
                                  .place = whole,
                                  .owner = spell(r, i)};
      }
      return invalid_move_event{.span = span,
                                .reason = move_block::deref,
                                .place = whole,
                                .owner = spell(r, i)};
    }
    if (r.kind == resolved_place::root::local && !r.steps.empty()) {
      const auto &root = cfg_.locals[r.local];
      const auto temporary = root.role != local_role::binding;
      if (!temporary && !root.owns) {
        return invalid_move_event{.span = span,
                                  .reason = move_block::borrowed,
                                  .place = whole,
                                  .owner = root.name};
      }
    }
    auto type = r.root_type;
    for (std::size_t i = 0; i < r.steps.size(); ++i) {
      if (has_own_drop(type)) {
        return invalid_move_event{.span = span,
                                  .reason = move_block::own_drop,
                                  .place = spell(r, i + 1),
                                  .owner = checked_.types.display(type)};
      }
      type = r.steps[i].type;
    }
    return std::nullopt;
  }

  /// Reads or moves the place `p` by value. A move of a part that may not
  /// move is reported and read instead.
  auto use_place(const place &p, bool moving, source_span span,
                 const void *node) -> value {
    auto r = resolve(p);
    moving = moving && movable(r.type());
    if (r.kind == resolved_place::root::global) {
      if (moving && !r.steps.empty() && report_captures_) {
        emit(static_write_event{.span = span,
                                .kind = static_write::move,
                                .name = r.name,
                                .method = {}});
      }
      return {};
    }
    if (r.kind == resolved_place::root::capture) {
      if (moving && report_captures_) {
        emit(invalid_move_event{.span = span,
                                .reason = move_block::captured,
                                .place = spell(r, r.steps.size()),
                                .owner = r.name});
      }
      return {};
    }
    if (moving && r.projection_temp && projection_statics_.contains(r.local)) {
      if (report_captures_) {
        emit(static_write_event{.span = span,
                                .kind = static_write::move,
                                .name = r.name,
                                .method = {}});
      }
      return {};
    }
    if (moving && !r.steps.empty()) {
      if (auto blocked = move_blocker(r, span)) {
        emit(std::move(*blocked));
        moving = false;
      } else if (field_prefix(r) < r.steps.size()) {
        // Out of a variant payload or through a raw pointer: a move of
        // part of the local that is not tracked by path.
        access_place(r, access_kind::read, span);
        return bears(r.type()) ? value{.loans = {}, .sources = {r.local}}
                               : value{};
      }
    }
    access_place(r, moving ? access_kind::move : access_kind::read, span,
                 node);
    return bears(r.type()) ? value{.loans = {}, .sources = {r.local}}
                           : value{};
  }

  // ------------------------------------------------------------------
  //  Writes the rules forbid.
  // ------------------------------------------------------------------

  [[nodiscard]] auto indirect(type_id type) const -> bool {
    const auto kind = kind_of(type);
    return kind == type_kind::ref_kind || kind == type_kind::ptr_kind ||
           (known(type) && checked_.types.is_view(type));
  }

  /// Whether changing `r` changes what its root holds behind a reference,
  /// pointer or view rather than the root itself. `through_place` counts
  /// the place's own type too: a method receiver is reached through it.
  [[nodiscard]] auto through_indirection(const resolved_place &r,
                                         bool through_place) const -> bool {
    if (indirect(r.root_type)) {
      return true;
    }
    for (std::size_t i = 0; i < r.steps.size(); ++i) {
      if (r.steps[i].kind == projection::step::deref ||
          ((through_place || i + 1 < r.steps.size()) &&
           indirect(r.steps[i].type))) {
        return true;
      }
    }
    return false;
  }

  /// The read-only indirection a change of `r` would write through, if
  /// any: a `&T`, a `*T` (`include_pointers`), or a shared view.
  [[nodiscard]] auto shared_indirection(const resolved_place &r,
                                        bool through_place,
                                        bool include_pointers) const
      -> std::optional<type_id> {
    auto shared = std::optional<type_id>{};
    const auto cross = [&](type_id type) -> void {
      if (!known(type)) {
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
    cross(r.root_type);
    for (std::size_t i = 0; i < r.steps.size(); ++i) {
      if (through_place || i + 1 < r.steps.size()) {
        cross(r.steps[i].type);
      }
    }
    return shared;
  }

  /// How the source spells the place a write changes: a method receiver
  /// without the `*` its autoref inserted (`v.push(1)`), and an element an
  /// `index_set` call stores into as `v[…]`.
  [[nodiscard]] auto written_spelling(const resolved_place &r,
                                      static_write kind) const
      -> std::string {
    auto count = r.steps.size();
    if (kind == static_write::receiver || storing_) {
      while (count > 0 && r.steps[count - 1].kind == projection::step::deref) {
        --count;
      }
    }
    auto out = spell(r, count);
    if (storing_) {
      out += "[…]";
    }
    return out;
  }

  /// Rejects `kind` on `r`, rooted at `info`, when it changes something
  /// behind a read-only reference, or `info` itself when it is a `let`.
  auto reject_place_write(const local_info &info, const resolved_place &r,
                          static_write kind, source_span span,
                          std::string_view method = {}) -> void {
    if (!report_captures_) {
      return;
    }
    if (const auto shared = shared_indirection(
            r, kind == static_write::receiver, kind != static_write::assign)) {
      emit(shared_write_event{.span = span,
                              .kind = kind,
                              .place = written_spelling(r, kind),
                              .method = std::string(method),
                              .through = checked_.types.display(*shared)});
      return;
    }
    if (kind == static_write::assign || !info.immutable ||
        through_indirection(r, kind == static_write::receiver)) {
      return;
    }
    emit(let_write_event{.span = span,
                         .kind = kind,
                         .name = info.name,
                         .method = std::string(method),
                         .declared = info.span,
                         .by_pattern = info.by_pattern});
  }

  auto reject_alias_write(const resolved_place &r, static_write kind,
                          source_span span, std::string_view method) -> void {
    if (!report_captures_) {
      return;
    }
    if (const auto shared = shared_indirection(
            r, kind == static_write::receiver, true)) {
      emit(shared_write_event{.span = span,
                              .kind = kind,
                              .place = written_spelling(r, kind),
                              .method = std::string(method),
                              .through = checked_.types.display(*shared)});
      return;
    }
    emit(let_write_event{.span = span,
                         .kind = kind,
                         .name = r.alias_name,
                         .method = std::string(method),
                         .declared = r.alias_span,
                         .by_pattern = r.alias_by_pattern});
  }

  /// The checks a write, `&mut` or `mut self` call on `r` gets before it
  /// is recorded: a `static`, a capture a task only borrows, a capture's
  /// own rules, and the place's. False when the write has no local.
  auto check_write(const resolved_place &r, static_write kind,
                   source_span span, std::string_view method,
                   bool projected_only) -> bool {
    switch (r.kind) {
    case resolved_place::root::global:
      if (report_captures_) {
        emit(static_write_event{.span = span,
                                .kind = kind,
                                .name = r.name,
                                .method = std::string(method)});
      }
      return false;
    case resolved_place::root::capture: {
      const auto &entry = symbols_.at(r.capture).capture;
      if (task_block_ && entry.how != capture::mode::move) {
        if (report_captures_) {
          emit(task_capture_write_event{.span = span,
                                        .kind = kind,
                                        .name = entry.name,
                                        .method = std::string(method)});
        }
        return false;
      }
      if (entry.outer.has_value() && (!projected_only || !r.steps.empty())) {
        reject_place_write(*entry.outer, r, kind, span, method);
      }
      return false;
    }
    case resolved_place::root::local:
      if (!r.alias_name.empty() && !r.alias_writable &&
          kind != static_write::assign) {
        // Through a name a pattern binds that may not change: the part of
        // the subject it names (ch. 3).
        reject_alias_write(r, kind, span, method);
        return true;
      }
      if (!projected_only || !r.steps.empty()) {
        reject_place_write(cfg_.locals[r.local], r, kind, span, method);
      }
      return true;
    }
    return false;
  }

  // ------------------------------------------------------------------
  //  Expressions.
  // ------------------------------------------------------------------

  auto eval_opt(const expr *e) -> value {
    return e != nullptr ? eval(*e) : value{};
  }

  /// The value of an expression whose parts were stashed in `temp`.
  auto compound_result(const expr &e, local_id temp) -> value {
    emit(use_event{.local = temp});
    return bears(e.type) ? value{.loans = {}, .sources = {temp}} : value{};
  }

  auto eval(const expr &e) -> value {
    auto result = eval_parts(e);
    if (!bears(e.type)) {
      return {};
    }
    return result;
  }

  auto eval_parts(const expr &e) -> value {
    switch (e.kind) {
    case node_kind::literal:
    case node_kind::function_ref:
      return {};
    case node_kind::use: {
      const auto &u = static_cast<const use &>(e);
      return use_place(u.from, u.how == use::mode::move, e.span, e.origin);
    }
    case node_kind::borrow:
      return eval_borrow(static_cast<const borrow &>(e), loan_origin::borrow);
    case node_kind::call:
      return eval_call(static_cast<const call &>(e));
    case node_kind::primitive: {
      const auto &prim = static_cast<const primitive &>(e);
      const auto temp = new_temp(local_role::call_temp);
      const auto short_circuits =
          prim.binary == ast::binary_op::logical_and ||
          prim.binary == ast::binary_op::logical_or;
      for (std::size_t i = 0; i < prim.operands.size(); ++i) {
        if (short_circuits && i == 1) {
          stash(temp, eval_full(*prim.operands[i]));
        } else {
          stash(temp, eval(*prim.operands[i]));
        }
      }
      return compound_result(e, temp);
    }
    case node_kind::cast:
      return eval_opt(static_cast<const cast &>(e).operand.get());
    case node_kind::tuple_init:
    case node_kind::array_init:
    case node_kind::struct_init: {
      const auto &agg = static_cast<const aggregate_init &>(e);
      const auto temp = new_temp(local_role::call_temp);
      for (const auto &element : agg.elements) {
        if (agg.repeat.has_value() && movable(element->type) &&
            *agg.repeat != 1) {
          emit(invalid_move_event{
              .span = element->span,
              .reason = move_block::fill,
              .place = {},
              .owner = checked_.types.display(element->type)});
        }
        stash(temp, eval(*element));
      }
      return compound_result(e, temp);
    }
    case node_kind::variant_init: {
      const auto &v = static_cast<const variant_init &>(e);
      const auto temp = new_temp(local_role::call_temp);
      for (const auto &part : v.payload) {
        stash(temp, eval(*part));
      }
      return compound_result(e, temp);
    }
    case node_kind::lambda:
      return eval_lambda(static_cast<const lambda &>(e), nullptr);
    case node_kind::block: {
      const auto &b = static_cast<const block &>(e);
      push_scope(b.origin);
      auto tail = eval_block(b);
      const auto join = new_temp(local_role::join_temp);
      flow(join, tail, /*replace=*/true);
      pop_scope();
      if (in_task_ && b.origin != nullptr && is_crew_block(b)) {
        // A task waits for the crew's tasks after its body, and may be
        // cancelled while it does; lowering keys that check by the block.
        cancel_point(b.origin);
      }
      return value{.loans = {}, .sources = {join}};
    }
    case node_kind::if_expr:
      return eval_if(static_cast<const if_expr &>(e));
    case node_kind::match_expr:
      return eval_match(static_cast<const match_expr &>(e));
    case node_kind::await_expr: {
      const auto &a = static_cast<const await_expr &>(e);
      auto result = eval_opt(a.task.get());
      if (in_task_ && e.origin != nullptr) {
        // Dropping the task while it waits here resumes it only to return.
        cancel_point(e.origin);
      }
      return result;
    }
    case node_kind::host_call: {
      const auto &h = static_cast<const host_call &>(e);
      const auto temp = new_temp(local_role::call_temp);
      for (const auto &arg : h.args) {
        stash(temp, eval(*arg));
      }
      return compound_result(e, temp);
    }
    default:
      return {};
    }
  }

  /// A borrow of a place, or of a value that is not one, which is then a
  /// temporary of its statement.
  auto eval_borrow(const borrow &b, loan_origin origin,
                   std::string_view method = {}, loan_id exempt = k_no_loan,
                   bool reserve = false, bool assigns = false) -> value {
    if (checked_.types.is_view(referent(b.type)) && origin == loan_origin::borrow) {
      origin = loan_origin::view;
    }
    // `&mut xs[a..b]` is the view the range call makes: the reference
    // carries the view's borrow of `xs`, and lives as long as it does.
    if (b.of.root.kind == place_root::origin::temporary && b.of.path.empty() &&
        b.of.root.value != nullptr &&
        b.of.root.value->kind == node_kind::call &&
        checked_.types.is_view(b.of.root.value->type)) {
      return eval(*b.of.root.value);
    }
    auto r = resolve(b.of);
    const auto is_mut = b.is_mut && !reserve;
    if (b.is_mut) {
      // `xs[i] = v` through `index_set` is an assignment to `xs[i]`.
      const auto storing = std::exchange(storing_, assigns);
      const auto kind = assigns ? static_write::assign
                        : origin == loan_origin::receiver
                            ? static_write::receiver
                            : static_write::borrow_mut;
      const auto checked_write =
          check_write(r, kind, b.span, assigns ? std::string_view{} : method,
                      /*projected_only=*/false);
      storing_ = storing;
      if (!checked_write) {
        return {};
      }
    } else if (r.kind != resolved_place::root::local) {
      return {};
    }
    // `xs[0]` reads `xs`, though the index call borrows it to do so.
    const auto target = access_place(
        r,
        is_mut                       ? access_kind::borrow_mut
        : std::exchange(index_read_, false) ? access_kind::read
                                     : access_kind::borrow_shared,
        b.span, nullptr, exempt);
    auto result = value{};
    const auto loan = new_loan(target, is_mut, origin, b.span);
    cfg_.loans[loan].reserved_mut = reserve;
    result.loans.push_back(loan);
    // A borrow of a place reached through a reference (`&(*h.deref()).x`)
    // also carries what that reference borrows.
    if (bears(r.type()) || reborrows(r)) {
      result.sources.push_back(r.local);
    }
    return result;
  }

  // ------------------------------------------------------------------
  //  Calls.
  // ------------------------------------------------------------------

  [[nodiscard]] auto signature_decl(const callee &target) const
      -> const ast::func_decl * {
    const auto *decl =
        target.instance_of != nullptr ? target.instance_of : target.decl;
    if (decl == nullptr) {
      return nullptr;
    }
    const auto it = checked_.ownership.instance_templates.find(decl);
    return it != checked_.ownership.instance_templates.end() ? it->second
                                                             : decl;
  }

  [[nodiscard]] auto type_of(const ast::node *node) const -> type_id {
    if (node == nullptr) {
      return k_unknown_type;
    }
    const auto it = checked_.node_types.find(node);
    return it != checked_.node_types.end() ? it->second : k_unknown_type;
  }

  [[nodiscard]] auto unknown_signature_type(type_id type) const -> bool {
    return !known(type) &&
           checked_.types.entry(type).kind != type_kind::type_var_kind;
  }

  [[nodiscard]] static auto is_type_parameter(type_kind kind) -> bool {
    return kind == type_kind::type_param_kind ||
           kind == type_kind::type_var_kind;
  }

  [[nodiscard]] auto collect_type_params(type_id type,
                                         std::unordered_set<type_id> &params)
      const -> bool {
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

  [[nodiscard]] auto reaches_result(type_id type,
                                    const std::unordered_set<type_id> &params)
      const -> bool {
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

  /// The type parameters through which a call to `decl` can hand back a
  /// borrow it was given; `nullopt` when any input's borrows may reach the
  /// result.
  [[nodiscard]] auto result_borrow_params(const ast::func_decl *decl) const
      -> std::optional<std::unordered_set<type_id>> {
    if (decl == nullptr || decl->modifiers.is_async ||
        decl->modifiers.is_generator) {
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
    if (unknown_signature_type(result) || result == semantic::k_error_type ||
        checked_.ownership.own_borrow_bearing_types.contains(result)) {
      return std::nullopt;
    }
    auto params = std::unordered_set<type_id>{};
    if (!collect_type_params(result, params)) {
      return std::nullopt;
    }
    return params;
  }

  /// Whether parameter `index` of `decl` can pass a borrow on to the
  /// result.
  [[nodiscard]] auto param_reaches_result(
      const ast::func_decl *decl, std::size_t index,
      const std::optional<std::unordered_set<type_id>> &flows) const -> bool {
    if (!flows.has_value() || decl == nullptr ||
        index >= decl->params.size()) {
      return true;
    }
    auto type = type_of(decl->params[index].pattern.get());
    const auto implicit = checked_.ownership.implicit_signatures.find(decl);
    if (implicit != checked_.ownership.implicit_signatures.end() &&
        index < implicit->second.params.size() &&
        implicit->second.params[index] != k_unknown_type) {
      type = implicit->second.params[index];
    }
    return reaches_result(type, *flows);
  }

  [[nodiscard]] static auto intrinsic_named(const call &c,
                                            std::string_view name) -> bool {
    return c.target.kind == callee::form::intrinsic && c.target.name == name;
  }

  /// Whether `b` is a `crew` block: `{ let k = crew_new(); body; crew_end(k) }`.
  [[nodiscard]] static auto is_crew_block(const block &b) -> bool {
    if (b.statements.size() != 3 ||
        b.statements.front()->kind != node_kind::let) {
      return false;
    }
    const auto &bound = static_cast<const let &>(*b.statements.front());
    return bound.value != nullptr && bound.value->kind == node_kind::call &&
           intrinsic_named(static_cast<const call &>(*bound.value),
                           "crew_new");
  }

  /// The symbol of the crew a crew intrinsic's first argument borrows.
  [[nodiscard]] static auto crew_symbol(const call &c)
      -> std::optional<symbol_id> {
    if (c.args.empty() || c.args.front()->kind != node_kind::borrow) {
      return std::nullopt;
    }
    const auto &b = static_cast<const borrow &>(*c.args.front());
    if (b.of.root.kind != place_root::origin::local) {
      return std::nullopt;
    }
    return b.of.root.symbol;
  }

  /// A call: the callee value, then each argument in order (the receiver
  /// first), then the call itself. Every borrow made along the way is held
  /// by the call's temporary until the call runs. A two-phase `&mut`
  /// receiver is only a shared reservation while the arguments run, and is
  /// activated as a real `&mut` when the call does. The result carries the
  /// borrows of only those arguments the callee's signature lets reach it.
  auto eval_call(const call &c) -> value {
    if (intrinsic_named(c, "task_block") && c.args.size() == 1 &&
        c.args.front()->kind == node_kind::lambda) {
      return eval_task_block(static_cast<const lambda &>(*c.args.front()));
    }
    const auto temp = new_temp(local_role::call_temp);
    const auto held = new_temp(local_role::call_temp);
    const auto *decl = signature_decl(c.target);
    // A raw pointer carries no borrow: what it points at is the `machine`
    // code's to account for (ch. 38, Raw memory and ownership).
    const auto raw_result = kind_of(c.type) == type_kind::ptr_kind;
    const auto flows = c.target.kind == callee::form::function ||
                               c.target.kind == callee::form::trait_method
                           ? result_borrow_params(decl)
                           : std::nullopt;
    // A call made to reach another call's receiver (`ys[0].push(5)` calls
    // `at_mut` for `push`) is reported as that call.
    const auto via = std::exchange(receiver_of_, {});
    const auto method = !via.empty() && kind_of(c.type) == type_kind::ref_kind
                            ? via
                            : method_spelling(c.target);
    // `v[i] = x` through an `index_set` impl.
    const auto stores =
        c.target.trait_name == "index_set" ||
        method_spelling(c.target) == "set_at";
    if (c.target.value != nullptr) {
      stash(temp, eval(*c.target.value));
    }
    if (c.target.receiver == receiver_mode::consuming &&
        c.target.trait_name == "into_iterator" && c.origin != nullptr) {
      cfg_.consuming_calls.insert(c.origin);
    }
    const auto crew = intrinsic_named(c, "crew_spawn") ? crew_symbol(c)
                                                       : std::nullopt;
    struct activation {
      const borrow *receiver = nullptr;
      loan_id reservation = k_no_loan;
      local_id into = 0;
      bool stores = false; ///< `v[i] = x`: the activation writes `v`.
    };
    auto pending = std::optional<activation>{};
    for (std::size_t i = 0; i < c.args.size(); ++i) {
      const auto &arg = *c.args[i];
      // A task spawned into a crew holds what it borrows until the crew
      // ends, not through the handle `spawn` returns (ch. 28).
      const auto into = !raw_result && !crew.has_value() &&
                                param_reaches_result(decl, i, flows)
                            ? temp
                            : held;
      const auto is_receiver = i == 0 && c.target.receiver != receiver_mode::none;
      if (arg.kind == node_kind::borrow) {
        const auto &b = static_cast<const borrow &>(arg);
        const auto origin = is_receiver && b.inserted ? loan_origin::receiver
                                                      : loan_origin::borrow;
        // `v[i] = x` evaluates `x` before it stores: the store's `&mut v`
        // is reserved while it does, as a two-phase receiver is.
        if (b.is_mut && (b.two_phase || (is_receiver && stores)) &&
            b.of.root.kind != place_root::origin::temporary) {
          receiver_of_ = is_receiver ? method : std::string{};
          auto reserved = eval_borrow(b, origin, method, k_no_loan,
                                      /*reserve=*/true, is_receiver && stores);
          receiver_of_.clear();
          if (!reserved.loans.empty()) {
            pending = activation{.receiver = &b,
                                 .reservation = reserved.loans.front(),
                                 .into = into,
                                 .stores = is_receiver && stores};
          }
          stash(into, reserved);
          continue;
        }
        receiver_of_ = is_receiver && b.is_mut ? method : std::string{};
        const auto element_read = i == 0 && !b.is_mut && reads_element(c);
        index_read_ = element_read;
        auto lent = eval_borrow(b, origin, method, k_no_loan, false,
                                is_receiver && stores);
        index_read_ = false;
        receiver_of_.clear();
        // `xs[i]` by value is a copy of an element: it carries what the
        // collection's contents carry, not the call's loan of `xs`.
        if (const auto owner = local_of(b.of);
            element_read && into == temp &&
            kind_of(c.type) != type_kind::ref_kind && owner != k_no_local) {
          stash(held, lent);
          stash(temp, value{.loans = {}, .sources = {owner}});
          continue;
        }
        stash(into, lent);
        continue;
      }
      auto passed = eval(arg);
      if (crew.has_value() && i == 1) {
        if (const auto found = crews_.find(*crew); found != crews_.end()) {
          stash(found->second, passed);
        }
      }
      stash(into, passed);
    }
    if (pending.has_value()) {
      // The reservation becomes the `&mut` the call takes.
      auto r = resolve(pending->receiver->of, /*hold_root_temporary=*/false);
      if (r.kind == resolved_place::root::local) {
        const auto target = access_place(
            r,
            pending->stores ? access_kind::write_part : access_kind::borrow_mut,
            pending->receiver->span, nullptr, pending->reservation);
        auto activated = value{};
        activated.loans.push_back(new_loan(target, true, loan_origin::receiver,
                                           pending->receiver->span));
        if (bears(r.type())) {
          activated.sources.push_back(r.local);
        }
        stash(pending->into, activated);
      }
    }
    emit(use_event{.local = held});
    if (intrinsic_named(c, "crew_end")) {
      if (const auto symbol = crew_symbol(c).or_else([&] {
            return c.args.empty() || c.args.front()->kind != node_kind::use
                       ? std::optional<symbol_id>{}
                       : std::optional<symbol_id>{
                             static_cast<const use &>(*c.args.front())
                                 .from.root.symbol};
          })) {
        if (const auto found = crews_.find(*symbol); found != crews_.end()) {
          emit(use_event{.local = found->second});
        }
      }
    }
    if (in_task_ && c.origin != nullptr &&
        (intrinsic_named(c, "task_get") || intrinsic_named(c, "task_yield") ||
         intrinsic_named(c, "await_par") || intrinsic_named(c, "await_race"))) {
      // A task waits here, and may be cancelled while it does.
      cancel_point(c.origin);
    }
    return compound_result(c, temp);
  }

  // ------------------------------------------------------------------
  //  Lambdas and tasks.
  // ------------------------------------------------------------------

  /// The generic lambda `made` is a copy of, and whether it is the first.
  [[nodiscard]] auto generic_copy_of(const lambda &made) const
      -> std::optional<std::pair<const ast::lambda_expr *, bool>> {
    if (made.closure_origin == nullptr) {
      return std::nullopt;
    }
    for (const auto &[written, copies] :
         checked_.ownership.generic_lambda_instances) {
      const auto at = std::ranges::find(copies, made.closure_origin);
      if (at != copies.end()) {
        return std::pair{written, at == copies.begin()};
      }
    }
    return std::nullopt;
  }

  /// A lambda: its body is a graph of its own; here, what creating it does
  /// to this graph's locals. A borrowed capture borrows for as long as the
  /// closure lives; a moved one moves in, and one the closure must drop is
  /// recorded for its drop glue. A later copy of a generic lambda carries
  /// the first copy's loans instead of borrowing again.
  auto eval_lambda(const lambda &made, const std::string *bound_to) -> value {
    const auto copy = generic_copy_of(made);
    const auto later_copy =
        copy.has_value() && generic_loans_.contains(copy->first);
    {
      auto nested = graph_builder(checked_, out_, this);
      nested.build_lambda(made, report_captures_ && !later_copy);
    }
    auto result = value{};
    auto loans = std::vector<loan_id>{};
    for (const auto &c : made.captures) {
      auto r = resolve(c.from);
      const auto span = made.span;
      switch (c.how) {
      case capture::mode::copy:
        if (r.kind == resolved_place::root::local) {
          access_place(r, access_kind::read, span);
          if (bears(c.type)) {
            result.sources.push_back(r.local);
          }
        }
        break;
      case capture::mode::move:
        if (r.kind == resolved_place::root::capture) {
          if (report_captures_ && !later_copy && movable(c.type) &&
              !task_block_) {
            emit(invalid_move_event{.span = span,
                                    .reason = move_block::captured,
                                    .place = r.name,
                                    .owner = r.name});
          }
          break;
        }
        if (r.kind != resolved_place::root::local) {
          break;
        }
        access_place(r, movable(c.type) ? access_kind::move : access_kind::read,
                     span, made.closure_origin);
        if (movable(c.type) && droppable(c.type)) {
          cfg_.owned_captures[made.closure_origin].push_back(owned_capture{
              .name = c.from.root.name, .type = c.type, .node = nullptr});
        }
        if (bears(c.type)) {
          result.sources.push_back(r.local);
        }
        break;
      case capture::mode::borrow:
      case capture::mode::borrow_mut: {
        const auto is_mut = c.how == capture::mode::borrow_mut;
        if (is_mut) {
          if (!check_write(r, static_write::borrow_mut, span, {},
                           /*projected_only=*/!made.capture_list)) {
            break;
          }
        } else if (r.kind != resolved_place::root::local) {
          break;
        }
        if (later_copy) {
          break;
        }
        const auto target =
            access_place(r, is_mut ? access_kind::borrow_mut
                                   : access_kind::borrow_shared,
                         span);
        const auto loan = new_loan(target, is_mut, loan_origin::capture, span);
        cfg_.loans[loan].implicit = !made.capture_list;
        if (bound_to != nullptr) {
          cfg_.loans[loan].carrier = *bound_to;
        }
        loans.push_back(loan);
        if (bears(c.type)) {
          result.sources.push_back(r.local);
        }
        break;
      }
      }
    }
    if (copy.has_value()) {
      if (later_copy) {
        const auto &first = generic_loans_.at(copy->first);
        loans.insert(loans.end(), first.begin(), first.end());
      } else {
        generic_loans_.insert_or_assign(copy->first, loans);
      }
    }
    result.loans.insert(result.loans.end(), loans.begin(), loans.end());
    return result;
  }

  /// An `async:` or `on(...)` block: its body runs as a task of its own. A
  /// capture it owns moves in; any other is lent to it, and the task value
  /// holds a shared loan of each one that is not `copy`.
  auto eval_task_block(const lambda &made) -> value {
    {
      auto nested = graph_builder(checked_, out_, this);
      nested.build_task(made, report_captures_);
    }
    auto result = value{};
    auto records = std::vector<task_capture>{};
    for (const auto &c : made.captures) {
      auto r = resolve(c.from);
      records.push_back(task_capture{.name = c.from.root.name,
                                     .type = c.type,
                                     .moved = c.how == capture::mode::move,
                                     .node = &c,
                                     .span = made.span});
      if (r.kind != resolved_place::root::local) {
        continue;
      }
      if (c.how == capture::mode::move) {
        access_place(r, access_kind::move, made.span, made.origin);
      } else if (movable(c.type)) {
        const auto target =
            access_place(r, access_kind::borrow_shared, made.span);
        result.loans.push_back(
            new_loan(target, false, loan_origin::capture, made.span));
      } else {
        access_place(r, access_kind::read, made.span);
      }
      if (bears(c.type)) {
        result.sources.push_back(r.local);
      }
    }
    if (made.origin != nullptr) {
      cfg_.task_captures[made.origin] = std::move(records);
    }
    return result;
  }

  // ------------------------------------------------------------------
  //  Control flow.
  // ------------------------------------------------------------------

  auto emit_return(const value &returned, const void *jump) -> void {
    flow(return_slot_, returned, /*replace=*/true);
    if (jump != nullptr) {
      mark_jump(jump, 0);
    }
    end_temporaries(0);
    end_scopes(0);
    goto_block(exit_);
  }

  auto eval_if(const if_expr &e) -> value {
    if (e.condition != nullptr) {
      static_cast<void>(eval_full(*e.condition));
    }
    const auto join = new_temp(local_role::join_temp);
    const auto end = new_block();
    const auto then = new_block();
    const auto otherwise = new_block();
    fork(then, otherwise);
    current_ = then;
    flow(join, eval_branch(*e.then_branch), /*replace=*/true);
    goto_block(end);
    current_ = otherwise;
    if (e.else_branch != nullptr) {
      flow(join, eval_branch(*e.else_branch), /*replace=*/true);
    }
    goto_block(end);
    current_ = end;
    return value{.loans = {}, .sources = {join}};
  }

  auto eval_branch(const block &b) -> value {
    push_scope(b.origin);
    auto tail = eval_block(b);
    const auto join = new_temp(local_role::join_temp);
    flow(join, tail, /*replace=*/true);
    pop_scope();
    return value{.loans = {}, .sources = {join}};
  }

  /// Whether `pattern`'s bindings are disjoint parts of the subject, as
  /// written: no `as` alias, `|`, `&` or array pattern.
  [[nodiscard]] static auto disjoint(const match_arm &arm) -> bool {
    if (arm.origin == nullptr) {
      return std::ranges::all_of(arm.alternatives, [](const auto &alt) {
        return disjoint_test(alt.test);
      });
    }
    const auto *pattern =
        ast::as<const ast::pattern>(static_cast<const ast::node *>(arm.origin));
    return pattern == nullptr ||
           semantic::pattern_bindings_are_disjoint(*pattern);
  }

  [[nodiscard]] static auto disjoint_test(const pattern_test &test) -> bool {
    if (test.origin != nullptr) {
      const auto *pattern = ast::as<const ast::pattern>(
          static_cast<const ast::node *>(test.origin));
      if (pattern != nullptr) {
        return semantic::pattern_bindings_are_disjoint(*pattern);
      }
    }
    return std::ranges::all_of(test.parts, &disjoint_test);
  }

  /// Records the parts of an owned pattern that nothing binds but that still
  /// need a drop: a `_` at a droppable position, and the droppable fields a
  /// struct pattern's `..` skips.
  auto record_leftovers(const pattern_test &test) -> void {
    for (const auto &part : test.parts) {
      record_leftovers(part);
    }
    if (test.origin == nullptr) {
      return;
    }
    const auto *node = static_cast<const ast::node *>(test.origin);
    switch (node->kind) {
    case ast::node_kind::wildcard_pattern:
      if (droppable(test.subject_type)) {
        cfg_.leftover_drops.insert(test.origin);
      }
      return;
    case ast::node_kind::struct_pattern: {
      const auto &fields = ast::cast<ast::struct_pattern>(*node).fields;
      if (!std::ranges::any_of(fields, &ast::field_pattern::is_rest)) {
        return;
      }
      const auto plan = checked_.drop.drop_plans.find(test.subject_type);
      if (plan == checked_.drop.drop_plans.end()) {
        return;
      }
      for (const auto &[name, type] : plan->second.droppable_fields) {
        if (!std::ranges::any_of(fields, [&](const auto &f) -> bool {
              return !f.is_rest && f.name == name;
            })) {
          cfg_.leftover_drops.insert(test.origin);
          return;
        }
      }
      return;
    }
    default:
      return;
    }
  }

  /// The type of a match's subject as written: without the `deref` steps
  /// the elaboration added.
  [[nodiscard]] static auto source_subject_type(const match_expr &m)
      -> type_id {
    const auto &path = m.subject.path;
    if (m.subject_derefs == 0 || m.subject_derefs > path.size()) {
      return m.subject.type();
    }
    const auto kept = path.size() - m.subject_derefs;
    return kept == 0 ? m.subject.root.type : path[kept - 1].type;
  }

  /// Whether matching hands the match the subject's value to own: its type
  /// is a value rather than a reference or view, and it is a fresh value,
  /// or a local or a field of one that some binding moves out of — not
  /// `self`, a capture or a global. A fresh value is owned even when no
  /// binding moves (`let _ = make()`), so the match drops it.
  [[nodiscard]] auto match_owns_subject(const match_expr &m,
                                        const resolved_place &subject,
                                        bool moves) const -> bool {
    const auto type = source_subject_type(m);
    if (!known(type) || type == semantic::k_error_type ||
        kind_of(type) == type_kind::ref_kind ||
        checked_.types.is_view(type)) {
      return false;
    }
    if (subject.kind != resolved_place::root::local) {
      return false;
    }
    const auto &root = cfg_.locals[subject.local];
    if (root.role != local_role::binding) {
      return true; // a call result, constructor or literal: a fresh value
    }
    return moves && root.name != "self" && subject.alias_name.empty();
  }

  auto eval_match(const match_expr &m) -> value {
    auto moves = false;
    for (const auto &arm : m.arms) {
      for (const auto &alt : arm.alternatives) {
        for (const auto &b : alt.bindings) {
          moves = moves || b.how == pattern_binding::mode::move;
        }
      }
    }
    // A destructuring `let`'s temporaries end with its statement, before
    // the rest of the block its arm holds.
    const auto let_like = m.form == match_form::let_pattern;
    if (let_like) {
      open_temporaries(temporaries_.empty() ? nullptr
                                            : temporaries_.back().key);
    }
    auto subject = resolve(m.subject, /*hold_root_temporary=*/false);
    auto subject_value = value{};
    if (subject.kind == resolved_place::root::local) {
      const auto movable_subject = moves && movable(subject.type());
      if (movable_subject && !subject.steps.empty()) {
        if (auto blocked = move_blocker(subject, m.span)) {
          emit(std::move(*blocked));
        }
      }
      access_place(subject,
                   movable_subject && field_prefix(subject) == subject.steps.size()
                       ? access_kind::move
                       : access_kind::read,
                   m.span, m.subject_origin);
      if (bears(subject.type())) {
        subject_value.sources.push_back(subject.local);
      }
    } else if (subject.kind == resolved_place::root::capture && moves &&
               report_captures_ && movable(subject.type())) {
      emit(invalid_move_event{.span = m.span,
                              .reason = move_block::captured,
                              .place = spell(subject, subject.steps.size()),
                              .owner = subject.name});
    } else if (subject.kind == resolved_place::root::global && moves &&
               movable(subject.type()) && report_captures_) {
      emit(static_write_event{.span = m.span,
                              .kind = static_write::move,
                              .name = subject.name,
                              .method = {}});
    }
    if (let_like) {
      subject_value = close_temporaries(subject_value);
    }
    const auto held = new_temp(local_role::subject_temp);
    flow(held, subject_value, /*replace=*/true);

    const auto owns = match_owns_subject(m, subject, moves);
    const auto join = new_temp(local_role::join_temp);
    const auto end = new_block();
    auto let_owned = std::vector<local_id>{};
    for (std::size_t i = 0; i < m.arms.size(); ++i) {
      const auto &arm = m.arms[i];
      const auto is_last = i + 1 == m.arms.size();
      const auto body = new_block();
      const auto next = is_last ? end : new_block();
      if (is_last) {
        goto_block(body);
      } else {
        fork(body, next);
      }
      current_ = body;
      // A destructuring `let`'s arm holds the rest of its block, which is
      // still the block's scope: its bindings end with the block's other
      // names, in reverse order. They join the block's scope once the
      // match is built, so an `else` arm, which runs without them, does
      // not see them.
      const auto let_arm = let_like && i == 0;
      push_scope(let_arm ? nullptr : arm.body->origin);
      bind_arm(m, arm, subject, held, owns);
      if (arm.guard != nullptr) {
        static_cast<void>(eval_full(*arm.guard));
        const auto guarded = new_block();
        fork(guarded, next);
        current_ = guarded;
      }
      auto tail = eval_arm_body(*arm.body);
      flow(join, tail, /*replace=*/true);
      if (let_arm) {
        let_owned = std::move(scopes_.back().owned);
        scopes_.pop_back();
      } else {
        pop_scope();
      }
      goto_block(end);
      current_ = next;
    }
    if (m.arms.empty()) {
      goto_block(end);
    }
    current_ = end;
    if (!scopes_.empty()) {
      auto &owner = scopes_.back().owned;
      owner.insert(owner.end(), let_owned.begin(), let_owned.end());
    }
    // `or else`: the failure path drops what the operand held there.
    if (m.form == match_form::or_else && m.origin != nullptr &&
        droppable(source_subject_type(m))) {
      cfg_.unmatched_drops.insert(m.origin);
    }
    return value{.loans = {}, .sources = {join}};
  }

  /// An arm body: its statements in the arm's scope, and a value written
  /// as an expression is a full expression of its own.
  auto eval_arm_body(const block &b) -> value {
    if (b.statements.empty() && b.result != nullptr && b.origin == nullptr) {
      return eval_full(*b.result);
    }
    return eval_block(b);
  }

  /// Declares an arm's bindings. Bindings that move parts of an owned
  /// subject own them; an arm whose bindings overlap owns the whole subject
  /// instead, and may bind only copies besides.
  auto bind_arm(const match_expr &m, const match_arm &arm,
                const resolved_place &subject, local_id held, bool owns)
      -> void {
    if (arm.alternatives.empty()) {
      return;
    }
    const auto &alt = arm.alternatives.front();
    const auto is_disjoint = disjoint(arm);
    const auto by_pattern = m.form != match_form::let_pattern;
    const auto owned = owns && is_disjoint;
    if (owned) {
      for (const auto &a : arm.alternatives) {
        record_leftovers(a.test);
      }
      if (arm.origin != nullptr && droppable(source_subject_type(m))) {
        cfg_.unmatched_drops.insert(arm.origin);
      }
    }
    // An alias of the whole value (`@some(_) as whole`) owns it; without
    // one, a `<subject>` local does.
    const auto is_owner = [&](const pattern_binding &b) -> bool {
      return !is_disjoint && b.how == pattern_binding::mode::move &&
             b.path.empty();
    };
    const auto has_owner = std::ranges::any_of(alt.bindings, is_owner);
    const auto owns_whole = owns && !is_disjoint && !has_owner &&
                            droppable(source_subject_type(m)) &&
                            arm.origin != nullptr;
    if (owns_whole) {
      cfg_.owning_subjects.insert(arm.origin);
      const auto whole = declare("<subject>", source_subject_type(m), arm.span,
                                 /*whole=*/true, arm.origin);
      bind_value(whole, value{.loans = {}, .sources = {held}});
    }
    for (const auto &b : alt.bindings) {
      if (b.how == pattern_binding::mode::alias) {
        auto aliased = subject;
        aliased.alias_name.clear();
        aliased.alias_writable = b.writable;
        aliased.alias_span = b.span;
        aliased.alias_by_pattern = by_pattern;
        for (const auto &step : b.path) {
          aliased.steps.push_back(
              step_of(step));
        }
        symbols_.insert_or_assign(b.symbol, sym{.kind = sym::form::alias,
                                                .local = k_no_local,
                                                .alias = std::move(aliased),
                                                .capture = {}});
        continue;
      }
      // An alias of the whole value (`@some(_) as whole`) owns it.
      if (!is_disjoint && b.how == pattern_binding::mode::move &&
          movable(b.type) && !b.path.empty()) {
        emit(invalid_move_event{.span = b.span,
                                .reason = move_block::overlapping,
                                .place = b.name,
                                .owner = {}});
      }
      const auto whole = (owned && droppable(b.type)) || (owns && is_owner(b));
      const auto local =
          declare(b.name, b.type, b.span, whole, b.origin, owned);
      cfg_.locals[local].immutable = !b.writable;
      cfg_.locals[local].by_pattern = by_pattern;
      symbols_.insert_or_assign(b.symbol, sym{.kind = sym::form::local,
                                              .local = local,
                                              .alias = {},
                                              .capture = {}});
      bind_value(local, value{.loans = {}, .sources = {held}});
    }
  }

  auto eval_loop(const while_loop &loop) -> void {
    const auto head = new_block();
    goto_block(head);
    current_ = head;
    const auto depth = temporaries_.size();
    if (loop.condition != nullptr) {
      static_cast<void>(eval_full(*loop.condition));
    }
    const auto body = new_block();
    const auto exit = new_block();
    // A loop with no condition (`while let`, a `for` over an iterator)
    // leaves only through a `break`.
    if (loop.condition != nullptr) {
      fork(body, exit);
    } else {
      goto_block(body);
    }
    current_ = body;
    const auto step = loop.step != nullptr ? new_block() : head;
    scopes_.push_back(scope{.key = loop.body->origin,
                            .owned = {},
                            .is_loop = true,
                            .continue_target = step,
                            .break_target = exit,
                            .temporary_depth = depth});
    static_cast<void>(eval_block(*loop.body));
    pop_scope();
    if (loop.step != nullptr) {
      goto_block(step);
      current_ = step;
      push_scope(loop.step->origin);
      static_cast<void>(eval_block(*loop.step));
      pop_scope();
    }
    goto_block(head);
    current_ = exit;
  }

  // ------------------------------------------------------------------
  //  Statements.
  // ------------------------------------------------------------------

  /// Lowers a block's statements and result in the current scope.
  auto eval_block(const block &b) -> value {
    auto open = false;
    for (const auto &s : b.statements) {
      if (!s->continues_statement || !open) {
        if (open) {
          static_cast<void>(close_temporaries());
        }
        open_temporaries(s->origin);
        open = true;
      }
      eval_stmt(*s);
    }
    if (open) {
      static_cast<void>(close_temporaries());
    }
    if (b.result == nullptr) {
      return {};
    }
    return eval_full(*b.result);
  }

  auto eval_stmt(const stmt &s) -> void {
    switch (s.kind) {
    case node_kind::let:
      eval_let(static_cast<const let &>(s));
      return;
    case node_kind::assign:
      eval_assign(static_cast<const assign &>(s));
      return;
    case node_kind::expr_stmt: {
      const auto &e = *static_cast<const expr_stmt &>(s).value;
      auto v = eval(e);
      if (e.kind != node_kind::use && e.kind != node_kind::block &&
          e.kind != node_kind::if_expr && e.kind != node_kind::match_expr &&
          e.kind != node_kind::lambda) {
        // A value computed and discarded is a temporary of its statement.
        static_cast<void>(hold_temporary(e, v));
      }
      return;
    }
    case node_kind::while_loop:
      eval_loop(static_cast<const while_loop &>(s));
      return;
    case node_kind::return_stmt: {
      const auto &j = static_cast<const jump &>(s);
      auto v = j.value != nullptr ? eval_full(*j.value) : value{};
      emit_return(v, s.origin);
      start_unreachable();
      return;
    }
    case node_kind::break_stmt:
    case node_kind::continue_stmt: {
      const auto is_break = s.kind == node_kind::break_stmt;
      for (auto i = scopes_.size(); i > 0; --i) {
        if (scopes_[i - 1].is_loop) {
          if (s.origin != nullptr) {
            mark_jump(s.origin, i - 1);
          }
          end_temporaries(scopes_[i - 1].temporary_depth);
          end_scopes(i - 1);
          goto_block(is_break ? scopes_[i - 1].break_target
                              : scopes_[i - 1].continue_target);
          break;
        }
      }
      start_unreachable();
      return;
    }
    case node_kind::yield_stmt: {
      const auto &j = static_cast<const jump &>(s);
      if (j.value != nullptr) {
        static_cast<void>(eval_full(*j.value));
      }
      if (j.suspension != nullptr) {
        // Dropping the generator while it is suspended here resumes it
        // only to return.
        cancel_point(j.suspension);
      }
      return;
    }
    case node_kind::contract_check: {
      const auto &check = static_cast<const contract_check &>(s);
      if (check.condition != nullptr) {
        static_cast<void>(eval_full(*check.condition));
      }
      return;
    }
    default:
      return;
    }
  }

  auto eval_let(const let &l) -> void {
    auto v = value{};
    const std::string spelled = base_name(l.name);
    if (l.value != nullptr) {
      if (l.value->kind == node_kind::lambda) {
        v = eval_lambda(static_cast<const lambda &>(*l.value), &spelled);
      } else {
        v = eval(*l.value);
      }
    }
    const auto local = declare(l.name, l.type, l.span, /*whole=*/true,
                               l.binding_origin);
    cfg_.locals[local].immutable = !l.is_mut;
    if (spelled == "<for iterator>") {
      // What a `for` loop iterates, held for the whole loop.
      cfg_.locals[local].role = local_role::loop_source;
    }
    symbols_.insert_or_assign(l.symbol, sym{.kind = sym::form::local,
                                            .local = local,
                                            .alias = {},
                                            .capture = {}});
    if (l.value != nullptr && l.value->kind == node_kind::call &&
        intrinsic_named(static_cast<const call &>(*l.value), "crew_new")) {
      // What the tasks spawned into the crew borrow, held until it ends.
      crews_.insert_or_assign(l.symbol, new_temp(local_role::crew_tasks));
    }
    if (l.value != nullptr) {
      bind_value(local, v);
    }
  }

  auto eval_assign(const assign &a) -> void {
    auto v = eval_opt(a.value.get());
    auto r = resolve(a.target);
    if (!check_write(r, static_write::assign, a.span, {},
                     /*projected_only=*/true)) {
      return;
    }
    if (r.steps.empty()) {
      emit(assign_event{.key = a.origin, .local = r.local});
      access(r.local, access_kind::write_whole, a.span);
      bind_value(r.local, v);
      return;
    }
    const auto prefix = field_prefix(r);
    const auto fields_only = prefix == r.steps.size();
    // A field of a local that owns its value is tracked by path; one reached
    // through a reference holds a value. An element or a raw pointer write
    // is the collection's or the `machine` code's to handle.
    const auto through_ref = std::ranges::none_of(
        std::views::iota(std::size_t{0}, r.steps.size()), [&](std::size_t i) {
          return r.steps[i].kind == projection::step::element ||
                 kind_of(type_before(r, i)) == type_kind::ptr_kind;
        });
    if (!a.compound && through_ref) {
      emit(assign_event{.key = a.origin,
                        .local = fields_only && cfg_.locals[r.local].owns
                                     ? target_of(r)
                                     : k_no_local});
    }
    if (fields_only) {
      access_place(r, a.compound ? access_kind::read : access_kind::write_whole,
                   a.span);
      if (a.compound) {
        access_place(r, access_kind::write_part, a.span);
      }
    } else {
      access_place(r, access_kind::write_part, a.span);
    }
    flow(r.local, v, /*replace=*/false);
  }
};

} // namespace

auto ownership_graphs(const function &fn,
                      const semantic::checked_types &checked)
    -> std::vector<function_cfg> {
  auto out = std::vector<function_cfg>{};
  if (fn.body == nullptr) {
    return out;
  }
  auto builder = graph_builder(checked, out, nullptr);
  builder.build_function(fn);
  // The function's own graph first.
  std::ranges::rotate(out, out.end() - 1);
  return out;
}

namespace {

/// The `deriving` clause each derived method came from, by its declaration:
/// its spans are in the standard library's `deriving.cn`, not the module's
/// file.
auto derived_origins(const semantic::checked_types &checked)
    -> std::unordered_map<const ast::func_decl *, source_span> {
  auto out = std::unordered_map<const ast::func_decl *, source_span>{};
  for (const auto &splice : checked.lowering.synthesized_item_splices) {
    if (splice.impl == nullptr || !splice.derived_from.has_value()) {
      continue;
    }
    for (const auto &item : splice.impl->items) {
      if (const auto *decl = ast::as<const ast::func_decl>(item.get())) {
        out.insert_or_assign(decl, *splice.derived_from);
      }
    }
  }
  return out;
}

/// Reports a diagnostic found in a derived method at the `deriving` clause
/// that made it.
auto relocate(diagnostic d, source_span origin) -> diagnostic {
  for (auto &label : d.labels) {
    label.span = origin;
  }
  d.with_note("the method was generated by this type's `deriving` clause");
  return d;
}

} // namespace

auto check_ownership(const adapted_program &program,
                     const semantic::checked_types &checked,
                     diagnostic_bag &diag, std::vector<bool> &file_has_errors,
                     unsigned skip_from_fileid) -> void {
  const auto origins = derived_origins(checked);
  for (const auto &module : program.modules) {
    if (!module->file_id.has_value()) {
      continue;
    }
    const auto file = *module->file_id;
    if (static_cast<unsigned>(file) >= skip_from_fileid) {
      continue;
    }
    if (static_cast<std::size_t>(file) < file_has_errors.size() &&
        file_has_errors[static_cast<std::size_t>(file)]) {
      continue;
    }
    const auto errors_before = diag.error_count();
    auto seen = std::set<std::pair<std::string, std::uint32_t>>{};
    // Each declaration is checked once: as its template when it has one.
    // An instance has the template's body with its parameters substituted.
    for (const auto &held : module->functions) {
      const auto *fn = held.get();
      if (fn->instance_of != nullptr || fn->body == nullptr) {
        continue;
      }
      auto local = diagnostic_bag{};
      for (const auto &cfg : ownership_graphs(*fn, checked)) {
        semantic::ownership::check_graph(cfg, checked, local, file);
      }
      const auto generated =
          fn->decl != nullptr ? origins.find(fn->decl) : origins.end();
      for (auto &d : local.diagnostics()) {
        const auto start = d.labels.empty() ? 0U : d.labels.front().span.start;
        if (!seen.emplace(d.message, start).second) {
          continue;
        }
        diag.emit(generated != origins.end()
                      ? relocate(std::move(d), generated->second)
                      : d);
      }
    }
    if (diag.error_count() > errors_before &&
        static_cast<std::size_t>(file) < file_has_errors.size()) {
      file_has_errors[static_cast<std::size_t>(file)] = true;
    }
  }
}

} // namespace cinder::core
