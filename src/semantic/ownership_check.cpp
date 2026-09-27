#include "ownership_check.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "src/semantic/ownership_cfg.h"

namespace cinder::semantic {
namespace {

using ownership::access_event;
using ownership::access_kind;
using ownership::block_id;
using ownership::event;
using ownership::flow_event;
using ownership::function_cfg;
using ownership::k_no_loan;
using ownership::loan_id;
using ownership::loan_info;
using ownership::loan_origin;
using ownership::local_id;
using ownership::local_role;
using ownership::use_event;

using loan_set = std::vector<loan_id>; ///< Sorted, unique.
using live_set = std::vector<bool>;    ///< Indexed by `local_id`.

auto merge_into(loan_set &into, const loan_set &from) -> bool {
  if (from.empty()) {
    return false;
  }
  auto merged = loan_set{};
  merged.reserve(into.size() + from.size());
  std::ranges::set_union(into, from, std::back_inserter(merged));
  if (merged.size() == into.size()) {
    return false;
  }
  into = std::move(merged);
  return true;
}

/// The forward facts at a program point: which loans each holder carries,
/// and which locals may already have been moved from (and where).
struct forward_state {
  bool reached = false;
  std::vector<loan_set> contents;
  std::vector<std::optional<source_span>> moved_at;

  /// Joins `other` into this state; returns whether anything changed.
  auto join(const forward_state &other) -> bool {
    if (!other.reached) {
      return false;
    }
    if (!reached) {
      *this = other;
      return true;
    }
    auto changed = false;
    for (std::size_t i = 0; i < contents.size(); ++i) {
      changed = merge_into(contents[i], other.contents[i]) || changed;
      if (!moved_at[i].has_value() && other.moved_at[i].has_value()) {
        moved_at[i] = other.moved_at[i];
        changed = true;
      }
    }
    return changed;
  }
};

/// Live-after to live-before across one event.
auto live_step(const event &e, live_set &live) -> void {
  if (const auto *a = std::get_if<access_event>(&e)) {
    switch (a->kind) {
    case access_kind::read:
    case access_kind::move:
    case access_kind::borrow_shared:
    case access_kind::borrow_mut:
    case access_kind::write_part: // writes *through* the value it holds
      live[a->local] = true;
      break;
    case access_kind::storage_dead:
      live[a->local] = false;
      break;
    case access_kind::write_whole:
      break;
    }
  } else if (const auto *f = std::get_if<flow_event>(&e)) {
    if (f->replace) {
      live[f->dest] = false;
    }
    for (const auto source : f->sources) {
      live[source] = true;
    }
  } else if (const auto *u = std::get_if<use_event>(&e)) {
    live[u->local] = true;
  }
}

auto forward_step(const event &e, const function_cfg &cfg,
                  forward_state &state) -> void {
  if (const auto *a = std::get_if<access_event>(&e)) {
    switch (a->kind) {
    case access_kind::move:
      if (cfg.locals[a->local].movable && !state.moved_at[a->local]) {
        state.moved_at[a->local] = a->span;
      }
      break;
    case access_kind::write_whole:
      state.moved_at[a->local].reset();
      break;
    case access_kind::storage_dead:
      state.moved_at[a->local].reset();
      state.contents[a->local].clear();
      break;
    default:
      break;
    }
  } else if (const auto *f = std::get_if<flow_event>(&e)) {
    auto next = f->replace ? loan_set{} : state.contents[f->dest];
    auto direct = f->loans;
    std::ranges::sort(direct);
    merge_into(next, direct);
    for (const auto source : f->sources) {
      merge_into(next, state.contents[source]);
    }
    state.contents[f->dest] = std::move(next);
  }
}

/// Whether an access of `kind` may not happen while `loan` is live.
auto conflicts(access_kind kind, const loan_info &loan) -> bool {
  switch (kind) {
  case access_kind::read:
  case access_kind::borrow_shared:
    return loan.is_mut;
  default:
    return true;
  }
}

/// How strongly a holder names the reason a loan is still live: a binding
/// the user wrote reads best, a compiler temporary worst.
auto holder_rank(local_role role) -> int {
  switch (role) {
  case local_role::binding:
    return 0;
  case local_role::loop_source:
    return 1;
  case local_role::return_slot:
    return 2;
  default:
    return 3;
  }
}

class ownership_checker {
public:
  ownership_checker(const function_cfg &cfg, const checked_types &checked,
                    diagnostic_bag &diag, file_id_type file_id)
      : cfg_(cfg), checked_(checked), diag_(diag), file_id_(file_id) {}

