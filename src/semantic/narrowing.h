#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "src/parser/ast.h"
#include "src/semantic/linear_poly.h"
#include "src/semantic/predicates.h"
#include "src/semantic/reason.h"
#include "src/semantic/type_table.h"

namespace cinder::semantic::detail {

// ==========================================================================
//  Narrowing — what is known, and what a refined type owes
//
//  Holds the proof side of dependent types: the facts that hold at a point
//  in a function, how a binding, a branch condition, or a precondition adds
//  to them, and what discharging a refinement at a use site requires.
//
//  The engine reads the type table and a `predicate_translator` and works
//  on a `fact_set` it is handed; it holds no checker state and emits no
//  diagnostics. When a proof fails it returns a `narrowing_failure` with
//  the pieces a diagnostic needs, and the checker words and reports it.
// ==========================================================================

/// Restores a fact set to its length on entry when it goes out of scope, so
/// a branch's path conditions and a block's local refinements are visible
/// exactly within the region that dominates them and nowhere else.
class fact_scope {
public:
  explicit fact_scope(fact_set &facts) : facts_(&facts), depth_(facts.size()) {}
  fact_scope(const fact_scope &) = delete;
  fact_scope(fact_scope &&) = delete;
  auto operator=(const fact_scope &) -> fact_scope & = delete;
  auto operator=(fact_scope &&) -> fact_scope & = delete;
  ~fact_scope() { facts_->resize(depth_); }

private:
  fact_set *facts_;
  size_t depth_;
};

/// The polynomial standing for a resolved value slot (`3`, `n + 1`, an
/// unsubstituted `n`), or `nullopt` when the slot is not a value.
auto slot_poly(const type_table &types, type_id slot)
    -> std::optional<linear_poly>;

/// Whether `found` already carries at least the predicate `expected`
/// demands — the same refinement, or one layered on top of it.
auto refines(const type_table &types, type_id found, type_id expected) -> bool;

/// Whether a solver atom is *about* `name` — the name itself (`x`), a
/// projection of it (`x.len`), or a call over it (`len(x)`). Errs toward
/// true: a fact wrongly kept is unsound, one wrongly dropped costs a proof.
auto atom_mentions(std::string_view atom, std::string_view name) -> bool;

/// Why a narrowing could not be proved, with the text a diagnostic quotes.
struct narrowing_failure {
  /// `refuted` when the facts rule the predicate out, else `unknown`.
  proof_result outcome = proof_result::unknown;
  /// The refinement as it reads at this site.
  std::string refined_name;
  /// The refinement's base type.
  std::string base_name;
  /// The constraint the diagnostic names.
  std::string goal_text;
  /// The facts in scope that bear on the goal, when there are any to show.
  std::optional<std::string> known_note;
};

class narrowing_engine {
public:
  narrowing_engine(const type_table &types, predicate_translator translator,
                   fact_set &facts)
      : types_(&types), translator_(std::move(translator)), facts_(&facts) {}

  /// Weighs using a value of type `found` where `expected` is required.
  /// `nullopt` means nothing is owed or the debt is proved; otherwise the
  /// failure to report. `solved` pins a callee's value parameters at a call.
  auto check(type_id expected, type_id found, const ast::expr *value,
             const value_bindings *solved) -> std::optional<narrowing_failure>;

  /// Adds everything the type of binding `name` says about its value:
  /// refinement predicates, an array's length, a struct's invariant.
  auto assume_binding(std::string_view name, type_id type) -> void;

  /// Assumes a condition (or its negation) for the region it dominates.
  auto assume_condition(const ast::expr *condition, bool negated) -> void;

  /// Assumes `var >= 0`, labelled `label`.
  auto assume_non_negative(std::string var, std::string label) -> void;

  /// Assumes a precondition. Returns whether it was already provable from
  /// what is known, in which case no run-time check is needed.
  auto assume_precondition(const ast::expr &condition) -> bool;

  /// Forgets everything known about `name`.
  auto invalidate(std::string_view name) -> void;

  /// The goal that narrowing `value` to `refined` owes; empty when the
  /// predicate falls outside the fragment.
  auto goal(type_id refined, const ast::expr &value,
            const value_bindings *solved) const -> goal_form;

  /// Facts in scope that mention something `goal` does, for a note.
  auto known_facts_note(const goal_form &goal) const
      -> std::optional<std::string>;

  /// The label of the goal's first constraint, or `fallback`.
  static auto goal_label(const goal_form &goal, std::string_view fallback)
      -> std::string;

private:
  auto refinement_subst(const type_entry &refinement, const linear_poly &value,
                        std::string spelling,
                        const value_bindings *solved) const
      -> std::optional<predicate_subst>;
  auto refuted_label(const goal_form &goal, std::string_view fallback) const
      -> std::string;
  auto predicate_text(type_id refined, const ast::expr &value) const
      -> std::string;
  auto display_refined(type_id refined, const value_bindings *solved) const
      -> std::string;

  const type_table *types_;
  predicate_translator translator_;
  fact_set *facts_;
};

} // namespace cinder::semantic::detail
