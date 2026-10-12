#include "src/semantic/narrowing.h"

#include <algorithm>
#include <format>
#include <unordered_set>
#include <vector>

#include "src/semantic/module_index.h"

namespace cinder::semantic::detail {

namespace {

/// Follows ref types to the value type they lend.
auto strip_ref_layers(const type_table &types, type_id id) -> type_id {
  while (types.entry(id).kind == type_kind::ref_kind) {
    id = types.entry(id).result;
  }
  return id;
}

} // namespace

/// The polynomial standing for a resolved value slot (`3`, `n + 1`, an
/// unsubstituted `n`). This is how a refinement's arguments reach its
/// predicate: `index[n + 1]`'s `self < n` must become `self < n + 1`.
auto slot_poly(const type_table &types, type_id slot)
    -> std::optional<linear_poly> {
  const auto &entry = types.entry(slot);
  switch (entry.kind) {
  case type_kind::const_value_kind:
  case type_kind::symbolic_value_kind:
    return entry.value;
  default:
    return std::nullopt;
  }
}

auto refines(const type_table &types, type_id found, type_id expected)
    -> bool {
  for (auto current = found;
       types.entry(current).kind == type_kind::refinement_kind;
       current = types.entry(current).result) {
    if (current == expected) {
      return true;
    }
  }
  return false;
}

auto atom_mentions(std::string_view atom, std::string_view name) -> bool {
  for (size_t at = atom.find(name); at != std::string_view::npos;
       at = atom.find(name, at + 1)) {
    const auto before = at == 0 ? ' ' : atom[at - 1];
    const auto after =
        at + name.size() >= atom.size() ? ' ' : atom[at + name.size()];
    const auto is_name_char = [](char c) -> bool {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
             (c >= '0' && c <= '9') || c == '_';
    };
    if (!is_name_char(before) && !is_name_char(after)) {
      return true;
    }
  }
  return false;
}

/// The substitution under which a refinement's predicate speaks about
/// `value` (a concrete expression, rendered as `spelling`): `self` becomes
/// the value, and each of the declaration's value parameters becomes the
/// argument supplied for it. `nullopt` when an argument falls outside the
/// fragment, which makes the whole predicate unusable rather than
/// half-substituted.
auto narrowing_engine::refinement_subst(const type_entry &refinement,
                                        const linear_poly &value,
                                        std::string spelling,
                                        const value_bindings *solved) const
    -> std::optional<predicate_subst> {
  auto subst = predicate_subst{};
  subst.values.emplace("self", value);
  subst.spellings.emplace("self", std::move(spelling));

  if (refinement.decl == nullptr) {
    return subst;
  }
  for (size_t i = 0; i < refinement.decl->type_params.size(); ++i) {
    const auto &param = refinement.decl->type_params[i];
    if (!param.is_value_param || i >= refinement.args.size()) {
      continue;
    }
    auto poly = slot_poly(*types_, refinement.args[i]);
    if (!poly.has_value()) {
      return std::nullopt;
    }
    // At a call site, the callee's own value parameters have been pinned by
    // the other arguments: `index[n]`'s `n` is the `4` that `v: array[T, 4]`
    // just supplied. Substituting here is what turns an obligation stated in
    // the callee's terms into one the caller can actually be held to.
    if (solved != nullptr) {
      poly = poly_substitute(*poly, *solved);
    }
    subst.spellings.emplace(param.name, poly->display());
    subst.values.emplace(param.name, *poly);
  }
  return subst;
}

/// Builds the goal that narrowing `value` to `refined` owes: the
/// refinement's predicate, re-stated about this value. Walks a refinement
/// of a refinement down to the base, conjoining every predicate on the way.
auto narrowing_engine::goal(type_id refined, const ast::expr &value,
                            const value_bindings *solved) const -> goal_form {
  const auto subst_value = translator_.solver_poly(value, predicate_subst{});
  if (!subst_value.has_value()) {
    return {};
  }
  const auto spelling = translator_.describe(value, predicate_subst{});

  auto result = goal_form{};
  for (auto current = refined;
       types_->entry(current).kind == type_kind::refinement_kind;
       current = types_->entry(current).result) {
    const auto &entry = types_->entry(current);
    if (entry.predicate == nullptr) {
      continue;
    }
    const auto subst = refinement_subst(entry, *subst_value, spelling, solved);
    if (!subst.has_value()) {
      return {};
    }
    const auto part = translator_.goal_from(*entry.predicate, *subst, false);
    if (part.empty()) {
      return {};
    }
    if (result.empty()) {
      result = part;
      continue;
    }
    auto combined = goal_form{};
    for (const auto &left : result) {
      for (const auto &right : part) {
        auto merged = left;
        merged.insert(merged.end(), right.begin(), right.end());
        combined.push_back(std::move(merged));
      }
    }
    result = std::move(combined);
  }
  return result;
}

/// The facts currently in scope, rendered for a diagnostic. Only the ones
/// that actually mention something the goal mentions are worth showing —
/// a reader who wants to know why `i < n` didn't follow is not helped by
/// being told what is known about `k`.
auto narrowing_engine::known_facts_note(const goal_form &goal) const
    -> std::optional<std::string> {
  auto mentioned = std::unordered_set<std::string>{};
  for (const auto &alternative : goal) {
    for (const auto &item : alternative) {
      for (const auto &term : item.poly.terms) {
        mentioned.insert(term.var);
      }
    }
  }
  if (mentioned.empty()) {
    // The goal is arithmetic on constants (`9 < 4`). There is nothing to
    // know about it, and saying "nothing is known" would read as a failure
    // of the compiler rather than of the code.
    return std::nullopt;
  }
  auto shown = std::vector<std::string>{};
  for (const auto &fact : *facts_) {
    if (fact.label.empty()) {
      continue;
    }
    const auto relevant = std::ranges::any_of(
        fact.poly.terms, [&](const poly_term &term) -> bool {
          return mentioned.contains(term.var);
        });
    if (relevant) {
      shown.push_back(std::format("`{}`", fact.label));
    }
  }
  if (shown.empty()) {
    return "nothing is known about the values in this condition here";
  }
  return std::format("known here: {}", join_strings(shown, ", "));
}

/// The one goal that every alternative shares, for a diagnostic's headline
/// — a goal is almost always a single constraint, and when it isn't, its
/// first alternative is still the honest thing to lead with.
///
/// `fallback` is what to say when the goal is *empty*, which is not an
/// internal failure but a real and expected outcome: the predicate fell
/// outside the reasoning fragment (a float bound, `n * n`), so there is no
/// constraint to name and the compiler must quote the source instead.
auto narrowing_engine::goal_label(const goal_form &goal,
                                  std::string_view fallback) -> std::string {
  for (const auto &alternative : goal) {
    for (const auto &item : alternative) {
      if (!item.label.empty()) {
        return item.label;
      }
    }
  }
  return std::string(fallback);
}

/// The label of the constraint the facts refute on its own, for a refuted
/// goal. Refutation of a conjunction comes from one conjunct, and quoting
/// the first would name a constraint that may well be satisfied
/// (`250 >= 0` when it is `250 <= 100` that fails). Falls back to
/// `goal_label` when only the conjunction as a whole is refuted.
auto narrowing_engine::refuted_label(const goal_form &goal,
                                     std::string_view fallback) const
    -> std::string {
  for (const auto &alternative : goal) {
    for (const auto &item : alternative) {
      if (!item.label.empty() &&
          solve(*facts_, goal_form{fact_set{item}}) ==
              proof_result::refuted) {
        return item.label;
      }
    }
  }
  return goal_label(goal, fallback);
}

/// The source text of a refinement's predicate, restated about `value` —
/// the honest thing to quote when the predicate is real but undecidable.
auto narrowing_engine::predicate_text(type_id refined,
                                      const ast::expr &value) const
    -> std::string {
  const auto &entry = types_->entry(refined);
  if (entry.predicate == nullptr) {
    return std::format("the predicate of `{}`", types_->display(refined));
  }
  auto subst = predicate_subst{};
  subst.spellings.emplace("self",
                          translator_.describe(value, predicate_subst{}));
  return translator_.describe(*entry.predicate, subst);
}

/// Spells a refinement the way it reads *at this site*, with the callee's
/// value parameters replaced by what the call site pinned them to — so the
/// diagnostic says "expected `index[4]`" rather than "expected `index[n]`"
/// and quotes an `n` the reader cannot see from where they are standing.
auto narrowing_engine::display_refined(type_id refined,
                                       const value_bindings *solved) const
    -> std::string {
  const auto &entry = types_->entry(refined);
  if (solved == nullptr || entry.decl == nullptr || entry.args.empty()) {
    return types_->display(refined);
  }
  auto arguments = std::vector<std::string>{};
  for (const auto arg : entry.args) {
    const auto poly = slot_poly(*types_, arg);
    arguments.push_back(poly.has_value()
                            ? poly_substitute(*poly, *solved).display()
                            : types_->display(arg));
  }
  return std::format("{}[{}]", entry.decl->name, join_strings(arguments, ", "));
}

/// Discharges the obligation a *narrowing* incurs: using a value where a
/// more refined type is required. Widening is free and never lands here
/// (`compatible` accepts both directions structurally — see `types.h`);
/// this is the sole place the predicate is actually made to hold.
auto narrowing_engine::check(type_id expected, type_id found,
                             const ast::expr *value,
                             const value_bindings *solved)
    -> std::optional<narrowing_failure> {
  if (value == nullptr ||
      types_->entry(expected).kind != type_kind::refinement_kind) {
    return std::nullopt;
  }
  // Already at least as refined as required: no debt. `found` being the
  // same refinement (or a refinement *of* it) carries the predicate along
  // with it, which is the whole point of the type surviving assignment.
  if (refines(*types_, found, expected)) {
    return std::nullopt;
  }
  // A value whose type isn't even the right shape has a type error already
  // reported against it; adding an unprovable-predicate error on top would
  // be noise about a value that was never going to work.
  if (types_->is_unknown(types_->strip_refinement(found))) {
    return std::nullopt;
  }

  const auto needed = goal(expected, *value, solved);
  const auto outcome =
      needed.empty() ? proof_result::unknown : solve(*facts_, needed);
  if (outcome == proof_result::proved) {
    return std::nullopt;
  }

  const auto fallback_text = predicate_text(expected, *value);
  return narrowing_failure{
      .outcome = outcome,
      .refined_name = display_refined(expected, solved),
      .base_name = types_->display(types_->strip_refinement(expected)),
      .goal_text = outcome == proof_result::refuted
                       ? refuted_label(needed, fallback_text)
                       : goal_label(needed, fallback_text),
      .known_note = known_facts_note(needed),
  };
}

/// Everything the *type* of a binding says about its value, added to the
/// facts in scope: a refined type contributes its predicate (with `self`
/// bound to the name), and an array contributes its length
/// (`len(v) = n`) — which is what lets an index into it be proved in bounds
/// against a matching `index[n]`.
auto narrowing_engine::assume_binding(std::string_view name, type_id type)
    -> void {
  const auto value = poly_variable(std::string(name));

  const auto bare = strip_ref_layers(*types_, type);
  for (auto current = bare;
       types_->entry(current).kind == type_kind::refinement_kind;
       current = types_->entry(current).result) {
    const auto &entry = types_->entry(current);
    if (entry.predicate == nullptr) {
      continue;
    }
    const auto subst =
        refinement_subst(entry, value, std::string(name), nullptr);
    if (!subst.has_value()) {
      continue;
    }
    for (auto &fact : translator_.facts_from(*entry.predicate, *subst, false)) {
      facts_->push_back(std::move(fact));
    }
  }

  const auto &entry = types_->entry(types_->strip_refinement(bare));
  if (entry.kind == type_kind::array_kind && !entry.args.empty()) {
    if (const auto length = slot_poly(*types_, entry.args.front())) {
      auto poly =
          poly_sub(poly_variable(std::format("len({})", name)), *length);
      facts_->push_back(constraint{
          .poly = std::move(poly),
          .rel = relation::eq,
          .label = std::format("len({}) == {}", name, length->display()),
      });
    }
  }

  // A struct's `invariant` holds of *every* value of that type, everywhere —
  // that is what makes it an invariant rather than a precondition. So it
  // enters the environment for any binding of the type, which is what lets
  // a `positive_int` whose `invariant self.value > 0` satisfy a `positive`
  // with no further proof (`cinder-reference.md` §Contracts).
  if (entry.decl != nullptr && entry.decl->invariant != nullptr &&
      !entry.decl->invariant->has_error) {
    auto subst = predicate_subst{};
    subst.values.emplace("self", value);
    subst.spellings.emplace("self", std::string(name));
    for (auto &fact :
         translator_.facts_from(*entry.decl->invariant, subst, false)) {
      facts_->push_back(std::move(fact));
    }
  }
}

// ------------------------------------------------------------------------
//  Flow-sensitive narrowing (`spec/dependent-types-design.md` §5.1, item 6)
//
//  Inside `if x > 0:`, the fact `x > 0` holds — so passing `x` where a
//  `positive` is expected discharges statically, with no `try_from` and no
//  annotation. The `else` gets the negation. Facts are scoped exactly to
//  the region the condition dominates, and any assignment to a name drops
//  everything known about it.
//
//  This is ergonomics, not expressiveness: the language is already complete
//  without it, because `try_from` covers everything. It is deliberately
//  not clever — nothing narrows through a function call, a closure, or an
//  `&mut`.
// ------------------------------------------------------------------------

auto narrowing_engine::assume_condition(const ast::expr *condition,
                                        bool negated) -> void {
  // An `if let` plants an `error_expr` in `condition` on every parse (see
  // `param_usage_inferrer::walk_expr`); it says nothing about any value.
  if (condition == nullptr || condition->has_error) {
    return;
  }
  for (auto &fact :
       translator_.facts_from(*condition, predicate_subst{}, negated)) {
    facts_->push_back(std::move(fact));
  }
}

auto narrowing_engine::assume_non_negative(std::string var, std::string label)
    -> void {
  facts_->push_back(constraint{
      .poly = poly_variable(std::move(var)),
      .rel = relation::ge,
      .label = std::move(label),
  });
}

/// A `pre` is an *obligation* at every call site and a *fact* inside the
/// callee. It is weighed against everything already known — the parameters'
/// own types plus the preconditions ahead of it, which are checked first at
/// run time and so hold by the time it is evaluated. One that follows from
/// those cannot fail on any call, so its run-time check can be elided.
auto narrowing_engine::assume_precondition(const ast::expr &condition)
    -> bool {
  const auto needed = translator_.goal_from(condition, predicate_subst{}, false);
  const auto proved =
      !needed.empty() && solve(*facts_, needed) == proof_result::proved;
  for (auto &fact :
       translator_.facts_from(condition, predicate_subst{}, false)) {
    facts_->push_back(std::move(fact));
  }
  return proved;
}

/// Forgets everything known about `name`. Called when a value is assigned
/// to, or lent mutably — at which point every fact that mentioned it
/// describes a value that no longer exists.
auto narrowing_engine::invalidate(std::string_view name) -> void {
  if (name.empty()) {
    return;
  }
  std::erase_if(*facts_, [&](const constraint &fact) -> bool {
    return std::ranges::any_of(fact.poly.terms,
                               [&](const poly_term &term) -> bool {
                                 return atom_mentions(term.var, name);
                               });
  });
}

} // namespace cinder::semantic::detail
