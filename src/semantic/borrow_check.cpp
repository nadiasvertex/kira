#include "borrow_check.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kira::semantic {
namespace {

/// Whether the identifier `name` appears anywhere in `node`'s subtree — the
/// last-use test for view liveness. It must never *under*-report (a missed
/// mention would end a view's live region too early and hide a real conflict),
/// so an unrecognized node kind conservatively reports a mention: keeping a
/// view live one statement too long can at worst raise a false positive, never
/// mask an alias. Every construct that can hold an expression is recursed
/// precisely so that safety valve is rarely reached.
[[nodiscard]] auto subtree_mentions(const ast::node &node,
                                    std::string_view name) -> bool;

/// Recurses a statement/expression list for `subtree_mentions`.
[[nodiscard]] auto any_mentions(const std::vector<ast::ptr<ast::node>> &nodes,
                                std::string_view name) -> bool {
  return std::ranges::any_of(nodes, [&](const ast::ptr<ast::node> &n) -> bool {
    return n != nullptr && subtree_mentions(*n, name);
  });
}

[[nodiscard]] auto mentions_opt(const ast::expr *expr, std::string_view name)
    -> bool {
  return expr != nullptr && subtree_mentions(*expr, name);
}

/// Whether `name` appears in any `if`/`elif` branch or the `else` body — shared
/// by `if_expr` and `if_stmt`, which have identical shapes.
[[nodiscard]] auto
branches_mention(const std::vector<ast::if_branch> &branches,
                 const std::vector<ast::ptr<ast::node>> &else_body,
                 std::string_view name) -> bool {
  const auto in_branch = [&](const ast::if_branch &branch) -> bool {
    return mentions_opt(branch.condition.get(), name) ||
           mentions_opt(branch.let_expr.get(), name) ||
           any_mentions(branch.body, name);
  };
  return std::ranges::any_of(branches, in_branch) ||
         any_mentions(else_body, name);
}

/// Whether `name` appears in any match arm — shared by `match_expr` and
/// `match_stmt`.
[[nodiscard]] auto arms_mention(const std::vector<ast::match_arm> &arms,
                                std::string_view name) -> bool {
  return std::ranges::any_of(arms, [&](const ast::match_arm &arm) -> bool {
    return mentions_opt(arm.guard.get(), name) ||
           mentions_opt(arm.body_expr.get(), name) ||
           any_mentions(arm.body_stmts, name);
  });
}

auto subtree_mentions(const ast::node &node, std::string_view name) -> bool {
  switch (node.kind) {
  case ast::node_kind::ident_expr:
    return dynamic_cast<const ast::ident_expr &>(node).name == name;
  case ast::node_kind::literal_expr:
  case ast::node_kind::break_stmt:
  case ast::node_kind::continue_stmt:
    return false;
  case ast::node_kind::group_expr:
    return mentions_opt(dynamic_cast<const ast::group_expr &>(node).inner.get(),
                        name);
  case ast::node_kind::unary_expr:
    return mentions_opt(
        dynamic_cast<const ast::unary_expr &>(node).operand.get(), name);
  case ast::node_kind::cast_expr:
    return mentions_opt(
        dynamic_cast<const ast::cast_expr &>(node).operand.get(), name);
  case ast::node_kind::try_expr:
    return mentions_opt(dynamic_cast<const ast::try_expr &>(node).operand.get(),
                        name);
  case ast::node_kind::binary_expr: {
    const auto &e = dynamic_cast<const ast::binary_expr &>(node);
    return mentions_opt(e.lhs.get(), name) || mentions_opt(e.rhs.get(), name);
  }
  case ast::node_kind::field_expr:
    return mentions_opt(
        dynamic_cast<const ast::field_expr &>(node).object.get(), name);
  case ast::node_kind::index_expr: {
    const auto &e = dynamic_cast<const ast::index_expr &>(node);
    return mentions_opt(e.object.get(), name) ||
           mentions_opt(e.index.get(), name);
  }
  case ast::node_kind::call_expr: {
    const auto &e = dynamic_cast<const ast::call_expr &>(node);
    return mentions_opt(e.callee.get(), name) ||
           std::ranges::any_of(e.args, [&](const ast::call_arg &arg) -> bool {
             return mentions_opt(arg.value.get(), name);
           });
  }
  case ast::node_kind::tuple_expr: {
    const auto &e = dynamic_cast<const ast::tuple_expr &>(node);
    return std::ranges::any_of(e.elements,
                               [&](const ast::ptr<ast::expr> &el) -> bool {
                                 return mentions_opt(el.get(), name);
                               });
  }
  case ast::node_kind::array_expr: {
    const auto &e = dynamic_cast<const ast::array_expr &>(node);
    return std::ranges::any_of(e.elements,
                               [&](const ast::ptr<ast::expr> &el) -> bool {
                                 return mentions_opt(el.get(), name);
                               }) ||
           mentions_opt(e.fill_value.get(), name) ||
           mentions_opt(e.fill_count.get(), name);
  }
  case ast::node_kind::struct_expr: {
    const auto &e = dynamic_cast<const ast::struct_expr &>(node);
    return std::ranges::any_of(
        e.fields, [&](const ast::struct_field_init &field) -> bool {
          return mentions_opt(field.value.get(), name);
        });
  }
  case ast::node_kind::if_expr: {
    const auto &e = dynamic_cast<const ast::if_expr &>(node);
    return branches_mention(e.branches, e.else_body, name);
  }
  case ast::node_kind::match_expr: {
    const auto &e = dynamic_cast<const ast::match_expr &>(node);
    return mentions_opt(e.subject.get(), name) || arms_mention(e.arms, name);
  }
  case ast::node_kind::block_expr:
    return any_mentions(dynamic_cast<const ast::block_expr &>(node).stmts,
                        name);
  case ast::node_kind::lambda_expr: {
    const auto &e = dynamic_cast<const ast::lambda_expr &>(node);
    // A capture list mentions its names as surely as the body does — more
    // so, since a `&mut` entry is where the borrow is actually taken.
    if (e.captures.has_value() &&
        std::ranges::any_of(*e.captures,
                            [&](const ast::lambda_capture &entry) -> bool {
                              return entry.name == name;
                            })) {
      return true;
    }
    return mentions_opt(e.body_expr.get(), name) ||
           any_mentions(e.body_stmts, name);
  }
  case ast::node_kind::let_stmt: {
    const auto &e = dynamic_cast<const ast::let_stmt &>(node);
    return mentions_opt(e.initializer.get(), name) ||
           any_mentions(e.else_body, name);
  }
  case ast::node_kind::var_stmt:
    return mentions_opt(
        dynamic_cast<const ast::var_stmt &>(node).initializer.get(), name);
  case ast::node_kind::assign_stmt: {
    const auto &e = dynamic_cast<const ast::assign_stmt &>(node);
    return mentions_opt(e.target.get(), name) ||
           mentions_opt(e.value.get(), name);
  }
  case ast::node_kind::expr_stmt:
    return mentions_opt(dynamic_cast<const ast::expr_stmt &>(node).expr.get(),
                        name);
  case ast::node_kind::return_stmt:
    return mentions_opt(
        dynamic_cast<const ast::return_stmt &>(node).value.get(), name);
  case ast::node_kind::if_stmt: {
    const auto &e = dynamic_cast<const ast::if_stmt &>(node);
    return branches_mention(e.branches, e.else_body, name);
  }
  case ast::node_kind::while_stmt: {
    const auto &e = dynamic_cast<const ast::while_stmt &>(node);
    return mentions_opt(e.condition.get(), name) ||
           mentions_opt(e.let_expr.get(), name) || any_mentions(e.body, name);
  }
  case ast::node_kind::for_stmt: {
    const auto &e = dynamic_cast<const ast::for_stmt &>(node);
    return mentions_opt(e.iterable.get(), name) ||
           mentions_opt(e.guard.get(), name) || any_mentions(e.body, name);
  }
  case ast::node_kind::match_stmt: {
    const auto &e = dynamic_cast<const ast::match_stmt &>(node);
    return mentions_opt(e.subject.get(), name) || arms_mention(e.arms, name);
  }
  default:
    // An unrecognized construct that might mention `name`: assume it does, so a
    // view is never dropped early.
    return true;
  }
}

/// Peels enclosing parentheses off an expression. `f((&x))` lends `x` exactly
/// like `f(&x)` does, so a borrow wrapped in one or more `group_expr`s is
/// still the borrow it wraps.
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

/// The root binding a place expression ultimately projects out of — the
/// `x` in `x`, `x.field`, `x[i]`, `(x).field`, and so on. Mirrors
/// `checker::assignment_root_ident` (`check.cpp`), which this pass cannot
/// reach directly. Returns an empty string for anything not rooted in a
/// plain binding (a deref, a call result, a literal, ...).
auto root_binding_name(const ast::expr &expr) -> std::string {
  const auto *current = &expr;
  while (true) {
    switch (current->kind) {
    case ast::node_kind::ident_expr:
      return dynamic_cast<const ast::ident_expr &>(*current).name;
    case ast::node_kind::field_expr: {
      const auto &field = dynamic_cast<const ast::field_expr &>(*current);
      if (field.object == nullptr) {
        return {};
      }
      current = field.object.get();
      break;
    }
    case ast::node_kind::index_expr: {
      const auto &index = dynamic_cast<const ast::index_expr &>(*current);
      if (index.object == nullptr) {
        return {};
      }
      current = index.object.get();
      break;
    }
    case ast::node_kind::group_expr: {
      const auto &group = dynamic_cast<const ast::group_expr &>(*current);
      if (group.inner == nullptr) {
        return {};
      }
      current = group.inner.get();
      break;
    }
    default:
      return {};
    }
  }
}

/// A borrow of a named place, collected for the exclusivity check.
struct call_borrow {
  std::string root;    ///< Root binding being borrowed (never empty once kept).
  bool is_mut = false; ///< `&mut` vs `&`.
  /// A two-phase receiver borrow while its own call's arguments are being
  /// evaluated: only *reserved*, not yet active. A reserved borrow tolerates a
  /// coexisting *shared* borrow of the same value (that shared borrow is
  /// released before the receiver's call runs), but still conflicts with a
  /// `&mut` — and when it does, it is a genuine `&mut`, so it reports as one.
  bool reserved = false;
  /// True when this borrow is held by a *view* (`slice`/`mut slice`) that
  /// outlives the statement rather than by an in-flight call argument. It
  /// changes only the diagnostic wording (see `report_exclusivity`); a view
  /// borrow conflicts by exactly the same `borrows_conflict` rule.
  bool is_view = false;
  /// The binding that holds the view, when `is_view` — for the diagnostic and
  /// for last-use liveness tracking.
  std::string via;
  /// True when this borrow is a closure's `&`/`&mut` capture rather than a
  /// `slice`/`cell` view. It conflicts by exactly the same rule; the flag
  /// only changes the diagnostic's wording, since telling a programmer their
  /// closure is a `slice` would be worse than saying nothing.
  bool is_capture = false;
  source_span span = source_span::dummy();
};

/// A view binding that is live across statements, keeping its source
/// collection(s) borrowed. `borrows` has one entry per source root the view
/// was traced back to (usually one, but a view built from several — a struct
/// literal storing two slices, say — borrows all of them). `last_use` is the
/// index, within the declaring block, of the last statement that mentions
/// `binding`; the view is live from its declaration through that statement.
struct live_view {
  std::vector<call_borrow> borrows;
  std::string binding;
  std::size_t last_use = 0;
};

/// Whether two borrows of the same value may not coexist. Any `&mut`
/// participates, except a reserved (two-phase) borrow tolerates a coexisting
/// shared borrow.
[[nodiscard]] auto borrows_conflict(const call_borrow &a, const call_borrow &b)
    -> bool {
  if (a.root != b.root) {
    return false;
  }
  if (!a.is_mut && !b.is_mut) {
    return false; // two shared borrows never conflict
  }
  // A reserved borrow paired with a plain shared borrow is the two-phase case
  // the reservation exists to permit (`xs.set(i, xs.get(j))`).
  if ((a.reserved && !b.is_mut) || (b.reserved && !a.is_mut)) {
    return false;
  }
  return true;
}

/// The borrows one call makes of its own operands, split by how long each
/// stays live relative to that call's *own* argument evaluation.
struct call_borrows {
  /// Borrows of the written arguments — explicit `&`/`&mut` and implicit
  /// autoref of a bare place to a `&`/`&mut` parameter. These are live for
  /// the whole call, including while sibling arguments (and anything nested
  /// in them) are evaluated.
  std::vector<call_borrow> args;
  /// The autoref of a method/UFCS receiver. Modelled two-phase (like Rust):
  /// it is *active* when the call runs — so it participates in the call's own
  /// exclusivity check against `args` and against enclosing borrows — but only
  /// *reserved* while this call's own arguments are evaluated, during which it
  /// behaves as a shared borrow. That is what lets `xs.set(i, xs.get(j))`
  /// type-check while `xs.set(i, take(&mut xs))` does not.
  std::optional<call_borrow> receiver;
};

/// The set of borrows live at a given point — those made by the chain of
/// enclosing calls whose evaluation is still in progress. Passed *down* the
/// walk by value so a borrow is visible exactly to the sub-expressions
/// evaluated while it is live, and to nothing sequenced after it.
using live_set = std::vector<call_borrow>;

/// Walks every checked function and lambda body enforcing the borrow
/// discipline — see `borrow_check.h` for the rules.
class borrow_checker {
public:
  borrow_checker(const checked_types &checked, diagnostic_bag &diag,
                 file_id_type file_id)
      : checked_(checked), diag_(diag), file_id_(file_id) {}