  auto run() -> void {
    for (const auto &escape : cfg_.escapes) {
      report_escape(escape);
    }
    compute_liveness();
    compute_forward();
    report();
  }

private:
  const function_cfg &cfg_;
  const checked_types &checked_;
  diagnostic_bag &diag_;
  file_id_type file_id_;
  std::vector<live_set> live_in_;
  std::vector<forward_state> in_;
  std::set<local_id> reported_moves_;
  std::set<std::pair<local_id, loan_id>> reported_conflicts_;

  [[nodiscard]] auto local_count() const -> std::size_t {
    return cfg_.locals.size();
  }

  [[nodiscard]] auto live_out(block_id b) const -> live_set {
    auto live = live_set(local_count(), false);
    for (const auto s : cfg_.blocks[b].successors) {
      for (std::size_t i = 0; i < live.size(); ++i) {
        live[i] = live[i] || live_in_[s][i];
      }
    }
    return live;
  }

  auto compute_liveness() -> void {
    live_in_.assign(cfg_.blocks.size(), live_set(local_count(), false));
    auto changed = true;
    while (changed) {
      changed = false;
      for (auto b = cfg_.blocks.size(); b > 0; --b) {
        const auto id = static_cast<block_id>(b - 1);
        auto live = live_out(id);
        const auto &events = cfg_.blocks[id].events;
        for (auto e = events.rbegin(); e != events.rend(); ++e) {
          live_step(*e, live);
        }
        if (live != live_in_[id]) {
          live_in_[id] = std::move(live);
          changed = true;
        }
      }
    }
  }

  auto compute_forward() -> void {
    in_.assign(cfg_.blocks.size(), forward_state{});
    for (auto &state : in_) {
      state.contents.assign(local_count(), {});
      state.moved_at.assign(local_count(), std::nullopt);
    }
    in_[0].reached = true;
    auto changed = true;
    while (changed) {
      changed = false;
      for (std::size_t b = 0; b < cfg_.blocks.size(); ++b) {
        if (!in_[b].reached) {
          continue;
        }
        auto state = in_[b];
        for (const auto &e : cfg_.blocks[b].events) {
          forward_step(e, cfg_, state);
        }
        for (const auto s : cfg_.blocks[b].successors) {
          changed = in_[s].join(state) || changed;
        }
      }
    }
  }

  auto report() -> void {
    for (std::size_t b = 0; b < cfg_.blocks.size(); ++b) {
      if (!in_[b].reached) {
        continue;
      }
      const auto &events = cfg_.blocks[b].events;
      // Liveness after each event, walking back from the block's end.
      auto live_after = std::vector<live_set>(events.size());
      auto live = live_out(static_cast<block_id>(b));
      for (auto i = events.size(); i > 0; --i) {
        live_after[i - 1] = live;
        live_step(events[i - 1], live);
      }
      auto state = in_[b];
      for (std::size_t i = 0; i < events.size(); ++i) {
        if (const auto *a = std::get_if<access_event>(&events[i])) {
          check_move(*a, state);
          check_conflict(*a, state, live_after[i]);
        }
        forward_step(events[i], cfg_, state);
      }
    }
  }