  auto check_function(const ast::func_decl &decl) -> void {
    if (decl.body_expr != nullptr) {
      walk_expr(*decl.body_expr, /*borrow_ok=*/false, {});
    }
    walk_body(decl.body_stmts, {});
  }

  auto check_lambda(const ast::lambda_expr &lambda) -> void {
    if (lambda.body_expr != nullptr) {
      walk_expr(*lambda.body_expr, /*borrow_ok=*/false, {});
    }
    walk_body(lambda.body_stmts, {});
  }

private:
  const checked_types &checked_;
  diagnostic_bag &diag_;
  file_id_type file_id_;

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

  /// A parameter's bound name if it is a simple identifier pattern, else empty
  /// — mirrors `move_checker::param_name_of` (`move_check.cpp`).
  [[nodiscard]] static auto param_name_of(const ast::param &param)
      -> std::string {
    if (param.pattern != nullptr &&
        param.pattern->kind == ast::node_kind::binding_pattern) {
      return dynamic_cast<const ast::binding_pattern &>(*param.pattern).name;
    }
    return {};
  }

  /// Whether a parameter is written with a leading `mut` (`mut self`) — the
  /// mutability flag lives on the binding pattern, not the `param`.
  [[nodiscard]] static auto param_is_mut(const ast::param &param) -> bool {
    return param.pattern != nullptr &&
           param.pattern->kind == ast::node_kind::binding_pattern &&
           dynamic_cast<const ast::binding_pattern &>(*param.pattern).is_mut;
  }

  /// Whether `id` is a view type (`slice[T]` / `mut slice[T]`, `cell[T]` /
  /// `mut cell[T]`) — the borrowing values the language lets outlive a
  /// single call.
  [[nodiscard]] auto is_view_type(type_id id) const -> bool {
    const auto &entry = checked_.types.entry(id);
    return entry.kind == type_kind::builtin_generic_kind &&
           (entry.name == "slice" || entry.name == "slice_mut" ||
            entry.name == "cell" || entry.name == "cell_mut");
  }

  /// Whether `unary` is a plain-reference borrow — an `&`/`&mut` whose result
  /// is a `ref_kind` referring to an ordinary value. A `&`/`&mut
  /// container[a..b]` borrows a *view* instead (its referent is a
  /// `slice`/`slice_mut`), and views are exempt from the escape rule and not
  /// tracked for exclusivity here — so a borrow whose referent is a view is
  /// not a plain-reference borrow.
  [[nodiscard]] auto is_ref_borrow(const ast::unary_expr &unary) const -> bool {
    if (unary.op != ast::unary_op::addr_of &&
        unary.op != ast::unary_op::addr_of_mut) {
      return false;
    }
    const auto type = lookup_type(&unary);
    if (checked_.types.is_unknown(type)) {
      return false;
    }
    const auto &entry = checked_.types.entry(type);
    if (entry.kind != type_kind::ref_kind) {
      return false;
    }
    return !is_view_type(entry.result);
  }

  /// If `expr` (ignoring parentheses) is a plain-reference borrow, returns it;
  /// otherwise `nullptr`.
  [[nodiscard]] auto as_ref_borrow(const ast::expr &expr) const
      -> const ast::unary_expr * {
    const auto &stripped = strip_groups(expr);
    if (stripped.kind != ast::node_kind::unary_expr) {
      return nullptr;
    }
    const auto &unary = dynamic_cast<const ast::unary_expr &>(stripped);
    return is_ref_borrow(unary) ? &unary : nullptr;
  }

  // ------------------------------------------------------------------
  //  Collecting the borrows a single call makes of named places.
  // ------------------------------------------------------------------

  /// The borrow a method/UFCS receiver makes of its own value, if any. The
  /// receiver of `recv.f(...)` is passed as the callee's first parameter; it
  /// is *borrowed* (not moved) when that parameter is `self` (by reference
  /// regardless of `mut self`) or an explicit `&`/`&mut`. Mirrors
  /// `move_checker::receiver_is_moved` — a first parameter with an ordinary
  /// name and a plain (non-reference) type moves its receiver instead, so
  /// there is no borrow to track.
  [[nodiscard]] auto receiver_borrow_of(const ast::call_expr &call) const
      -> std::optional<call_borrow> {
    const auto it = checked_.resolved_callees.find(&call);
    if (it == checked_.resolved_callees.end() || it->second.decl == nullptr) {
      return std::nullopt;
    }
    const auto *receiver = it->second.receiver;
    if (receiver == nullptr) {
      return std::nullopt;
    }
    const auto &decl = *it->second.decl;
    if (decl.params.empty()) {
      return std::nullopt;
    }
    const auto &front = decl.params.front();
    bool is_mut = false;
    if (param_name_of(front) == "self") {
      // `self` is taken by reference regardless of `mut`; `mut self` is a
      // mutable receiver borrow, plain `self` a shared one.
      is_mut = param_is_mut(front);
    } else {
      // A UFCS free function: the receiver only borrows if the first parameter
      // is an explicit reference — otherwise it is moved (no borrow).
      const auto ptype = lookup_type(front.pattern.get());
      if (checked_.types.is_unknown(ptype)) {
        return std::nullopt;
      }
      const auto &pe = checked_.types.entry(ptype);
      if (pe.kind != type_kind::ref_kind) {
        return std::nullopt;
      }
      is_mut = pe.is_mut;
    }
    auto root = root_binding_name(*receiver);
    if (root.empty()) {
      return std::nullopt;
    }
    return call_borrow{
        .root = std::move(root), .is_mut = is_mut, .span = receiver->span};
  }