  auto check_move(const access_event &a, const forward_state &state)
      -> void {
    if (a.kind == access_kind::write_whole ||
        a.kind == access_kind::storage_dead) {
      return;
    }
    const auto &moved = state.moved_at[a.local];
    if (!moved.has_value() || !cfg_.locals[a.local].movable ||
        !reported_moves_.insert(a.local).second) {
      return;
    }
    report_use_after_move(cfg_.locals[a.local].name, a.span, *moved);
  }

  auto check_conflict(const access_event &a, const forward_state &state,
                      const live_set &live) -> void {
    auto best_loan = k_no_loan;
    auto best_holder = local_id{0};
    auto best_rank = 4;
    for (std::size_t h = 0; h < live.size(); ++h) {
      if (!live[h]) {
        continue;
      }
      for (const auto l : state.contents[h]) {
        const auto &loan = cfg_.loans[l];
        if (loan.root != a.local || l == a.exempt ||
            !conflicts(a.kind, loan)) {
          continue;
        }
        const auto rank = holder_rank(cfg_.locals[h].role);
        if (rank < best_rank || (rank == best_rank && l < best_loan)) {
          best_loan = l;
          best_holder = static_cast<local_id>(h);
          best_rank = rank;
        }
      }
    }
    if (best_loan == k_no_loan ||
        !reported_conflicts_.emplace(a.local, best_loan).second) {
      return;
    }
    report_conflict(a, cfg_.loans[best_loan], best_holder);
  }

  // ------------------------------------------------------------------
  //  Diagnostics.
  // ------------------------------------------------------------------

  [[nodiscard]] auto type_kind_of(type_id type) const
      -> std::optional<type_kind> {
    if (checked_.types.is_unknown(type)) {
      return std::nullopt;
    }
    return checked_.types.entry(type).kind;
  }

  /// "the view `s` of `xs`", "the closure `a`", "the `for` loop over `xs`".
  [[nodiscard]] auto describe_holder(local_id holder,
                                     const std::string &root) const
      -> std::string {
    const auto &info = cfg_.locals[holder];
    if (info.role == local_role::loop_source) {
      return std::format("the `for` loop over `{}`", root);
    }
    const auto kind = type_kind_of(info.type);
    if (kind == type_kind::fn_kind) {
      return std::format("the closure `{}`", info.name);
    }
    if (kind == type_kind::ref_kind &&
        !checked_.borrow_bearing_types.contains(
            checked_.types.entry(info.type).result)) {
      return std::format("the reference `{}` to `{}`", info.name, root);
    }
    return std::format("the view `{}` of `{}`", info.name, root);
  }

  [[nodiscard]] static auto is_in_flight(local_role role) -> bool {
    return role == local_role::call_temp || role == local_role::join_temp ||
           role == local_role::subject_temp;
  }

  auto report_conflict(const access_event &a, const loan_info &loan,
                       local_id holder) -> void {
    const auto &root = cfg_.locals[a.local].name;
    const auto role = cfg_.locals[holder].role;
    if (a.kind == access_kind::storage_dead) {
      if (role == local_role::return_slot) {
        report_returned_borrow(a, loan, root);
      } else {
        report_outlived(a, loan, holder, root);
      }
      return;
    }
    if (is_in_flight(role)) {
      if (a.kind == access_kind::borrow_shared ||
          a.kind == access_kind::borrow_mut) {
        report_same_call(a, loan, root);
      } else {
        report_access_while_lent(a, loan, root);
      }
      return;
    }
    report_access_while_held(a, loan, holder, root);
  }

  auto report_use_after_move(const std::string &name, source_span span,
                             source_span moved_at) -> void {
    auto d = diagnostic(diagnostic_level::error,
                        std::format("use of moved value `{}`", name), file_id_);
    d.with_label(span, std::format("`{}` used here after being moved", name));
    d.with_secondary_label(moved_at, std::format("`{}` moved here", name));
    d.with_note(
        "a value's owner may use it once more before it goes out of scope; "
        "moving it transfers that ownership away, and Cinder does not "
        "implicitly copy non-scalar values");
    d.with_note("a move inside a loop, or in only one branch of an `if` or "
                "`match`, still counts: the use is rejected if any path to "
                "it has already moved the value");
    d.with_help(std::format(
        "borrow it instead with `&{0}` (or `&mut {0}`) if the callee only "
        "needs to read or modify it, or restructure the code so `{0}` is "
        "only used once",
        name));
    diag_.emit(d);
  }