  /// Adds an entry for every bare place passed by implicit autoref to a
  /// `&`/`&mut` parameter (`f(x)` where `f` takes `&int32`). Explicit borrows
  /// are collected separately; a `&x`/`&mut x` argument is skipped here.
  auto collect_autoref_args(const ast::call_expr &call,
                            const ast::func_decl &decl, bool has_receiver,
                            std::vector<call_borrow> &out) const -> void {
    // The receiver, when present, fills parameter 0; written arguments start
    // at the next parameter.
    const size_t base = has_receiver ? 1 : 0;
    auto next_positional = base;
    for (const auto &arg : call.args) {
      if (arg.value == nullptr) {
        continue;
      }
      auto param_index = size_t{0};
      if (arg.name.has_value()) {
        param_index = decl.params.size();
        for (size_t i = 0; i < decl.params.size(); ++i) {
          if (param_name_of(decl.params[i]) == *arg.name) {
            param_index = i;
            break;
          }
        }
      } else {
        param_index = next_positional++;
      }
      if (param_index >= decl.params.size()) {
        continue;
      }
      if (as_ref_borrow(*arg.value) != nullptr) {
        continue; // explicit borrow, handled by the caller
      }
      const auto ptype = lookup_type(decl.params[param_index].pattern.get());
      if (checked_.types.is_unknown(ptype)) {
        continue;
      }
      const auto &pe = checked_.types.entry(ptype);
      if (pe.kind != type_kind::ref_kind) {
        continue;
      }
      auto root = root_binding_name(*arg.value);
      if (root.empty()) {
        continue;
      }
      out.push_back(call_borrow{.root = std::move(root),
                                .is_mut = pe.is_mut,
                                .span = arg.value->span});
    }
  }

  /// All borrows `call` makes of named places, split into whole-call argument
  /// borrows and the two-phase receiver borrow.
  [[nodiscard]] auto collect_call_borrows(const ast::call_expr &call) const
      -> call_borrows {
    auto result = call_borrows{};
    for (const auto &arg : call.args) {
      if (arg.value == nullptr) {
        continue;
      }
      if (const auto *borrow = as_ref_borrow(*arg.value)) {
        auto root = borrow->operand != nullptr
                        ? root_binding_name(*borrow->operand)
                        : std::string{};
        if (!root.empty()) {
          result.args.push_back(
              call_borrow{.root = std::move(root),
                          .is_mut = borrow->op == ast::unary_op::addr_of_mut,
                          .span = borrow->span});
        }
      }
    }
    if (const auto it = checked_.resolved_callees.find(&call);
        it != checked_.resolved_callees.end() && it->second.decl != nullptr) {
      collect_autoref_args(call, *it->second.decl,
                           it->second.receiver != nullptr, result.args);
    }
    result.receiver = receiver_borrow_of(call);
    return result;
  }

  /// Reports the first exclusivity conflict of each `fresh` borrow against the
  /// borrows already live (`ambient`) and against earlier `fresh` borrows.
  /// Two borrows of the same root conflict when at least one is `&mut`.
  auto check_new_borrows(const live_set &ambient,
                         const std::vector<call_borrow> &fresh) -> void {
    for (size_t j = 0; j < fresh.size(); ++j) {
      const call_borrow *conflict = nullptr;
      for (const auto &earlier : ambient) {
        if (borrows_conflict(earlier, fresh[j])) {
          conflict = &earlier;
          break;
        }
      }
      for (size_t i = 0; conflict == nullptr && i < j; ++i) {
        if (borrows_conflict(fresh[i], fresh[j])) {
          conflict = &fresh[i];
        }
      }
      if (conflict != nullptr) {
        report_exclusivity(*conflict, fresh[j]);
      }
    }
  }

  // ------------------------------------------------------------------
  //  View provenance: the persistent borrows a view-bearing value holds.
  // ------------------------------------------------------------------

  /// Whether `expr`'s recorded type transitively carries a view — see
  /// `checked_types::view_bearing_types`.
  [[nodiscard]] auto is_view_bearing(const ast::expr &expr) const -> bool {
    return checked_.view_bearing_types.contains(lookup_type(&expr));
  }

  /// The mutability of a view type: `mut slice`/`mut cell` (`slice_mut`/
  /// `cell_mut`) after peeling any enclosing references, else shared.
  [[nodiscard]] auto view_is_mut(type_id id) const -> bool {
    const auto *entry = &checked_.types.entry(id);
    while (entry->kind == type_kind::ref_kind) {
      entry = &checked_.types.entry(entry->result);
    }
    return entry->kind == type_kind::builtin_generic_kind &&
           (entry->name == "slice_mut" || entry->name == "cell_mut");
  }

  /// The source roots a view-bearing value borrows and keeps live — the
  /// provenance rules in `borrow_check.h`. `views` is the set of views
  /// currently live, so a value aliasing an existing view binding (`let n = m`,
  /// or a sub-slice `m[a..b]`) inherits that binding's roots rather than
  /// treating the view itself as a fresh collection. Returns empty for a value
  /// carrying no view, or a view whose source cannot be traced to a named root
  /// (conservatively untracked).
  /// The borrows a lambda's explicit capture list makes. A bare `name` entry
  /// is a by-value copy and borrows nothing; `&name` and `&mut name` borrow
  /// the named local for as long as the closure lives.
  ///
  /// A lambda with *no* capture list captures implicitly, and implicit
  /// capture is by value in every lowering today (both backends copy into
  /// the environment block), so there is nothing to track for it here — the
  /// borrow-vs-move distinction `16-closures-and-capture.md` describes for
  /// implicit capture is not enforced anywhere yet.
  [[nodiscard]] static auto capture_borrows(const ast::lambda_expr &lambda)
      -> std::vector<call_borrow> {
    auto out = std::vector<call_borrow>{};
    if (!lambda.captures.has_value()) {
      return out;
    }
    for (const auto &entry : *lambda.captures) {
      if (entry.mode == ast::capture_mode::by_value) {
        continue;
      }
      out.push_back(
          call_borrow{.root = entry.name,
                      .is_mut = entry.mode == ast::capture_mode::by_mut_ref,
                      .is_capture = true,
                      .span = entry.span});
    }
    return out;
  }

  [[nodiscard]] auto provenance_of(const ast::expr &expr,
                                   const live_set &views) const
      -> std::vector<call_borrow> {
    const auto &e = strip_groups(expr);
    // A closure with `&`/`&mut` capture entries borrows those variables and
    // holds the borrow for as long as the closure itself is live — the same
    // shape as a view binding, and tracked the same way. Checked before the
    // `is_view_bearing` gate below: a closure's type is `fn(...)`, so the
    // type alone never says it borrows; only its capture list does.
    if (e.kind == ast::node_kind::lambda_expr) {
      return capture_borrows(dynamic_cast<const ast::lambda_expr &>(e));
    }
    if (!is_view_bearing(e)) {
      return {};
    }
    switch (e.kind) {
    case ast::node_kind::unary_expr: {
      const auto &unary = dynamic_cast<const ast::unary_expr &>(e);
      auto inner = unary.operand != nullptr
                       ? provenance_of(*unary.operand, views)
                       : std::vector<call_borrow>{};
      if (unary.op == ast::unary_op::addr_of_mut) {
        for (auto &borrow : inner) {
          borrow.is_mut = true;
        }
      }
      return inner;
    }
    case ast::node_kind::index_expr: {
      const auto &index = dynamic_cast<const ast::index_expr &>(e);
      if (index.object == nullptr) {
        return {};
      }
      // A sub-slice of an existing view (`m[a..b]`) borrows *m*'s sources, not
      // `m` itself; a slice of a real collection borrows that collection.
      if (is_view_bearing(*index.object)) {
        return provenance_of(*index.object, views);
      }
      auto root = root_binding_name(*index.object);
      if (root.empty()) {
        return provenance_of(*index.object, views);
      }
      return {call_borrow{.root = std::move(root),
                          .is_mut = view_is_mut(lookup_type(&e)),
                          .span = e.span}};
    }
    case ast::node_kind::ident_expr: {
      const auto &ident = dynamic_cast<const ast::ident_expr &>(e);
      auto out = std::vector<call_borrow>{};
      for (const auto &borrow : views) {
        if (borrow.is_view && borrow.via == ident.name) {
          out.push_back(borrow);
        }
      }
      return out;
    }
    case ast::node_kind::field_expr: {
      const auto &field = dynamic_cast<const ast::field_expr &>(e);
      return field.object != nullptr ? provenance_of(*field.object, views)
                                     : std::vector<call_borrow>{};
    }
    case ast::node_kind::cast_expr: {
      const auto &cast = dynamic_cast<const ast::cast_expr &>(e);
      return cast.operand != nullptr ? provenance_of(*cast.operand, views)
                                     : std::vector<call_borrow>{};
    }
    case ast::node_kind::tuple_expr: {
      const auto &tuple = dynamic_cast<const ast::tuple_expr &>(e);
      auto out = std::vector<call_borrow>{};
      for (const auto &element : tuple.elements) {
        append_provenance(out, element.get(), views);
      }
      return out;
    }
    case ast::node_kind::array_expr: {
      const auto &array = dynamic_cast<const ast::array_expr &>(e);
      auto out = std::vector<call_borrow>{};
      for (const auto &element : array.elements) {
        append_provenance(out, element.get(), views);
      }
      append_provenance(out, array.fill_value.get(), views);
      return out;
    }
    case ast::node_kind::struct_expr: {
      const auto &literal = dynamic_cast<const ast::struct_expr &>(e);
      auto out = std::vector<call_borrow>{};
      for (const auto &field : literal.fields) {
        append_provenance(out, field.value.get(), views);
      }
      return out;
    }
    case ast::node_kind::call_expr: {
      // A call whose result carries a view keeps every place reachable through
      // its reference arguments and receiver borrowed for the view's lifetime —
      // the conservative rule that needs no per-argument provenance inference.
      const auto &call = dynamic_cast<const ast::call_expr &>(e);
      const auto borrows = collect_call_borrows(call);
      auto out = borrows.args;
      if (borrows.receiver.has_value()) {
        out.push_back(*borrows.receiver);
      }
      for (const auto &arg : call.args) {
        append_provenance(out, arg.value.get(), views);
      }
      return out;
    }
    default:
      // A view produced by an exotic expression form (comprehension, block,
      // etc.) is not one of the language's ordinary view producers; its source
      // is left untracked. This is a precision gap, not a fresh unsoundness in
      // the channels the design targets (slice, call-return, aggregate store).
      return {};
    }
  }

  /// Appends `expr`'s provenance to `out` (no-op for a null or non-view expr).
  auto append_provenance(std::vector<call_borrow> &out, const ast::expr *expr,
                         const live_set &views) const -> void {
    if (expr == nullptr) {
      return;
    }
    auto more = provenance_of(*expr, views);
    out.insert(out.end(), more.begin(), more.end());
  }

  /// If `node` binds or stores a view-bearing value into a named holder,
  /// registers one live view per source root, checks it against the borrows
  /// already live (`seed`), and computes its last-use extent over the rest of
  /// `stmts`. `index` is `node`'s position within `stmts`.
  auto register_views(const ast::node &node, std::size_t index,
                      const std::vector<ast::ptr<ast::node>> &stmts,
                      const live_set &seed, std::vector<live_view> &block_views)
      -> void {
    std::string holder;
    const ast::expr *initializer = nullptr;
    switch (node.kind) {
    case ast::node_kind::let_stmt: {
      const auto &stmt = dynamic_cast<const ast::let_stmt &>(node);
      if (stmt.pattern != nullptr &&
          stmt.pattern->kind == ast::node_kind::binding_pattern) {
        holder = dynamic_cast<const ast::binding_pattern &>(*stmt.pattern).name;
      }
      initializer = stmt.initializer.get();
      break;
    }
    case ast::node_kind::var_stmt: {
      const auto &stmt = dynamic_cast<const ast::var_stmt &>(node);
      holder = stmt.name;
      initializer = stmt.initializer.get();
      break;
    }
    case ast::node_kind::assign_stmt: {
      const auto &stmt = dynamic_cast<const ast::assign_stmt &>(node);
      if (stmt.target != nullptr) {
        holder = root_binding_name(*stmt.target);
      }
      initializer = stmt.value.get();
      break;
    }
    default:
      return;
    }
    if (holder.empty() || initializer == nullptr) {
      return;
    }
    auto borrows = provenance_of(*initializer, seed);
    if (borrows.empty()) {
      return;
    }
    for (auto &borrow : borrows) {
      borrow.is_view = true;
      borrow.via = holder;
    }
    // A newly created view conflicts with any incompatible borrow already live
    // (another `mut` view of the same collection, say).
    check_new_borrows(seed, borrows);
    auto last_use = index;
    for (auto j = index + 1; j < stmts.size(); ++j) {
      if (stmts[j] != nullptr && subtree_mentions(*stmts[j], holder)) {
        last_use = j;
      }
    }
    block_views.push_back(live_view{.borrows = std::move(borrows),
                                    .binding = std::move(holder),
                                    .last_use = last_use});
  }