  auto report_escape(const ownership::escape_site &escape) -> void {
    const auto &borrow = *escape.borrow;
    const auto *spelling =
        borrow.op == ast::unary_op::addr_of_mut ? "&mut" : "&";
    const auto subject = escape.root.empty()
                             ? std::string("value")
                             : std::format("`{}`", escape.root);
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

  /// Two borrows made for calls still being evaluated.
  auto report_same_call(const access_event &a, const loan_info &loan,
                        const std::string &root) -> void {
    const auto earlier_mut = loan.is_mut || loan.reserved_mut;
    const auto later_mut = a.kind == access_kind::borrow_mut;
    auto message =
        earlier_mut && later_mut
            ? std::format("cannot borrow `{}` as mutable more than once in the "
                          "same call",
                          root)
            : std::format("cannot borrow `{}` as mutable and immutable at the "
                          "same time",
                          root);
    auto d = diagnostic(diagnostic_level::error, std::move(message), file_id_);
    d.with_label(a.span, std::format("`{}` borrowed as {} here", root,
                                     later_mut ? "mutable" : "immutable"));
    d.with_secondary_label(
        loan.span, std::format("`{}` already borrowed as {} here", root,
                               earlier_mut ? "mutable" : "immutable"));
    d.with_note("a borrow made for a call lasts until that call returns — "
                "including while its other arguments, and any calls nested "
                "in them, are evaluated");
    d.with_note("at most one `&mut` borrow of a value may exist at a time, and "
                "a `&mut` borrow cannot coexist with any `&` borrow; any "
                "number of `&` borrows are fine");
    d.with_note("borrows are compared at whole-variable granularity, so two "
                "borrows of different fields of the same variable also "
                "conflict here");
    d.with_help(std::format(
        "pass a single borrow of `{0}` to this call, or compute the other "
        "argument into a local first so the two borrows no longer overlap",
        root));
    diag_.emit(d);
  }

  /// A read, write, or move of a value a call in progress has borrowed.
  auto report_access_while_lent(const access_event &a, const loan_info &loan,
                                const std::string &root) -> void {
    const auto *verb = a.kind == access_kind::move   ? "move"
                       : a.kind == access_kind::read ? "use"
                                                     : "assign to";
    auto d = diagnostic(
        diagnostic_level::error,
        std::format("cannot {} `{}` while it is {}borrowed", verb, root,
                    loan.is_mut ? "mutably " : ""),
        file_id_);
    d.with_label(a.span, std::format("`{}` {} here", root,
                                     a.kind == access_kind::move   ? "moved"
                                     : a.kind == access_kind::read ? "used"
                                                                   : "assigned"));
    d.with_secondary_label(
        loan.span,
        std::format("`{}` is borrowed here, until this call returns", root));
    d.with_note("a borrow made for a call lasts until that call returns, so "
                "the value cannot be moved, replaced, or — while the borrow "
                "is `&mut` — even read in the call's other arguments");
    d.with_help(std::format(
        "compute this argument into a local before the call, so it no "
        "longer overlaps the borrow of `{}`",
        root));
    diag_.emit(d);
  }

  /// A read, write, move, or borrow of a value a live view, closure,
  /// reference, or `for` loop still borrows.
  auto report_access_while_held(const access_event &a, const loan_info &loan,
                                local_id holder, const std::string &root)
      -> void {
    const auto &info = cfg_.locals[holder];
    const auto description = describe_holder(holder, root);
    const auto is_loop = info.role == local_role::loop_source;
    const auto is_capture = loan.origin == loan_origin::capture;
    const auto *verb = a.kind == access_kind::borrow_shared ||
                               a.kind == access_kind::borrow_mut
                           ? "borrow"
                       : a.kind == access_kind::move ? "move"
                       : a.kind == access_kind::read ? "use"
                                                     : "assign to";
    const auto *still = is_loop ? "still running" : "still in use";
    auto d = diagnostic(diagnostic_level::error,
                        std::format("cannot {} `{}` while {} is {}", verb,
                                    root, description, still),
                        file_id_);
    switch (a.kind) {
    case access_kind::borrow_shared:
    case access_kind::borrow_mut:
      d.with_label(a.span,
                   std::format("`{}` borrowed as {} here", root,
                               a.kind == access_kind::borrow_mut
                                   ? "mutable"
                                   : "immutable"));
      break;
    case access_kind::move:
      d.with_label(a.span, std::format("`{}` moved here", root));
      break;
    case access_kind::read:
      d.with_label(a.span, std::format("`{}` read here", root));
      break;
    default:
      d.with_label(a.span, std::format("`{}` assigned here", root));
      break;
    }
    if (is_loop) {
      d.with_secondary_label(
          loan.span,
          std::format("the loop borrows `{}` here, for the whole body", root));
      d.with_note(std::format(
          "a `for` loop over `&{0}`, `&mut {0}`, or a view or iterator of "
          "`{0}` keeps `{0}` borrowed until the loop ends; while it does, "
          "`{0}` allows at most one `&mut` and no `&mut` alongside any `&`, "
          "and cannot be moved or replaced",
          root));
      d.with_help(std::format("collect what the loop needs to change and "
                              "apply it to `{}` after the loop ends",
                              root));
      diag_.emit(d);
      return;
    }
    d.with_secondary_label(
        loan.span,
        std::format("{} borrows `{}` here, and is still used later",
                    is_capture ? std::format("the closure `{}`", info.name)
                               : std::format("`{}`", info.name),
                    root));
    if (is_capture) {
      d.with_note("a `&`/`&mut` entry in a capture list borrows that variable "
                  "for as long as the closure is live; while it is, the "
                  "variable allows at most one `&mut` and no `&mut` alongside "
                  "any `&`, exactly like a direct borrow");
      d.with_help(std::format(
          "finish using the closure before touching `{0}` again, or "
          "capture `{0}` by value so the closure no longer aliases it",
          root));
    } else {
      d.with_note("a view (`slice`/`mut slice`, `cell`/`mut cell`) keeps its "
                  "source borrowed for as long as the view is live; while it "
                  "is, the source allows at most one `&mut` and no `&mut` "
                  "alongside any `&`, and cannot be moved or replaced — and "
                  "while the view is `mut`, not even read directly");
      d.with_note("a value that stores or returns a view — including one built "
                  "by a call — keeps every collection it was traced back to "
                  "borrowed for as long as that value lives");
      d.with_help(std::format(
          "finish using `{0}` before touching `{1}` again, or copy the data "
          "so `{0}` no longer aliases `{1}`",
          info.name, root));
    }
    diag_.emit(d);
  }

  /// A local going out of scope while something that borrows it lives on.
  auto report_outlived(const access_event &a, const loan_info &loan,
                       local_id holder, const std::string &root) -> void {
    const auto &info = cfg_.locals[holder];
    auto d =
        diagnostic(diagnostic_level::error,
                   std::format("`{}` does not live long enough", root),
                   file_id_);
    const auto named = info.role == local_role::binding ||
                       info.role == local_role::loop_source;
    d.with_label(loan.span,
                 named ? std::format("{} borrows `{}` here",
                                     describe_holder(holder, root), root)
                       : std::format("`{}` is borrowed here", root));
    d.with_secondary_label(
        a.span, std::format("`{}` is declared here, and dropped at the end "
                            "of its block",
                            root));
    d.with_note(named ? std::format("`{}` is still used after that block "
                                    "ends, when `{}` no longer exists",
                                    info.name, root)
                      : std::format("the block's value still borrows `{}` "
                                    "after the block ends, when `{}` no "
                                    "longer exists",
                                    root, root));
    d.with_help(std::format("declare `{}` in a scope that lasts as long as "
                            "the borrow, or copy the data out of it",
                            root));
    diag_.emit(d);
  }

  /// A returned value that borrows one of the function's own locals.
  auto report_returned_borrow(const access_event &a, const loan_info &loan,
                              const std::string &root) -> void {
    const auto is_capture = loan.origin == loan_origin::capture;
    auto d = diagnostic(
        diagnostic_level::error,
        is_capture
            ? std::format("cannot return a closure that borrows `{}`", root)
            : std::format("cannot return a borrow of the local `{}`", root),
        file_id_);
    d.with_label(loan.span,
                 std::format("`{}` is borrowed here, and the borrow is "
                             "part of the returned value",
                             root));
    d.with_secondary_label(
        a.span, std::format("`{}` belongs to this function, and is dropped "
                            "when it returns",
                            root));
    d.with_note("a returned value outlives every local of the function that "
                "returns it, so it cannot borrow one of them");
    d.with_help(
        is_capture
            ? std::format("capture `{0}` by value — list it without `&` in the "
                          "capture list, or mark the lambda `move` — so the "
                          "closure owns what it uses",
                          root)
            : std::format("return an owned value instead (copy the elements "
                          "into a new collection), or take `{}` as a `&` "
                          "parameter so the view borrows the caller's data",
                          root));
    diag_.emit(d);
  }
};

auto check_decl(const ast::func_decl &decl, const checked_types &checked,
                diagnostic_bag &diag, file_id_type file_id) -> void {
  if (decl.modifiers.is_intrinsic) {
    return;
  }
  for (const auto &cfg : ownership::build_function_cfgs(decl, checked)) {
    ownership_checker(cfg, checked, diag, file_id).run();
  }
}

auto walk_item(const ast::node &item, const checked_types &checked,
               diagnostic_bag &diag, file_id_type file_id) -> void {
  const auto walk_items = [&](const auto &items) -> void {
    for (const auto &child : items) {
      if (child != nullptr) {
        walk_item(*child, checked, diag, file_id);
      }
    }
  };
  switch (item.kind) {
  case ast::node_kind::func_decl:
    check_decl(dynamic_cast<const ast::func_decl &>(item), checked, diag,
               file_id);
    return;
  case ast::node_kind::impl_decl:
    walk_items(dynamic_cast<const ast::impl_decl &>(item).items);
    return;
  case ast::node_kind::trait_decl:
    walk_items(dynamic_cast<const ast::trait_decl &>(item).items);
    return;
  case ast::node_kind::extend_decl:
    walk_items(dynamic_cast<const ast::extend_decl &>(item).items);
    return;
  case ast::node_kind::sub_module_decl:
    walk_items(dynamic_cast<const ast::sub_module_decl &>(item).items);
    return;
  default:
    return;
  }
}

} // namespace

auto check_ownership(const std::vector<parsed_module> &inputs,
                     const checked_types &checked, diagnostic_bag &diag,
                     std::vector<bool> &file_has_errors,
                     unsigned skip_from_fileid) -> void {
  for (const auto &input : inputs) {
    if (static_cast<unsigned>(input.file_id) >= skip_from_fileid ||
        input.ast_file == nullptr) {
      continue;
    }
    if (static_cast<std::size_t>(input.file_id) < file_has_errors.size() &&
        file_has_errors[input.file_id]) {
      continue;
    }
    const auto errors_before = diag.error_count();
    for (const auto &item : input.ast_file->items) {
      if (item != nullptr) {
        walk_item(*item, checked, diag, input.file_id);
      }
    }
    if (diag.error_count() > errors_before &&
        static_cast<std::size_t>(input.file_id) < file_has_errors.size()) {
      file_has_errors[input.file_id] = true;
    }
  }
}

} // namespace cinder::semantic