  /// Walks `expr`. `borrow_ok` is true only in a *passing* position — a direct
  /// call argument, the callee, or the base of a field/index projection —
  /// where a plain-reference borrow lends its operand rather than escaping. A
  /// borrow found with `borrow_ok` false is stored, and rejected. `live` is
  /// the set of borrows made by enclosing calls still being evaluated, plus the
  /// views live across the enclosing statement, used to enforce exclusivity
  /// across call nesting and against live views.
  auto walk_expr(const ast::expr &expr, bool borrow_ok, const live_set &live)
      -> void {
    if (expr.has_error) {
      return;
    }
    switch (expr.kind) {
    case ast::node_kind::unary_expr: {
      const auto &unary = dynamic_cast<const ast::unary_expr &>(expr);
      if (is_ref_borrow(unary) && !borrow_ok) {
        report_escape(unary);
      }
      // A borrow's operand, a deref's pointee, any unary operand: never a
      // passing position of its own.
      if (unary.operand != nullptr) {
        walk_expr(*unary.operand, /*borrow_ok=*/false, live);
      }
      return;
    }

    case ast::node_kind::group_expr: {
      const auto &group = dynamic_cast<const ast::group_expr &>(expr);
      if (group.inner != nullptr) {
        walk_expr(*group.inner, borrow_ok, live);
      }
      return;
    }

    case ast::node_kind::field_expr: {
      // Projecting through a borrow (`(&x).field`) passes it through for the
      // access; it is not stored, so a borrow as the base is allowed.
      const auto &field = dynamic_cast<const ast::field_expr &>(expr);
      if (field.object != nullptr) {
        walk_expr(*field.object, /*borrow_ok=*/true, live);
      }
      return;
    }

    case ast::node_kind::index_expr: {
      const auto &index = dynamic_cast<const ast::index_expr &>(expr);
      if (index.object != nullptr) {
        walk_expr(*index.object, /*borrow_ok=*/true, live);
      }
      if (index.index != nullptr) {
        walk_expr(*index.index, /*borrow_ok=*/false, live);
      }
      return;
    }

    case ast::node_kind::call_expr: {
      const auto &call = dynamic_cast<const ast::call_expr &>(expr);
      const auto borrows = collect_call_borrows(call);

      // The borrows this call makes are live together when it runs: check them
      // against each other and against the enclosing borrows in `live`.
      auto direct = borrows.args;
      if (borrows.receiver.has_value()) {
        direct.push_back(*borrows.receiver);
      }
      check_new_borrows(live, direct);

      // The callee (and, for a method call, the receiver expression it wraps)
      // is evaluated *before* the arguments, so this call's own argument
      // borrows are not yet live there — only the enclosing borrows are.
      if (call.callee != nullptr) {
        walk_expr(*call.callee, /*borrow_ok=*/true, live);
      }

      // While this call's arguments are evaluated, its whole-call argument
      // borrows are live, and its receiver borrow is *reserved* (two-phase):
      // it tolerates a nested shared borrow of the same value but still
      // conflicts with a nested `&mut` of it.
      auto nested = live;
      nested.insert(nested.end(), borrows.args.begin(), borrows.args.end());
      if (borrows.receiver.has_value()) {
        auto reserved = *borrows.receiver;
        reserved.reserved = true;
        nested.push_back(reserved);
      }
      for (const auto &arg : call.args) {
        if (arg.value != nullptr) {
          walk_expr(*arg.value, /*borrow_ok=*/true, nested);
        }
      }
      return;
    }

    case ast::node_kind::binary_expr: {
      const auto &binary = dynamic_cast<const ast::binary_expr &>(expr);
      if (binary.lhs != nullptr) {
        walk_expr(*binary.lhs, /*borrow_ok=*/false, live);
      }
      if (binary.rhs != nullptr) {
        walk_expr(*binary.rhs, /*borrow_ok=*/false, live);
      }
      return;
    }

    case ast::node_kind::cast_expr: {
      const auto &cast = dynamic_cast<const ast::cast_expr &>(expr);
      if (cast.operand != nullptr) {
        walk_expr(*cast.operand, /*borrow_ok=*/false, live);
      }
      return;
    }

    case ast::node_kind::try_expr: {
      const auto &tri = dynamic_cast<const ast::try_expr &>(expr);
      if (tri.operand != nullptr) {
        walk_expr(*tri.operand, /*borrow_ok=*/false, live);
      }
      return;
    }

    case ast::node_kind::tuple_expr: {
      const auto &tuple = dynamic_cast<const ast::tuple_expr &>(expr);
      for (const auto &element : tuple.elements) {
        if (element != nullptr) {
          walk_expr(*element, /*borrow_ok=*/false, live);
        }
      }
      return;
    }

    case ast::node_kind::array_expr: {
      const auto &array = dynamic_cast<const ast::array_expr &>(expr);
      for (const auto &element : array.elements) {
        if (element != nullptr) {
          walk_expr(*element, /*borrow_ok=*/false, live);
        }
      }
      if (array.fill_value != nullptr) {
        walk_expr(*array.fill_value, /*borrow_ok=*/false, live);
      }
      if (array.fill_count != nullptr) {
        walk_expr(*array.fill_count, /*borrow_ok=*/false, live);
      }
      return;
    }

    case ast::node_kind::struct_expr: {
      const auto &literal = dynamic_cast<const ast::struct_expr &>(expr);
      if (literal.type_name != nullptr) {
        walk_expr(*literal.type_name, /*borrow_ok=*/false, live);
      }
      for (const auto &field : literal.fields) {
        if (field.value != nullptr) {
          walk_expr(*field.value, /*borrow_ok=*/false, live);
        }
      }
      return;
    }

    case ast::node_kind::lambda_expr:
      borrow_checker(checked_, diag_, file_id_)
          .check_lambda(dynamic_cast<const ast::lambda_expr &>(expr));
      return;

    case ast::node_kind::if_expr: {
      const auto &if_e = dynamic_cast<const ast::if_expr &>(expr);
      walk_if_branches(if_e.branches, if_e.else_body, live);
      return;
    }

    case ast::node_kind::match_expr: {
      const auto &match_e = dynamic_cast<const ast::match_expr &>(expr);
      if (match_e.subject != nullptr) {
        walk_expr(*match_e.subject, /*borrow_ok=*/false, live);
      }
      walk_match_arms(match_e.arms, live);
      return;
    }

    case ast::node_kind::block_expr: {
      const auto &block = dynamic_cast<const ast::block_expr &>(expr);
      walk_body(block.stmts, live);
      return;
    }

    default:
      // Identifiers, literals, module paths, and constructs with no
      // sub-expression of interest: nothing to check.
      return;
    }
  }

  /// Walks a statement list in order. `outer` holds the views live from
  /// enclosing blocks; each statement is walked with `outer` plus this block's
  /// own views that are still live at that point (last-use gated). A `let`/
  /// `var`/`assign` that binds a view-bearing value registers a live view for
  /// the rest of the block.
  auto walk_body(const std::vector<ast::ptr<ast::node>> &stmts,
                 const live_set &outer) -> void {
    auto block_views = std::vector<live_view>{};
    for (std::size_t i = 0; i < stmts.size(); ++i) {
      if (stmts[i] == nullptr) {
        continue;
      }
      auto seed = outer;
      for (const auto &view : block_views) {
        if (i <= view.last_use) {
          seed.insert(seed.end(), view.borrows.begin(), view.borrows.end());
        }
      }
      walk_stmt(*stmts[i], seed);
      register_views(*stmts[i], i, stmts, seed, block_views);
    }
  }

  /// Walks one statement. `seed` is the set of views live across it (plus, when
  /// this statement is itself nested in an expression, the enclosing call
  /// borrows) — the exclusivity check for any borrow the statement makes starts
  /// from it rather than from an empty set, which is how a borrow is caught
  /// conflicting with a view created in an earlier statement.
  auto walk_stmt(const ast::node &node, const live_set &seed) -> void {
    if (node.has_error) {
      return;
    }
    switch (node.kind) {
    case ast::node_kind::let_stmt: {
      const auto &stmt = dynamic_cast<const ast::let_stmt &>(node);
      if (stmt.initializer != nullptr) {
        walk_expr(*stmt.initializer, /*borrow_ok=*/false, seed);
      }
      walk_body(stmt.else_body, seed);
      return;
    }

    case ast::node_kind::var_stmt: {
      const auto &stmt = dynamic_cast<const ast::var_stmt &>(node);
      if (stmt.initializer != nullptr) {
        walk_expr(*stmt.initializer, /*borrow_ok=*/false, seed);
      }
      return;
    }

    case ast::node_kind::assign_stmt: {
      const auto &stmt = dynamic_cast<const ast::assign_stmt &>(node);
      if (stmt.value != nullptr) {
        walk_expr(*stmt.value, /*borrow_ok=*/false, seed);
      }
      if (stmt.target != nullptr) {
        walk_expr(*stmt.target, /*borrow_ok=*/false, seed);
      }
      return;
    }

    case ast::node_kind::expr_stmt: {
      const auto &stmt = dynamic_cast<const ast::expr_stmt &>(node);
      if (stmt.expr != nullptr) {
        walk_expr(*stmt.expr, /*borrow_ok=*/false, seed);
      }
      return;
    }

    case ast::node_kind::return_stmt: {
      const auto &stmt = dynamic_cast<const ast::return_stmt &>(node);
      if (stmt.value != nullptr) {
        walk_expr(*stmt.value, /*borrow_ok=*/false, seed);
      }
      return;
    }

    case ast::node_kind::break_stmt:
    case ast::node_kind::continue_stmt:
      return;

    case ast::node_kind::if_stmt: {
      const auto &stmt = dynamic_cast<const ast::if_stmt &>(node);
      walk_if_branches(stmt.branches, stmt.else_body, seed);
      return;
    }

    case ast::node_kind::while_stmt: {
      const auto &stmt = dynamic_cast<const ast::while_stmt &>(node);
      if (stmt.condition != nullptr) {
        walk_expr(*stmt.condition, /*borrow_ok=*/false, seed);
      }
      if (stmt.let_expr != nullptr) {
        walk_expr(*stmt.let_expr, /*borrow_ok=*/false, seed);
      }
      walk_body(stmt.body, seed);
      return;
    }

    case ast::node_kind::for_stmt: {
      const auto &stmt = dynamic_cast<const ast::for_stmt &>(node);
      if (stmt.iterable != nullptr) {
        walk_expr(*stmt.iterable, /*borrow_ok=*/false, seed);
      }
      if (stmt.guard != nullptr) {
        walk_expr(*stmt.guard, /*borrow_ok=*/false, seed);
      }
      walk_body(stmt.body, seed);
      return;
    }

    case ast::node_kind::match_stmt: {
      const auto &stmt = dynamic_cast<const ast::match_stmt &>(node);
      if (stmt.subject != nullptr) {
        walk_expr(*stmt.subject, /*borrow_ok=*/false, seed);
      }
      walk_match_arms(stmt.arms, seed);
      return;
    }

    case ast::node_kind::block_expr: {
      const auto &block = dynamic_cast<const ast::block_expr &>(node);
      walk_body(block.stmts, seed);
      return;
    }

    default:
      if (const auto *expr = dynamic_cast<const ast::expr *>(&node)) {
        walk_expr(*expr, /*borrow_ok=*/false, seed);
      }
      return;
    }
  }

  auto walk_if_branches(const std::vector<ast::if_branch> &branches,
                        const std::vector<ast::ptr<ast::node>> &else_body,
                        const live_set &seed) -> void {
    for (const auto &branch : branches) {
      if (branch.condition != nullptr) {
        walk_expr(*branch.condition, /*borrow_ok=*/false, seed);
      }
      if (branch.let_expr != nullptr) {
        walk_expr(*branch.let_expr, /*borrow_ok=*/false, seed);
      }
      walk_body(branch.body, seed);
    }
    walk_body(else_body, seed);
  }

  auto walk_match_arms(const std::vector<ast::match_arm> &arms,
                       const live_set &seed) -> void {
    for (const auto &arm : arms) {
      if (arm.guard != nullptr) {
        walk_expr(*arm.guard, /*borrow_ok=*/false, seed);
      }
      if (arm.body_expr != nullptr) {
        walk_expr(*arm.body_expr, /*borrow_ok=*/false, seed);
      }
      walk_body(arm.body_stmts, seed);
    }
  }

  auto report_escape(const ast::unary_expr &borrow) -> void {
    const auto *spelling =
        borrow.op == ast::unary_op::addr_of_mut ? "&mut" : "&";
    const auto root = borrow.operand != nullptr
                          ? root_binding_name(*borrow.operand)
                          : std::string{};
    const auto subject =
        root.empty() ? std::string("value") : std::format("`{}`", root);
    auto d = diagnostic(
        diagnostic_level::error,
        std::format("a borrow of {} cannot escape the call it was made for",
                    subject),
        file_id_);
    d.with_label(borrow.span,
                 std::format("this `{}` borrow is created here", spelling));
    d.with_note("a borrow lends a value to a function for the duration of one "
                "call; it is a way of passing a value, not a value in its own "
                "right, so it cannot be stored in a binding, returned, or "
                "placed in a struct field or collection");
    d.with_help("pass the borrow directly to the call that needs it; to hand "
                "back a window into a collection that outlives the call, "
                "return a view instead (see the Views chapter), or copy the "
                "value if the callee needs its own");
    diag_.emit(d);
  }

  auto report_exclusivity(const call_borrow &earlier, const call_borrow &later)
      -> void {
    if (earlier.is_view || later.is_view) {
      report_view_conflict(earlier, later);
      return;
    }
    const auto both_mut = earlier.is_mut && later.is_mut;
    auto message =
        both_mut
            ? std::format("cannot borrow `{}` as mutable more than once in the "
                          "same call",
                          later.root)
            : std::format("cannot borrow `{}` as mutable and immutable at the "
                          "same time",
                          later.root);
    auto d = diagnostic(diagnostic_level::error, std::move(message), file_id_);
    d.with_label(
        later.span,
        later.is_mut
            ? std::format("`{}` borrowed as mutable here", later.root)
            : std::format("`{}` borrowed as immutable here", later.root));
    d.with_secondary_label(
        earlier.span,
        earlier.is_mut
            ? std::format("`{}` already borrowed as mutable here", earlier.root)
            : std::format("`{}` already borrowed as immutable here",
                          earlier.root));
    d.with_note("at most one `&mut` borrow of a value may exist at a time, and "
                "a `&mut` borrow cannot coexist with any `&` borrow; any "
                "number of `&` borrows are fine");
    d.with_note("borrows are compared at whole-variable granularity, so two "
                "borrows of different fields of the same variable also "
                "conflict here");
    d.with_help(std::format(
        "pass a single borrow of `{0}` to this call, or copy `{0}` so each "
        "call receives its own value",
        later.root));
    diag_.emit(d);
  }

  /// Reports a borrow that conflicts with a *view* still holding the same
  /// collection borrowed across statements. At least one of `earlier`/`later`
  /// is a view; `later` is the borrow that triggered the conflict.
  auto report_view_conflict(const call_borrow &earlier,
                            const call_borrow &later) -> void {
    const auto holder = [](const call_borrow &borrow) -> std::string_view {
      return borrow.is_capture ? "closure" : "view";
    };
    const auto describe = [&](const call_borrow &borrow) -> std::string {
      if (borrow.is_view && !borrow.via.empty()) {
        // A closure names only itself here; the secondary label below
        // already says which variable it borrows, so repeating it inline
        // would make the sentence read twice as long and no clearer.
        return borrow.is_capture ? std::format("the closure `{}`", borrow.via)
                                 : std::format("the view `{}` of `{}`",
                                               borrow.via, borrow.root);
      }
      return std::format("a borrow of `{}`", borrow.root);
    };
    const auto involves_capture = earlier.is_capture || later.is_capture;
    auto message = std::format("cannot borrow `{}` while {} is still in use",
                               later.root, describe(earlier));
    auto d = diagnostic(diagnostic_level::error, std::move(message), file_id_);
    d.with_label(
        later.span,
        later.is_view
            ? std::format("this {} borrows `{}`", holder(later), later.root)
        : later.is_mut
            ? std::format("`{}` borrowed as mutable here", later.root)
            : std::format("`{}` borrowed as immutable here", later.root));
    d.with_secondary_label(
        earlier.span,
        earlier.is_view && !earlier.via.empty()
            ? std::format("the {} `{}` borrows `{}` here, and is still used "
                          "later",
                          holder(earlier), earlier.via, earlier.root)
            : std::format("`{}` already borrowed here", earlier.root));
    if (involves_capture) {
      d.with_note("a `&`/`&mut` entry in a capture list borrows that variable "
                  "for as long as the closure is live; while it is, the "
                  "variable allows at most one `&mut` and no `&mut` alongside "
                  "any `&`, exactly like a direct borrow");
    } else {
      d.with_note("a view (`slice`/`mut slice`) keeps its source collection "
                  "borrowed for as long as the view is live; while it is, the "
                  "collection allows at most one `&mut` and no `&mut` "
                  "alongside any `&`, exactly like a direct borrow");
    }
    d.with_note("a value that stores or returns a view — including one built "
                "by a call — keeps every collection it was traced back to "
                "borrowed for as long as that value lives");
    d.with_help(
        involves_capture
            ? std::format(
                  "finish using the closure before borrowing `{0}` again, or "
                  "capture `{0}` by value so the closure no longer aliases it",
                  later.root)
            : std::format("finish using the view before borrowing `{0}` "
                          "again, or copy the data so the view no longer "
                          "aliases `{0}`",
                          later.root));
    diag_.emit(d);
  }
};

/// Recursively finds every `func_decl` reachable from `item` (including
/// through `impl`/`trait`/`extend`/nested-`module` bodies) and runs the
/// borrow checker over its body. Mirrors `walk_item` in `move_check.cpp`.
auto walk_item(const ast::node &item, const checked_types &checked,
               diagnostic_bag &diag, file_id_type file_id) -> void {
  switch (item.kind) {
  case ast::node_kind::func_decl: {
    const auto &decl = dynamic_cast<const ast::func_decl &>(item);
    if (decl.modifiers.is_intrinsic) {
      return;
    }
    borrow_checker(checked, diag, file_id).check_function(decl);
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

auto check_borrows(const std::vector<parsed_module> &inputs,
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
