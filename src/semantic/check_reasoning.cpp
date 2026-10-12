#include "checker.h"

namespace cinder::semantic::detail {

/// The id `param` is interned as, and so its key in a `param_subst`.
auto checker::param_id(const ast::type_param &param) -> type_id {
  return session_.types_.type_param(param.name, param.higher_kinded_arity,
                                    &param);
}

/// The polynomial variable standing for the value parameter interned as
/// `param` — keyed on the declaration, so a callee's `n` and its caller's
/// `n` are two unknowns (`poly_param_var`).
auto checker::value_var(type_id param) -> std::string {
  return poly_param_var(session_.types_.entry(param).name, param);
}

/// `id` as a value slot: a bare value parameter becomes the polynomial
/// over its variable, the one representation a value slot has. Anything
/// else comes back unchanged. The unlabelled-slot convention applies — no
/// declaration says what the value's type is, so it is `usize`.
auto checker::as_value_slot(type_id id) -> type_id {
  const auto &entry = session_.types_.entry(id);
  if (entry.kind != type_kind::type_param_kind || !entry.is_value_param) {
    return id;
  }
  return session_.types_.symbolic_value(session_.types_.usize_type(),
                                        poly_variable(value_var(id)));
}

/// The solver variable a bare name in a predicate or condition stands for:
/// an in-scope value parameter is its declaration's variable, anything else
/// (a local, a parameter) keys on its spelling.
auto checker::ident_var(const std::string &name) -> std::string {
  if (const auto param = lookup_type_param(name);
      param.has_value() &&
      session_.types_.entry(*param).kind == type_kind::type_param_kind) {
    return value_var(*param);
  }
  return name;
}

/// A solution read back as a scope: each parameter's spelling to its
/// answer, for resolving a body written in those spellings.
auto checker::bindings_by_name(const param_subst &bindings) -> type_scope {
  auto scope = type_scope{};
  for (const auto &[param, solved] : bindings) {
    scope.insert_or_assign(session_.types_.entry(param).name, solved);
  }
  return scope;
}

/// The atom key standing for an expression the solver cannot look inside —
/// a field access, or a call to a `pure` function. Uninterpreted: `f(x)`
/// equals `f(x)` and nothing else is known about it, so the key must be
/// canonical (two spellings of the same argument must produce one key) and
/// injective (two different arguments must not).
auto checker::atom_key(const ast::expr &expr, const predicate_subst &subst)
    -> std::optional<std::string> {
  switch (expr.kind) {
  case ast::node_kind::ident_expr: {
    const auto &ident = dynamic_cast<const ast::ident_expr &>(expr);
    if (const auto it = subst.values.find(ident.name);
        it != subst.values.end()) {
      return it->second.atom_text();
    }
    return ident_var(ident.name);
  }
  case ast::node_kind::field_expr: {
    const auto &field = dynamic_cast<const ast::field_expr &>(expr);
    if (field.object == nullptr) {
      return std::nullopt;
    }
    const auto object = atom_key(*field.object, subst);
    return object.has_value()
               ? std::optional{std::format("{}.{}", *object, field.field_name)}
               : std::nullopt;
  }
  case ast::node_kind::module_path_expr: {
    // `p.value` and `self.value` parse as *paths*, not field accesses —
    // `a.b` is ambiguous between a projection and a module member until
    // names are resolved, and the parser does not resolve names. So the
    // shape a predicate about a struct field actually arrives in is this
    // one, and treating it as an atom keyed on the written path is both
    // correct and exactly what makes an `invariant self.value > 0`
    // discharge a `positive` at `p.value`: the fact and the goal key alike.
    const auto &path = dynamic_cast<const ast::module_path_expr &>(expr);
    if (path.segments.empty()) {
      return std::nullopt;
    }
    auto segments = path.segments;
    if (const auto it = subst.values.find(segments.front());
        it != subst.values.end()) {
      segments.front() = it->second.atom_text();
    }
    return join_strings(segments, ".");
  }
  case ast::node_kind::call_expr: {
    // A method call `v.len()` keys as `len(v)`, a free call `f(x)` as
    // `f(x)` — so a fact stated one way discharges a goal stated the other.
    const auto &call = dynamic_cast<const ast::call_expr &>(expr);
    if (call.callee == nullptr) {
      return std::nullopt;
    }
    auto name = std::string{};
    auto arguments = std::vector<std::string>{};
    if (call.callee->kind == ast::node_kind::field_expr) {
      const auto &field = dynamic_cast<const ast::field_expr &>(*call.callee);
      if (field.object == nullptr) {
        return std::nullopt;
      }
      const auto receiver = atom_key(*field.object, subst);
      if (!receiver.has_value()) {
        return std::nullopt;
      }
      name = field.field_name;
      arguments.push_back(*receiver);
    } else if (call.callee->kind == ast::node_kind::ident_expr) {
      name = dynamic_cast<const ast::ident_expr &>(*call.callee).name;
    } else {
      return std::nullopt;
    }
    for (const auto &arg : call.args) {
      if (arg.value == nullptr) {
        return std::nullopt;
      }
      const auto key = atom_key(*arg.value, subst);
      if (!key.has_value()) {
        return std::nullopt;
      }
      arguments.push_back(*key);
    }
    return std::format("{}({})", name, join_strings(arguments, ","));
  }
  default:
    return std::nullopt;
  }
}

/// Translates a value expression into a solver polynomial, treating what it
/// cannot decompose as an opaque atom. `nullopt` means the expression left
/// the fragment entirely (`m * n`, `n / 2`, an index, a cast) — see
/// `spec/dependent-types-design.md` §4.4.
auto checker::solver_poly(const ast::expr &expr, const predicate_subst &subst)
    -> std::optional<linear_poly> {
  switch (expr.kind) {
  case ast::node_kind::literal_expr: {
    const auto &lit = dynamic_cast<const ast::literal_expr &>(expr);
    if (lit.lit_kind != token_kind::int_lit) {
      return std::nullopt;
    }
    const auto value = parse_integer_literal(lit.value);
    return value.has_value()
               ? std::optional{poly_constant(static_cast<int64_t>(*value))}
               : std::nullopt;
  }
  case ast::node_kind::ident_expr: {
    const auto &ident = dynamic_cast<const ast::ident_expr &>(expr);
    if (const auto it = subst.values.find(ident.name);
        it != subst.values.end()) {
      return it->second;
    }
    return poly_variable(ident_var(ident.name));
  }
  case ast::node_kind::unary_expr: {
    const auto &unary = dynamic_cast<const ast::unary_expr &>(expr);
    if (unary.op != ast::unary_op::neg || unary.operand == nullptr) {
      return std::nullopt;
    }
    const auto operand = solver_poly(*unary.operand, subst);
    return operand.has_value() ? std::optional{poly_negate(*operand)}
                               : std::nullopt;
  }
  case ast::node_kind::binary_expr: {
    const auto &binary = dynamic_cast<const ast::binary_expr &>(expr);
    if (binary.lhs == nullptr || binary.rhs == nullptr) {
      return std::nullopt;
    }
    const auto left = solver_poly(*binary.lhs, subst);
    const auto right = solver_poly(*binary.rhs, subst);
    if (!left.has_value() || !right.has_value()) {
      return std::nullopt;
    }
    switch (binary.op) {
    case ast::binary_op::add:
      return poly_add(*left, *right);
    case ast::binary_op::sub:
      return poly_sub(*left, *right);
    case ast::binary_op::mul:
      if (left->is_constant()) {
        return poly_scale(*right, left->constant);
      }
      if (right->is_constant()) {
        return poly_scale(*left, right->constant);
      }
      return std::nullopt;
    default:
      return std::nullopt;
    }
  }
  case ast::node_kind::field_expr:
  case ast::node_kind::module_path_expr:
  case ast::node_kind::call_expr: {
    const auto key = atom_key(expr, subst);
    return key.has_value() ? std::optional{poly_variable(*key)} : std::nullopt;
  }
  default:
    return std::nullopt;
  }
}

/// The comparison a relational operator makes, once `negated` has been
/// folded in — `not (a < b)` is `a >= b`, so negation is a rewrite of the
/// operator, never a wrapper around the constraint.
auto checker::negate_comparison(ast::binary_op op) -> ast::binary_op {
  switch (op) {
  case ast::binary_op::eq_eq:
    return ast::binary_op::bang_eq;
  case ast::binary_op::bang_eq:
    return ast::binary_op::eq_eq;
  case ast::binary_op::lt:
    return ast::binary_op::gt_eq;
  case ast::binary_op::lt_eq:
    return ast::binary_op::gt;
  case ast::binary_op::gt:
    return ast::binary_op::lt_eq;
  case ast::binary_op::gt_eq:
    return ast::binary_op::lt;
  default:
    return op;
  }
}

/// Builds `lhs OP rhs` as a constraint `poly REL 0`. `<` and `<=` have no
/// relation of their own — they are the same statement about the negated
/// polynomial — and `<`/`>` shift the constant by one because the fragment
/// is over integers, where `a > b` *is* `a >= b + 1`.
auto checker::comparison_constraint(ast::binary_op op, const linear_poly &lhs,
                                    const linear_poly &rhs, std::string label)
    -> std::optional<constraint> {
  const auto difference = poly_sub(lhs, rhs);
  auto shifted = [&](int64_t by) -> linear_poly {
    auto poly = difference;
    poly.constant += by;
    return poly;
  };
  switch (op) {
  case ast::binary_op::eq_eq:
    return constraint{
        .poly = difference, .rel = relation::eq, .label = std::move(label)};
  case ast::binary_op::bang_eq:
    return constraint{
        .poly = difference, .rel = relation::ne, .label = std::move(label)};
  case ast::binary_op::gt_eq:
    return constraint{
        .poly = difference, .rel = relation::ge, .label = std::move(label)};
  case ast::binary_op::gt:
    return constraint{
        .poly = shifted(-1), .rel = relation::ge, .label = std::move(label)};
  case ast::binary_op::lt_eq:
    return constraint{.poly = poly_negate(difference),
                      .rel = relation::ge,
                      .label = std::move(label)};
  case ast::binary_op::lt:
    return constraint{.poly = poly_negate(shifted(1)),
                      .rel = relation::ge,
                      .label = std::move(label)};
  default:
    return std::nullopt;
  }
}

/// Renders a predicate back into source-like text with the substitution
/// applied, so a diagnostic about `index[n]`'s `self < n` can say
/// ``cannot prove `i < n` `` rather than quoting a predicate the user never
/// wrote about a `self` that isn't there.
auto checker::describe(const ast::expr &expr, const predicate_subst &subst)
    -> std::string {
  switch (expr.kind) {
  case ast::node_kind::literal_expr:
    return {dynamic_cast<const ast::literal_expr &>(expr).value};
  case ast::node_kind::ident_expr: {
    const auto &ident = dynamic_cast<const ast::ident_expr &>(expr);
    if (const auto it = subst.spellings.find(ident.name);
        it != subst.spellings.end()) {
      return it->second;
    }
    return ident.name;
  }
  case ast::node_kind::field_expr: {
    const auto &field = dynamic_cast<const ast::field_expr &>(expr);
    return field.object != nullptr
               ? std::format("{}.{}", describe(*field.object, subst),
                             field.field_name)
               : field.field_name;
  }
  case ast::node_kind::module_path_expr: {
    const auto &path = dynamic_cast<const ast::module_path_expr &>(expr);
    auto segments = path.segments;
    if (!segments.empty()) {
      if (const auto it = subst.spellings.find(segments.front());
          it != subst.spellings.end()) {
        segments.front() = it->second;
      }
    }
    return join_strings(segments, ".");
  }
  case ast::node_kind::call_expr: {
    const auto &call = dynamic_cast<const ast::call_expr &>(expr);
    auto arguments = std::vector<std::string>{};
    for (const auto &arg : call.args) {
      arguments.push_back(arg.value != nullptr ? describe(*arg.value, subst)
                                               : "_");
    }
    const auto callee =
        call.callee != nullptr ? describe(*call.callee, subst) : "_";
    return std::format("{}({})", callee, join_strings(arguments, ", "));
  }
  case ast::node_kind::unary_expr: {
    const auto &unary = dynamic_cast<const ast::unary_expr &>(expr);
    const auto operand =
        unary.operand != nullptr ? describe(*unary.operand, subst) : "_";
    switch (unary.op) {
    case ast::unary_op::neg:
      return std::format("-{}", operand);
    case ast::unary_op::logical_not:
      return std::format("not {}", operand);
    default:
      return operand;
    }
  }
  case ast::node_kind::binary_expr: {
    const auto &binary = dynamic_cast<const ast::binary_expr &>(expr);
    const auto left =
        binary.lhs != nullptr ? describe(*binary.lhs, subst) : "_";
    const auto right =
        binary.rhs != nullptr ? describe(*binary.rhs, subst) : "_";
    return std::format("{} {} {}", left, binary_op_spelling(binary.op), right);
  }
  default:
    return "...";
  }
}

/// Translates a boolean predicate into a goal in disjunctive normal form.
/// An empty result means "not expressible", which upstream reads as "not
/// provable" — the graceful path, never a silent acceptance.
auto checker::goal_from(const ast::expr &expr, const predicate_subst &subst,
                        bool negated) -> goal_form {
  switch (expr.kind) {
  case ast::node_kind::unary_expr: {
    const auto &unary = dynamic_cast<const ast::unary_expr &>(expr);
    if (unary.op == ast::unary_op::logical_not && unary.operand != nullptr) {
      return goal_from(*unary.operand, subst, !negated);
    }
    break;
  }
  case ast::node_kind::binary_expr: {
    const auto &binary = dynamic_cast<const ast::binary_expr &>(expr);
    if (binary.lhs == nullptr || binary.rhs == nullptr) {
      break;
    }
    // De Morgan: negation swaps the connective, so one traversal handles
    // both polarities.
    const auto conjunctive =
        (binary.op == ast::binary_op::logical_and) != negated;
    if (binary.op == ast::binary_op::logical_and ||
        binary.op == ast::binary_op::logical_or) {
      const auto left = goal_from(*binary.lhs, subst, negated);
      const auto right = goal_from(*binary.rhs, subst, negated);
      if (!conjunctive) {
        // Disjunction: either alternative will do, so an inexpressible side
        // just drops out and the other still stands on its own.
        auto combined = left;
        combined.insert(combined.end(), right.begin(), right.end());
        return combined;
      }
      // Conjunction: every conjunct must be proved, so an inexpressible
      // side sinks the whole goal.
      if (left.empty() || right.empty()) {
        return {};
      }
      auto combined = goal_form{};
      for (const auto &left_alt : left) {
        for (const auto &right_alt : right) {
          auto merged = left_alt;
          merged.insert(merged.end(), right_alt.begin(), right_alt.end());
          combined.push_back(std::move(merged));
        }
      }
      return combined;
    }

    const auto left = solver_poly(*binary.lhs, subst);
    const auto right = solver_poly(*binary.rhs, subst);
    if (!left.has_value() || !right.has_value()) {
      return {};
    }
    const auto op = negated ? negate_comparison(binary.op) : binary.op;
    auto built =
        comparison_constraint(op, *left, *right, describe(expr, subst));
    if (!built.has_value()) {
      return {};
    }
    return goal_form{fact_set{std::move(*built)}};
  }
  default:
    break;
  }

  // Any other boolean term — a call to a `pure` predicate function, say —
  // enters as `atom == 1`. Deliberately the weakest useful rule: an
  // identical fact discharges it, and nothing else does.
  const auto atom = atom_key(expr, subst);
  if (!atom.has_value()) {
    return {};
  }
  auto poly = poly_variable(*atom);
  poly.constant -= 1;
  return goal_form{fact_set{constraint{
      .poly = std::move(poly),
      .rel = negated ? relation::ne : relation::eq,
      .label = describe(expr, subst),
  }}};
}

/// Translates a boolean predicate into facts that may be *assumed*. Unlike
/// a goal this is a plain conjunction: a disjunctive fact carries no
/// information the solver's fragment can use, so it is dropped — sound,
/// because forgetting a fact can only make the compiler prove less.
auto checker::facts_from(const ast::expr &expr, const predicate_subst &subst,
                         bool negated) -> fact_set {
  const auto goal = goal_from(expr, subst, negated);
  return goal.size() == 1 ? goal.front() : fact_set{};
}

/// The polynomial standing for a resolved value slot (`3`, `n + 1`, an
/// unsubstituted `n`). This is how a refinement's arguments
/// reach its predicate: `index[n + 1]`'s `self < n` must become
/// `self < n + 1`.
auto checker::slot_poly(type_id slot) -> std::optional<linear_poly> {
  const auto &entry = session_.types_.entry(slot);
  switch (entry.kind) {
  case type_kind::const_value_kind:
  case type_kind::symbolic_value_kind:
    return entry.value;
  default:
    return std::nullopt;
  }
}

/// The substitution under which a refinement's predicate speaks about
/// `value` (a concrete expression, rendered as `spelling`): `self` becomes
/// the value, and each of the declaration's value parameters becomes the
/// argument supplied for it. `nullopt` when an argument falls outside the
/// fragment, which makes the whole predicate unusable rather than
/// half-substituted.
auto checker::refinement_subst(const type_entry &refinement,
                               const linear_poly &value, std::string spelling,
                               const value_bindings *solved)
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
    auto poly = slot_poly(refinement.args[i]);
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
auto checker::narrowing_goal(type_id refined, const ast::expr &value,
                             const value_bindings *solved) -> goal_form {
  const auto subst_value = solver_poly(value, predicate_subst{});
  if (!subst_value.has_value()) {
    return {};
  }
  const auto spelling = describe(value, predicate_subst{});

  auto goal = goal_form{};
  for (auto current = refined;
       session_.types_.entry(current).kind == type_kind::refinement_kind;
       current = session_.types_.entry(current).result) {
    const auto &entry = session_.types_.entry(current);
    if (entry.predicate == nullptr) {
      continue;
    }
    const auto subst = refinement_subst(entry, *subst_value, spelling, solved);
    if (!subst.has_value()) {
      return {};
    }
    const auto part = goal_from(*entry.predicate, *subst, false);
    if (part.empty()) {
      return {};
    }
    if (goal.empty()) {
      goal = part;
      continue;
    }
    auto combined = goal_form{};
    for (const auto &left : goal) {
      for (const auto &right : part) {
        auto merged = left;
        merged.insert(merged.end(), right.begin(), right.end());
        combined.push_back(std::move(merged));
      }
    }
    goal = std::move(combined);
  }
  return goal;
}

/// The facts currently in scope, rendered for a diagnostic. Only the ones
/// that actually mention something the goal mentions are worth showing —
/// a reader who wants to know why `i < n` didn't follow is not helped by
/// being told what is known about `k`.
auto checker::known_facts_note(const goal_form &goal)
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
  for (const auto &fact : function_.facts_) {
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
/// constraint to name and the compiler must quote the source instead. A
/// placeholder like "the refinement's predicate" would read as the compiler
/// having lost track of something, when in fact it knows exactly which
/// predicate it is and exactly why it can't decide it.
auto checker::goal_label(const goal_form &goal, std::string_view fallback)
    -> std::string {
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
auto checker::refuted_label(const goal_form &goal, std::string_view fallback)
    -> std::string {
  for (const auto &alternative : goal) {
    for (const auto &item : alternative) {
      if (!item.label.empty() &&
          solve(function_.facts_, goal_form{fact_set{item}}) ==
              proof_result::refuted) {
        return item.label;
      }
    }
  }
  return goal_label(goal, fallback);
}

/// The source text of a refinement's predicate, restated about `value` —
/// the honest thing to quote when the predicate is real but undecidable.
auto checker::predicate_text(type_id refined, const ast::expr &value)
    -> std::string {
  const auto &entry = session_.types_.entry(refined);
  if (entry.predicate == nullptr) {
    return std::format("the predicate of `{}`",
                       session_.types_.display(refined));
  }
  auto subst = predicate_subst{};
  subst.spellings.emplace("self", describe(value, predicate_subst{}));
  return describe(*entry.predicate, subst);
}

/// Spells a refinement the way it reads *at this site*, with the callee's
/// value parameters replaced by what the call site pinned them to — so the
/// diagnostic says "expected `index[4]`" rather than "expected `index[n]`"
/// and quotes an `n` the reader cannot see from where they are standing.
auto checker::display_refined(type_id refined, const value_bindings *solved)
    -> std::string {
  const auto &entry = session_.types_.entry(refined);
  if (solved == nullptr || entry.decl == nullptr || entry.args.empty()) {
    return session_.types_.display(refined);
  }
  auto arguments = std::vector<std::string>{};
  for (const auto arg : entry.args) {
    const auto poly = slot_poly(arg);
    arguments.push_back(poly.has_value()
                            ? poly_substitute(*poly, *solved).display()
                            : session_.types_.display(arg));
  }
  return std::format("{}[{}]", entry.decl->name, join_strings(arguments, ", "));
}

/// Discharges the obligation a *narrowing* incurs: using a value where a
/// more refined type is required. Widening is free and never lands here
/// (`compatible` accepts both directions structurally — see `types.h`);
/// this is the sole place the predicate is actually made to hold.
///
/// Proved: silent. Refuted: an error, because the code is wrong on every
/// path that reaches it. Unproven: the graceful path — a diagnostic naming
/// the goal, the facts, and both routes out.
auto checker::check_narrowing(type_id expected, type_id found,
                              const ast::expr *value, source_span span,
                              std::string_view context,
                              const value_bindings *solved) -> void {
  if (value == nullptr ||
      session_.types_.entry(expected).kind != type_kind::refinement_kind) {
    return;
  }
  // Already at least as refined as required: no debt. `found` being the
  // same refinement (or a refinement *of* it) carries the predicate along
  // with it, which is the whole point of the type surviving assignment.
  if (refines(found, expected)) {
    return;
  }
  // A value whose type isn't even the right shape has a type error already
  // reported against it; adding an unprovable-predicate error on top would
  // be noise about a value that was never going to work.
  if (session_.types_.is_unknown(session_.types_.strip_refinement(found))) {
    return;
  }

  const auto goal = narrowing_goal(expected, *value, solved);
  const auto outcome =
      goal.empty() ? proof_result::unknown : solve(function_.facts_, goal);
  if (outcome == proof_result::proved) {
    return;
  }

  const auto refined_name = display_refined(expected, solved);
  const auto base_name =
      session_.types_.display(session_.types_.strip_refinement(expected));
  const auto fallback_text = predicate_text(expected, *value);
  const auto goal_text = outcome == proof_result::refuted
                             ? refuted_label(goal, fallback_text)
                             : goal_label(goal, fallback_text);
  const auto message =
      outcome == proof_result::refuted
          ? std::format("`{}` is never true here, so this value can never "
                        "be a `{}`",
                        goal_text, refined_name)
          : std::format("cannot prove `{}` {}", goal_text, context);

  auto diag = diagnostic(diagnostic_level::error, message, file_.file_id_);
  diag.with_label(span, std::format("expected `{}`, found `{}`", refined_name,
                                    session_.types_.display(found)));
  if (const auto known = known_facts_note(goal)) {
    diag.children.emplace_back(diagnostic_level::note, *known, file_.file_id_);
  }
  if (outcome == proof_result::refuted) {
    diag.with_help(std::format(
        "This isn't a gap in the compiler's reasoning — the facts in scope "
        "rule the predicate out. Either the value or the `{}` bound is "
        "wrong.",
        refined_name));
  } else {
    diag.with_help(std::format(
        "Either constrain the value upstream — take it as a `{}` in the "
        "first place, or add a `pre` that says so — or prove it here:\n"
        "    if let @some(proof) = {}.try_from(<value>):\n"
        "        ...\n"
        "which turns the `{}` into a `{}` by checking the predicate at "
        "runtime.",
        refined_name, refined_name, base_name, refined_name));
  }
  emit_diag(diag);
  mark_error();
}

/// Solves a callee's value parameters against a concrete argument type:
/// matching `array[T, n]` against `array[int32, 4]` binds `n := 4`;
/// matching `vec[T, n + 1]` against a caller's `vec[int32, m + 1]` binds
/// `n := m`. What `out` already holds is substituted rather than solved
/// again, and a new answer never displaces an old one: a conflicting second
/// answer means the call is already ill-typed (`compatible` says so), and
/// guessing between them would only add a confusing second diagnostic.
///
/// This is what makes a dependent signature usable from outside itself. The
/// callee's `index[n]` means nothing at a call site until `n` is known, and
/// this is where it becomes known — from the *other* argument, which is
/// exactly how `safe_get(v, i)` ties the index to the array.
auto checker::match_value_params(type_id param, type_id argument,
                                 value_bindings &out) -> void {
  for (auto &[var, value] :
       infer::match_pattern(session_.types_, param, argument, out).values) {
    out.try_emplace(var, std::move(value));
  }
}

/// Everything the *type* of a binding says about its value, added to the
/// facts in scope: a refined type contributes its predicate (with `self`
/// bound to the name), and an array contributes its length
/// (`len(v) = n`) — which is what lets an index into it be proved in bounds
/// against a matching `index[n]`.
auto checker::assume_binding(std::string_view name, type_id type) -> void {
  const auto value = poly_variable(std::string(name));

  const auto bare = strip_refs(type);
  for (auto current = bare;
       session_.types_.entry(current).kind == type_kind::refinement_kind;
       current = session_.types_.entry(current).result) {
    const auto &entry = session_.types_.entry(current);
    if (entry.predicate == nullptr) {
      continue;
    }
    const auto subst =
        refinement_subst(entry, value, std::string(name), nullptr);
    if (!subst.has_value()) {
      continue;
    }
    for (auto &fact : facts_from(*entry.predicate, *subst, false)) {
      function_.facts_.push_back(std::move(fact));
    }
  }

  const auto &entry =
      session_.types_.entry(session_.types_.strip_refinement(bare));
  if (entry.kind == type_kind::array_kind && !entry.args.empty()) {
    if (const auto length = slot_poly(entry.args.front())) {
      auto poly =
          poly_sub(poly_variable(std::format("len({})", name)), *length);
      function_.facts_.push_back(constraint{
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
    for (auto &fact : facts_from(*entry.decl->invariant, subst, false)) {
      function_.facts_.push_back(std::move(fact));
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
//  without it, because `try_from` covers everything. It is deliberately the
//  last thing built, and deliberately does not try to be clever — nothing
//  narrows through a function call, a closure, or an `&mut`.
// ------------------------------------------------------------------------

/// Assumes a condition (or its negation) for the region it dominates.
auto checker::assume_condition(const ast::expr *condition, bool negated)
    -> void {
  // An `if let` plants an `error_expr` in `condition` on every parse (see
  // `param_usage_inferrer::walk_expr`); it says nothing about any value.
  if (condition == nullptr || condition->has_error) {
    return;
  }
  for (auto &fact : facts_from(*condition, predicate_subst{}, negated)) {
    function_.facts_.push_back(std::move(fact));
  }
}

/// Whether a solver atom is *about* `name` — the name itself (`x`), a
/// projection of it (`x.len`), or a call over it (`len(x)`, `f(x, y)`).
/// Used to decide what an assignment invalidates, so it errs toward
/// forgetting too much rather than too little: a fact wrongly kept is
/// unsound, a fact wrongly dropped merely costs a proof.
auto checker::atom_mentions(std::string_view atom, std::string_view name)
    -> bool {
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

/// Forgets everything known about `name`. Called when a value is assigned
/// to, or lent mutably — at which point every fact that mentioned it
/// describes a value that no longer exists.
auto checker::invalidate_facts(std::string_view name) -> void {
  if (name.empty()) {
    return;
  }
  std::erase_if(function_.facts_, [&](const constraint &fact) -> bool {
    return std::ranges::any_of(fact.poly.terms,
                               [&](const poly_term &term) -> bool {
                                 return atom_mentions(term.var, name);
                               });
  });
}

/// Builds the facts that hold on entry to a function body, in the order the
/// design doc lays out (§5.1): value-parameter domains, then each
/// parameter's type, then the preconditions. A `pre` is an *obligation* at
/// every call site and a *fact* inside the callee — that duality is the
/// whole of contract reasoning, and this is the half of it that assumes.
auto checker::collect_function_facts(const ast::func_decl &decl) -> void {
  for (const auto &param : decl.type_params) {
    if (!param.is_value_param || param.name.empty()) {
      continue;
    }
    const auto declared =
        param.bound_or_type != nullptr
            ? resolve_type(*param.bound_or_type, current_resolve_ctx())
            : session_.types_.usize_type();
    const auto &entry = session_.types_.entry(declared);
    const auto unsigned_domain =
        entry.kind == type_kind::builtin_kind &&
        (entry.name.starts_with("uint") || entry.name == "usize" ||
         entry.name == "byte");
    if (unsigned_domain) {
      function_.facts_.push_back(constraint{
          .poly = poly_variable(value_var(param_id(param))),
          .rel = relation::ge,
          .label = std::format("{} >= 0", param.name),
      });
    }
  }

  for (const auto &param : decl.params) {
    if (param.pattern == nullptr ||
        param.pattern->kind != ast::node_kind::binding_pattern) {
      continue;
    }
    const auto &binding =
        dynamic_cast<const ast::binding_pattern &>(*param.pattern);
    if (const auto *found = lookup_value(binding.name)) {
      assume_binding(binding.name, found->type);
    }
  }

  // Each `pre` is now weighed against everything already known — the
  // parameters' own types (a refinement or a `usize` domain is a fact
  // about every value that could ever be passed) plus the preconditions
  // ahead of it, which are checked before it at run time and so hold by
  // the time it is evaluated. A `pre` that follows from those needs no
  // runtime check: it cannot fail on any call, from anywhere, for the same
  // reason `p: positive` makes `pre p > 0` say nothing new. That — and not
  // "some call site happened to satisfy it" — is what makes an elision
  // sound for a check the callee performs once on entry, on behalf of
  // callers it may never see (`checked_types::elided_contracts`).
  for (const auto &contract : decl.contracts) {
    if (!contract.is_pre || contract.condition == nullptr ||
        contract.condition->has_error) {
      continue;
    }
    if (const auto goal =
            goal_from(*contract.condition, predicate_subst{}, false);
        !goal.empty() &&
        solve(function_.facts_, goal) == proof_result::proved) {
      session_.elided_contracts_.insert(&contract);
    }
    for (auto &fact :
         facts_from(*contract.condition, predicate_subst{}, false)) {
      function_.facts_.push_back(std::move(fact));
    }
  }
}

/// Whether `found` already carries at least the predicate `expected`
/// demands — the same refinement, or one layered on top of it.
auto checker::refines(type_id found, type_id expected) -> bool {
  for (auto current = found;
       session_.types_.entry(current).kind == type_kind::refinement_kind;
       current = session_.types_.entry(current).result) {
    if (current == expected) {
      return true;
    }
  }
  return false;
}

/// Builds the candidate name list for "did you mean" suggestions on an
/// undefined type: common builtins plus every type/trait/concept in the
/// current module and every in-scope generic parameter.
auto checker::type_name_candidates() -> std::vector<std::string> {
  auto candidates = std::vector<std::string>{
      "bool", "int32", "int64",  "float64", "str",   "char",
      "unit", "list",  "option", "result",  "usize", "byte",
  };
  if (file_.module_ != nullptr) {
    for (const auto &[name, decl] : file_.module_->types) {
      candidates.push_back(name);
    }
    for (const auto &[name, decl] : file_.module_->traits) {
      candidates.push_back(name);
    }
    for (const auto &[name, decl] : file_.module_->concepts) {
      candidates.push_back(name);
    }
  }
  for (const auto &scope : function_.type_params_) {
    for (const auto &[name, id] : scope) {
      candidates.push_back(name);
    }
  }
  return candidates;
}

/// Emits "undefined type" once per distinct name per file (via
/// `function_.reported_undefined_`), with a suggestion or a declare/import
/// hint.
auto checker::emit_undefined_type(const ast::named_type &named,
                                  std::string_view name) -> void {
  if (!function_.reported_undefined_.insert(std::format("type:{}", name))
           .second) {
    return;
  }
  auto diag =
      diagnostic(diagnostic_level::error,
                 std::format("undefined type `{}`", name), file_.file_id_);
  diag.with_label(named.span, "not found in this scope");
  if (const auto suggestion = best_suggestion(name, type_name_candidates())) {
    diag.with_help(std::format("did you mean `{}`?", *suggestion));
  } else {
    diag.with_help(std::format(
        "Declare `type {}` in module `{}`, or import it with `use`.", name,
        file_.module_name_));
  }
  emit_diag(diag);
  mark_error();
}

/// Checks generic-argument arity against `decl.type_params` (when
/// arguments were written at all — omitting them entirely is allowed and
/// leaves parameters `unknown`), then builds the instantiated type.
auto checker::instantiate_user_type(const ast::type_decl &decl,
                                    std::string_view owner_module,
                                    const ast::named_type &named,
                                    const resolve_ctx &ctx) -> type_id {
  // Kind-directed: a bare generic user type filling a higher-kinded slot
  // names the constructor itself, exactly as a bare prelude generic does.
  if (named.type_args.empty() && ctx.expected_ctor_arity.has_value() &&
      !decl.type_params.empty()) {
    return check_ctor_against_expected(
        session_.types_.ctor_ref(decl.name, owner_module, &decl,
                                 decl.type_params.size()),
        decl.name, decl.type_params.size(), named.span, ctx);
  }
  const auto args = resolve_type_args(named.type_args, ctx, &decl.type_params);

  if (!ctx.quiet && !named.type_args.empty() &&
      named.type_args.size() != decl.type_params.size()) {
    auto diag =
        diagnostic(diagnostic_level::error,
                   std::format("type `{}` expects {} type argument{}, found {}",
                               decl.name, decl.type_params.size(),
                               decl.type_params.size() == 1 ? "" : "s",
                               named.type_args.size()),
                   file_.file_id_);
    diag.with_label(named.span, "wrong number of type arguments");
    emit_diag(diag);
    mark_error();
    return k_error_type;
  }

  return make_user_type(decl, owner_module, args);
}

/// The parameter substitution a declaration's own body is resolved under:
/// each declared parameter bound to the argument supplied for it (or left
/// `unknown` when the caller wrote no arguments at all, which is legal).
auto checker::bindings_for_decl(const ast::type_decl &decl,
                                const std::vector<type_id> &args)
    -> type_scope {
  auto bindings = type_scope{};
  for (size_t i = 0; i < decl.type_params.size(); ++i) {
    bindings.emplace(decl.type_params[i].name,
                     i < args.size() ? args[i] : k_unknown_type);
  }
  return bindings;
}

/// Mints the nominal type of a refinement *declaration*
/// (`type index[n: usize] = usize where self < n`). Declaration-nominal, so
/// `index[n]` is spelled `index[n]` in every diagnostic and stays distinct
/// from any other refinement of `usize` — while the predicate rides along
/// on the type entry for the solver, which sees straight through the name
/// (`spec/dependent-types-design.md` §3).
auto checker::make_refinement_type(const ast::type_decl &decl,
                                   std::string_view owner_module,
                                   std::vector<type_id> args) -> type_id {
  const auto &refinement =
      dynamic_cast<const ast::refinement_type &>(*decl.definition);

  auto bindings = bindings_for_decl(decl, args);
  const auto *owner = index_.find_module(owner_module);
  const auto ctx =
      resolve_ctx{.module = owner != nullptr ? owner : file_.module_,
                  .param_bindings = &bindings,
                  .use_type_param_stack = false,
                  .quiet = true};
  const auto base = refinement.base != nullptr
                        ? resolve_type(*refinement.base, ctx)
                        : k_unknown_type;

  auto display_name = decl.name;
  if (!args.empty()) {
    display_name += '[';
    for (size_t i = 0; i < args.size(); ++i) {
      if (i != 0) {
        display_name += ", ";
      }
      display_name += session_.types_.display(args[i]);
    }
    display_name += ']';
  }

  const auto *predicate =
      dynamic_cast<const ast::expr *>(refinement.predicate.get());
  return session_.types_.refinement_of(
      &decl, display_name, owner_module, base, std::move(args),
      predicate != nullptr && !predicate->has_error ? predicate : nullptr);
}

/// Builds a user type id, expanding alias declarations to their target.
auto checker::make_user_type(const ast::type_decl &decl,
                             std::string_view owner_module,
                             std::vector<type_id> args) -> type_id {
  const auto is_struct_or_sum =
      decl.definition != nullptr &&
      (decl.definition->kind == ast::node_kind::struct_type_def ||
       decl.definition->kind == ast::node_kind::sum_type_def);
  if (is_struct_or_sum || decl.definition == nullptr) {
    return session_.types_.user_type(decl, owner_module, std::move(args));
  }

  // A refinement declaration is a type in its own right, not an alias for
  // its base — that is the whole of Phase 2.
  if (decl.definition->kind == ast::node_kind::refinement_type) {
    if (!cache_.aliases_in_progress_.insert(&decl).second) {
      return k_unknown_type; // cycle; reported by declaration checking
    }
    const auto result = make_refinement_type(decl, owner_module, args);
    cache_.aliases_in_progress_.erase(&decl);
    return result;
  }

  // Alias (`type meters = float64`) — expand to the target.
  if (!cache_.aliases_in_progress_.insert(&decl).second) {
    return k_unknown_type; // alias cycle; reported by declaration checking
  }
  auto param_bindings = bindings_for_decl(decl, args);
  const auto *owner = index_.find_module(owner_module);
  auto ctx = resolve_ctx{.module = owner != nullptr ? owner : file_.module_,
                         .param_bindings = &param_bindings,
                         .use_type_param_stack = false,
                         .quiet = true};
  auto resolved = k_unknown_type;
  switch (decl.definition->kind) {
  case ast::node_kind::named_type:
  case ast::node_kind::tuple_type:
  case ast::node_kind::slice_type:
  case ast::node_kind::array_type:
  case ast::node_kind::ref_type:
  case ast::node_kind::ptr_type:
  case ast::node_kind::fn_type:
  case ast::node_kind::quote_type:
  case ast::node_kind::union_type:
  case ast::node_kind::refinement_type:
  case ast::node_kind::bound_type:
    resolved = resolve_type(
        dynamic_cast<const ast::type_expr &>(*decl.definition), ctx);
    break;
  default:
    break;
  }
  cache_.aliases_in_progress_.erase(&decl);
  return resolved;
}

/// `ctx` with any constructor expectation cleared, for resolving the
/// *arguments* of an application — an argument position is an ordinary
/// type position even when the application itself fills a higher-kinded
/// slot.
auto checker::argument_ctx(const resolve_ctx &ctx) -> resolve_ctx {
  auto inner = ctx;
  inner.expected_ctor_arity = std::nullopt;
  return inner;
}

/// Checks a resolved constructor (or constructor-kinded parameter) against
/// the arity the surrounding higher-kinded slot expects. Returns the
/// constructor's own id on a match; otherwise explains the kind mismatch —
/// what was required, what was found, and how to bridge the gap — and
/// returns `k_error_type`.
auto checker::check_ctor_against_expected(type_id head, std::string_view name,
                                          size_t arity, source_span span,
                                          const resolve_ctx &ctx) -> type_id {
  const auto expected = *ctx.expected_ctor_arity;
  if (arity == expected) {
    return head;
  }
  if (!ctx.quiet) {
    const auto placeholders = [](size_t n) -> std::string {
      auto out = std::string{"[_"};
      for (size_t i = 1; i < n; ++i) {
        out += ", _";
      }
      return out + "]";
    };
    error_with_help(
        span,
        std::format("`{}` takes {} type argument{}, but this position "
                    "requires a {}-argument type constructor (`{}`)",
                    name, arity, arity == 1 ? "" : "s", expected,
                    placeholders(expected)),
        "type constructor of the wrong kind",
        std::format("A higher-kinded parameter is satisfied only by a "
                    "constructor of exactly matching arity. Wrap `{}` in a "
                    "dedicated {}-parameter struct or sum type and use "
                    "that wrapper here instead.",
                    name, expected));
  }
  return ctx.quiet ? k_unknown_type : k_error_type;
}

/// Finishes resolving a name that resolved to an in-scope generic
/// parameter (possibly higher-kinded) or to a constructor substituted for
/// one (trait-default monomorphization binding `F := option`). This is
/// where every kind rule is enforced: a kind-`*` parameter takes no
/// arguments, a constructor parameter used bare is not a type, and an
/// application must supply exactly the declared arity. Applying a
/// *concrete* constructor re-enters the ordinary interning path, so
/// `F[A]` under `F := option` yields the same `type_id` ordinary
/// `option[A]` code gets — the id-equality invariant substitution must
/// preserve.
auto checker::apply_resolved_head(type_id head, const ast::named_type &named,
                                  const resolve_ctx &ctx) -> type_id {
  // Copied, not referenced: interning below can push new entries.
  const auto entry = session_.types_.entry(head);

  if (entry.kind == type_kind::ctor_ref_kind) {
    if (named.type_args.empty()) {
      if (ctx.expected_ctor_arity.has_value()) {
        return check_ctor_against_expected(head, entry.name, entry.ctor_arity,
                                           named.span, ctx);
      }
      if (!ctx.quiet) {
        error_with_help(
            named.span,
            std::format("`{}` is a type constructor, not a type", entry.name),
            "unapplied type constructor in type position",
            std::format("Apply it to {} type argument{} — e.g. "
                        "`{}[...]` — to name a concrete type.",
                        entry.ctor_arity, entry.ctor_arity == 1 ? "" : "s",
                        entry.name));
      }
      return ctx.quiet ? k_unknown_type : k_error_type;
    }
    if (named.type_args.size() != entry.ctor_arity) {
      if (!ctx.quiet) {
        error(named.span,
              std::format("type `{}` expects {} type argument{}, found {}",
                          entry.name, entry.ctor_arity,
                          entry.ctor_arity == 1 ? "" : "s",
                          named.type_args.size()),
              "wrong number of type arguments");
      }
      return ctx.quiet ? k_unknown_type : k_error_type;
    }
    if (entry.decl != nullptr) {
      return instantiate_user_type(*entry.decl, entry.module_name, named,
                                   argument_ctx(ctx));
    }
    return session_.types_.builtin_generic(
        entry.name, resolve_type_args(named.type_args, argument_ctx(ctx)));
  }

  if (entry.kind != type_kind::type_param_kind) {
    // A value-parameter slot or an already-substituted concrete type —
    // nothing kind-shaped to check here.
    return head;
  }

  const auto arity = entry.ctor_arity;
  if (named.type_args.empty()) {
    if (arity == 0) {
      return head;
    }
    if (ctx.expected_ctor_arity.has_value()) {
      return check_ctor_against_expected(head, entry.name, arity, named.span,
                                         ctx);
    }
    if (!ctx.quiet) {
      error_with_help(
          named.span,
          std::format("`{}` is a {}-argument type constructor, not a type",
                      entry.name, arity),
          "higher-kinded parameter used as a type",
          std::format("`{}` was declared as `{}[{}]`, so it names a type "
                      "only once applied — write `{}[...]` with {} type "
                      "argument{}.",
                      entry.name, entry.name, arity == 1 ? "_" : "_, _",
                      entry.name, arity, arity == 1 ? "" : "s"));
    }
    return ctx.quiet ? k_unknown_type : k_error_type;
  }

  if (arity == 0) {
    if (!ctx.quiet) {
      error_with_help(
          named.span,
          std::format("type parameter `{}` does not take type arguments",
                      entry.name),
          "kind mismatch: applying an ordinary type parameter",
          std::format("`{}` stands for a complete type. If it should stand "
                      "for a type *constructor* like `option`, declare it "
                      "higher-kinded: `{}[_]`.",
                      entry.name, entry.name));
    }
    return ctx.quiet ? k_unknown_type : k_error_type;
  }
  if (named.type_args.size() != arity) {
    if (!ctx.quiet) {
      error(named.span,
            std::format("`{}` expects {} type argument{}, found {}", entry.name,
                        arity, arity == 1 ? "" : "s", named.type_args.size()),
            "wrong number of type arguments");
    }
    return ctx.quiet ? k_unknown_type : k_error_type;
  }
  return session_.types_.param_app(
      head, resolve_type_args(named.type_args, argument_ctx(ctx)));
}

/// Resolves a named-type reference through, in order: a session-owned
/// multi-segment path; an in-scope generic parameter or substitution;
/// `self`; a builtin scalar or prelude container; the current module's
/// declarations; an imported name; a prelude trait used in bound position;
/// or (failing all of that) reports an undefined-type error, unless
/// suppressed by `ctx.quiet` or an external wildcard import that might
/// plausibly supply the name.
auto checker::resolve_named_type(const ast::named_type &named,
                                 const resolve_ctx &ctx) -> type_id {
  if (named.path.empty()) {
    return k_unknown_type;
  }

  if (named.path.size() == 2 && named.path.front() == "self") {
    // `self.output` — an associated-type reference, not a module path.
    if (const auto it = function_.self_assoc_types_.find(named.path.back());
        it != function_.self_assoc_types_.end()) {
      return it->second;
    }
    return k_unknown_type;
  }

  if (named.path.size() > 1) {
    if (const auto found = find_type_decl_by_path(named.path)) {
      return instantiate_user_type(*found->first, found->second, named, ctx);
    }
    return k_unknown_type;
  }

  const auto &name = named.path.front();

  // Generic parameters in scope. `apply_resolved_head` owns every kind
  // rule: bare kind-`*` params pass through, `F[A]` becomes a parameter
  // application (or, under a concrete substitution, the real applied
  // type), and misuse gets a kind diagnostic.
  if (ctx.param_bindings != nullptr) {
    if (const auto it = ctx.param_bindings->find(name);
        it != ctx.param_bindings->end()) {
      return apply_resolved_head(it->second, named, ctx);
    }
  }
  if (ctx.use_type_param_stack) {
    if (const auto param = lookup_type_param(name)) {
      return apply_resolved_head(*param, named, ctx);
    }
  }

  // `self` names the implementing type inside impls and traits.
  if (name == "self") {
    return function_.self_type_;
  }

  // Builtins and prelude containers.
  if (named.type_args.empty() && is_builtin_scalar_name(name)) {
    return session_.types_.builtin(name);
  }
  if (name == "array") {
    // `array[T, n]` through named-type syntax — the element type in the
    // first slot, the length value in the second.
    const auto *element_node = named.type_args.empty()
                                   ? nullptr
                                   : dynamic_cast<const ast::type_expr *>(
                                         named.type_args.front().value.get());
    const auto element = element_node != nullptr
                             ? resolve_type(*element_node, ctx)
                             : k_unknown_type;
    const auto length =
        named.type_args.size() < 2 || named.type_args[1].value == nullptr
            ? k_unknown_type
            : resolve_length_arg(*named.type_args[1].value, ctx);
    return array_with_length(element, length);
  }
  // A module's own type declaration (or an explicit import) shadows a
  // prelude generic of the same name, exactly as a local value shadows a
  // prelude value: `type box = {...}` makes `box` in type position mean
  // the user's struct, not the builtin `box[T]`.
  const auto shadowed_by_user_type = user_type_shadows_builtin(name, ctx);
  if (!shadowed_by_user_type) {
    if (const auto arity = builtin_generic_arity(name)) {
      // Kind-directed: where a higher-kinded slot is being filled, a bare
      // prelude generic names the *constructor* itself (`monad[option]`,
      // `impl monad for option`), not an instantiation.
      if (named.type_args.empty() && ctx.expected_ctor_arity.has_value()) {
        return check_ctor_against_expected(
            session_.types_.ctor_ref(name, "", nullptr, arity->first), name,
            arity->first, named.span, ctx);
      }
      auto args = resolve_type_args(named.type_args, ctx);
      if (!ctx.quiet && !named.type_args.empty() &&
          (named.type_args.size() < arity->first ||
           named.type_args.size() > arity->second)) {
        error(named.span,
              std::format("type `{}` expects {} type argument{}, found {}",
                          name, arity->first, arity->first == 1 ? "" : "s",
                          named.type_args.size()),
              "wrong number of type arguments");
        return k_error_type;
      }
      return session_.types_.builtin_generic(name, std::move(args));
    }
  }
  if (name == "fn") {
    return k_unknown_type;
  }

  // Current module declarations (across all of the module's files).
  const auto *members = ctx.module != nullptr
                            ? ctx.module
                            : index_.find_module(file_.module_name_);
  if (members != nullptr) {
    if (const auto it = members->types.find(name); it != members->types.end()) {
      return instantiate_user_type(*it->second.decl, members->module_name,
                                   named, ctx);
    }
    if (members->traits.contains(name) || members->concepts.contains(name)) {
      return k_unknown_type; // trait/concept used in a bound position
    }
  }

  // Imported names.
  if (const auto *binding = find_import(name)) {
    if (const auto *source = import_source_module(*binding)) {
      const auto member = imported_member_name(*binding);
      if (const auto it = source->types.find(member);
          it != source->types.end()) {
        return instantiate_user_type(*it->second.decl, source->module_name,
                                     named, ctx);
      }
    }
    return k_unknown_type; // external or non-type import; trust it
  }

  // Names re-exported by a session-owned wildcard import (`use a.b.*`).
  for (const auto *source : wildcard_import_sources()) {
    if (const auto it = source->types.find(std::string(name));
        it != source->types.end()) {
      return instantiate_user_type(*it->second.decl, source->module_name, named,
                                   ctx);
    }
    if (source->traits.contains(std::string(name)) ||
        source->concepts.contains(std::string(name))) {
      return k_unknown_type; // trait/concept used in a bound position
    }
  }

  // Prelude traits and concepts used in bound positions (`T: show`,
  // `T: numeric`).
  if (find_prelude_trait(name).has_value() ||
      find_prelude_concept(name).has_value() || name == "send" ||
      name == "share" || name == "pool" || name == "copy") {
    return k_unknown_type;
  }

  // A `type` the prelude re-exports — `ordering`, which used to be a
  // builtin scalar name and so needed no lookup at all. Tried after the
  // module's own types and its imports, so a local declaration still wins.
  if (const auto found = find_prelude_type(name)) {
    return instantiate_user_type(*found->first, found->second, named, ctx);
  }

  if (ctx.quiet || file_.file_has_external_wildcard_) {
    return k_unknown_type;
  }

  emit_undefined_type(named, name);
  return k_error_type;
}

/// Whether the current module declares (or explicitly imports) a type
/// named `name`, which then takes precedence over a prelude builtin
/// generic of the same spelling.
auto checker::user_type_shadows_builtin(std::string_view name,
                                        const resolve_ctx &ctx) -> bool {
  const auto *members = ctx.module != nullptr
                            ? ctx.module
                            : index_.find_module(file_.module_name_);
  if (members != nullptr && members->types.contains(std::string(name))) {
    return true;
  }
  return find_import(name) != nullptr;
}

/// Resolves a bare `mut` prefix in type position (`mut slice[T]`,
/// `mut cell[T]`) to the `_mut` builtin-generic counterpart of the view
/// type it prefixes. `mut` is only meaningful immediately before `slice[T]`
/// or `cell[T]` — every other inner type (`mut int32`, `mut list[T]`, a
/// user struct) has no defined meaning, so it is a diagnostic here rather
/// than a silent pass-through.
auto checker::resolve_mut_type(const ast::mut_type &mt, const resolve_ctx &ctx)
    -> type_id {
  const auto *named =
      mt.inner != nullptr
          ? dynamic_cast<const ast::named_type *>(mt.inner.get())
          : nullptr;
  if (named != nullptr && named->path.size() == 1 &&
      (named->path.front() == "slice" || named->path.front() == "cell")) {
    const auto mut_name = named->path.front() + "_mut";
    const auto arity = builtin_generic_arity(mut_name);
    auto args = resolve_type_args(named->type_args, ctx);
    if (!ctx.quiet && arity.has_value() && !named->type_args.empty() &&
        (named->type_args.size() < arity->first ||
         named->type_args.size() > arity->second)) {
      error(named->span,
            std::format("type `{}` expects {} type argument{}, found {}",
                        mut_name, arity->first, arity->first == 1 ? "" : "s",
                        named->type_args.size()),
            "wrong number of type arguments");
      return k_error_type;
    }
    return session_.types_.builtin_generic(mut_name, std::move(args));
  }
  if (ctx.quiet) {
    return k_error_type;
  }
  error_with_help(
      mt.span, "`mut` is not valid here in type position", "unexpected `mut`",
      "`mut` may only appear immediately before `slice[T]` or `cell[T]` — "
      "e.g. `mut slice[T]`, `mut cell[T]`.");
  return k_error_type;
}

/// The type a value *acts* as once every compile-time-only wrapper is
/// peeled away: the target behind a reference, and the base behind a
/// refinement. What an operator sees, in other words. The one thing that
/// must never see through a refinement is a narrowing coercion, which is
/// the obligation itself.
auto checker::base_shape(type_id id) -> type_id {
  return session_.types_.strip_refinement(
      strip_refs(session_.types_.strip_refinement(id)));
}

/// Follows ref types to the value type they lend.
auto checker::strip_refs(type_id id) -> type_id {
  const auto *entry = &session_.types_.entry(id);
  while (entry->kind == type_kind::ref_kind) {
    id = entry->result;
    entry = &session_.types_.entry(id);
  }
  return id;
}

/// Builds the `comptime::value::type_value` bindings for `decl`'s own
/// type parameters, so a `static if`/`static assert` in `decl`'s body, or
/// a nested comptime-only call reached from it, can reflect on them via
/// `T.name()`/`T.kind()`. Shared by `check_function` (binding a generic
/// function's own type parameters before checking its body) and
/// `try_fold_comptime_only_call` (rebinding a caller's type parameters
/// into a nested call's evaluator locals) — both used to build this by
/// hand with a stray `strip_refs(it->second)` that silently discarded a
/// bound view (`is_view[T]()` could never observe `T.name() == "ref"`
/// through either path). Binds the argument type as-is instead: a bare
/// generic parameter inferred from a `&U` argument at the call site
/// (`is_view`'s own doc comment in std.traits.category) needs
/// `entry.name == "ref"` to survive, not collapse to `U`'s name.
/// `entry.decl` is already null for a `ref_kind` (and every other
/// non-declared) entry regardless, matching `type_value`'s nullable
/// `decl` field, so nothing downstream needs the strip.
auto checker::type_param_comptime_values(
    const std::vector<ast::type_param> &type_params, const type_scope &slots)
    -> std::vector<std::pair<std::string, comptime::value>> {
  auto values = std::vector<std::pair<std::string, comptime::value>>{};
  for (const auto &param : type_params) {
    if (param.is_value_param || param.name.empty()) {
      continue;
    }
    const auto it = slots.find(param.name);
    if (it == slots.end()) {
      continue;
    }
    const auto &entry = session_.types_.entry(it->second);
    values.emplace_back(param.name, comptime::value::make_type_value(
                                        entry.name, entry.decl, it->second));
  }
  return values;
}

// ==========================================================================
//  Typed runtime proofs — `try_from`
//  (`spec/dependent-types-design.md` §7)
//
//  The escape hatch that lets the reasoning fragment stay small without
//  making the language incomplete. Everything the solver cannot prove, a
//  `try_from` can *check*:
//
//      if let @some(i) = index[n].try_from(raw):
//          v[i]        # `i` is an `index[n]` — the unwrap *is* the proof
//
//  No flow analysis is needed to see that: `i` simply has the refined type,
//  because that is what the intrinsic returns.
// ==========================================================================

/// Recognizes `<refinement>.try_from(value)` — either `positive.try_from(x)`
/// or, for a parameterized refinement, `index[n].try_from(x)` — and returns
/// the refinement type it names.
auto checker::try_from_target(const ast::call_expr &call)
    -> std::optional<type_id> {
  if (call.callee == nullptr ||
      call.callee->kind != ast::node_kind::field_expr) {
    return std::nullopt;
  }
  const auto &field = dynamic_cast<const ast::field_expr &>(*call.callee);
  if (field.field_name != "try_from" || field.object == nullptr) {
    return std::nullopt;
  }

  // `positive` — a bare name. `index[n]` — parsed as an *index* expression,
  // since at parse time there is no telling a generic instantiation from a
  // subscript.
  const auto *name_node = field.object.get();
  const ast::index_expr *instantiation = nullptr;
  if (name_node->kind == ast::node_kind::index_expr) {
    instantiation = &dynamic_cast<const ast::index_expr &>(*name_node);
    name_node = instantiation->object.get();
  }
  if (name_node == nullptr || name_node->kind != ast::node_kind::ident_expr) {
    return std::nullopt;
  }
  const auto &name = dynamic_cast<const ast::ident_expr &>(*name_node).name;

  const auto found = find_type_decl_by_name(name);
  if (!found.has_value() || found->first == nullptr ||
      found->first->definition == nullptr ||
      found->first->definition->kind != ast::node_kind::refinement_type) {
    return std::nullopt;
  }

  // Rebuild the instantiation's arguments the same way a type annotation
  // would, so `index[n]` here and `index[n]` in a signature are one type.
  auto args = std::vector<type_id>{};
  if (instantiation != nullptr && instantiation->index != nullptr) {
    const auto &params = found->first->type_params;
    if (!params.empty()) {
      args.push_back(resolve_value_arg(*instantiation->index, params.front(),
                                       current_resolve_ctx(),
                                       instantiation->index->span));
    }
  }
  return make_user_type(*found->first, found->second, std::move(args));
}

/// Types and desugars a `try_from` call.
///
/// Types it as `option[refined]`, and rewrites it into ordinary Cinder that
/// any backend already knows how to lower — no new HIR node, no new opcode,
/// no intrinsic:
///
///     block:
///         let <tmp> = <value>
///         if <predicate with self := tmp>:
///             @some(<tmp>)
///         else:
///             @none
///
/// The binding is what makes this honest: `<value>` is evaluated exactly
/// once, even though it is *mentioned* twice, so a `try_from` of an
/// effectful expression behaves the way the reader expects. The rewrite is
/// recorded in `spliced_fragments`, which `hir::lower_expr` already consults
/// to redirect a node to the syntax it was rewritten to.
auto checker::infer_refinement_try_from(const ast::call_expr &call)
    -> std::optional<type_id> {
  const auto refined = try_from_target(call);
  if (!refined.has_value()) {
    return std::nullopt;
  }
  const auto &entry = session_.types_.entry(*refined);
  const auto base = entry.result;
  const auto result_type =
      session_.types_.builtin_generic("option", {*refined});

  if (call.args.size() != 1 || call.args.front().value == nullptr) {
    error_with_help(
        call.span,
        std::format("`{}.try_from` takes exactly one argument",
                    session_.types_.display(*refined)),
        "wrong number of arguments",
        std::format("Pass the `{}` you want to check, and match on the "
                    "`option[{}]` it returns.",
                    session_.types_.display(base),
                    session_.types_.display(*refined)));
    return result_type;
  }

  const auto &value = *call.args.front().value;
  const auto found = infer_expr(value, base);
  type_mismatch(value.span, base, found, "for this value to be checked");

  if (entry.predicate == nullptr) {
    // The predicate failed to parse or check; its own declaration already
    // said so. Type the call anyway so nothing cascades.
    return result_type;
  }

  // `index[n].try_from(raw)` in a template has no check to build — `n` is
  // a symbol — so each instance builds its own, against its constant.
  if (function_.current_template_ != nullptr &&
      mentions_template_param(*refined)) {
    defer_to_instances(
        call,
        [this, &call, &value,
         refined = *refined](instance_subst &subst) -> void {
          const auto concrete = substitute_type(refined, subst);
          const auto concrete_entry = session_.types_.entry(concrete);
          if (concrete_entry.predicate == nullptr) {
            return;
          }
          const auto &clone = *clone_of(subst, &call);
          if (const auto *fragment = build_try_from_fragment(
                  clone, *clone_of(subst, &value), concrete_entry,
                  concrete_entry.result, concrete)) {
            session_.spliced_fragments_[&clone] = fragment;
          }
        });
    return record_expr_type(call, result_type);
  }
  auto fragment = build_try_from_fragment(call, value, entry, base, *refined);
  if (fragment != nullptr) {
    session_.spliced_fragments_[&call] = fragment;
  }
  return record_expr_type(call, result_type);
}

/// Builds (and type-checks) the rewritten body described by
/// `infer_refinement_try_from`, returning the synthesized `block_expr` — or
/// `nullptr` if the predicate or the value uses syntax the cloner doesn't
/// cover, in which case the call still types correctly and only its lowering
/// is refused, with a diagnostic.
auto checker::build_try_from_fragment(const ast::call_expr &call,
                                      const ast::expr &value,
                                      const type_entry &entry, type_id base,
                                      type_id refined) -> const ast::node * {
  auto predicate = ast::clone_expr(*entry.predicate);
  if (!predicate.has_value()) {
    error_with_help(
        call.span,
        std::format("`{}.try_from` cannot be compiled here: {}",
                    session_.types_.display(refined),
                    predicate.error().message),
        "this proof cannot be lowered",
        "A refinement's predicate has to be an ordinary expression the "
        "compiler can re-emit as the runtime check this proof compiles "
        "into. Simplify the predicate, or check the value with a hand-"
        "written `if` instead.");
    return nullptr;
  }

  const auto parameters = refinement_constants(entry);
  if (!parameters.has_value()) {
    if (mentions_template_param(refined)) {
      // `index[n].try_from(raw)` inside a function generic over `n`. There
      // is no check to build *here* — `n` is a symbol — but there will be
      // one in each instance the template is compiled into, where `n` is a
      // number (`instantiate_generic_function`). The template itself is never
      // lowered, so leaving this call without a fragment costs nothing.
      return nullptr;
    }
    error_with_help(
        call.span,
        std::format("`{}.try_from` needs its parameters to be known here",
                    session_.types_.display(refined)),
        "this refinement is still generic",
        std::format(
            "The check this compiles into compares the value against the "
            "refinement's parameters, so they have to be real values at "
            "this point — `{}.try_from(...)` with a literal, not one "
            "written in terms of a value parameter the caller supplies.",
            session_.types_.display(
                session_.types_.strip_refinement(refined))));
    return nullptr;
  }

  // A name no source file can collide with, since `#` is not an identifier
  // character — this binding is the compiler's, not the user's, and must
  // never shadow something they wrote.
  const auto temp_name = std::format("#proof{}", session_.proof_temporaries_++);
  specialize_predicate(*predicate, temp_name, *parameters);

  auto binding = ast::make<ast::binding_pattern>();
  binding->span = call.span;
  binding->name = temp_name;

  // The value is *not* cloned. A placeholder stands in for it, redirected
  // through `spliced_fragments` to the expression the user actually wrote —
  // which the checker has already typed, node for node. Cloning would mean
  // re-inferring the copy (re-reporting any diagnostic inside it) and would
  // limit `try_from` to whatever `ast::clone_expr` happens to cover. This
  // way the argument may be any expression at all, and it is evaluated
  // exactly once, where the user wrote it.
  auto placeholder = make_ident(temp_name, value.span, /*is_variant=*/false);
  const auto *placeholder_node = placeholder.get();
  session_.spliced_fragments_[placeholder_node] = &value;
  record_expr_type(*placeholder_node, base);

  auto bind = ast::make<ast::let_stmt>();
  bind->span = call.span;
  bind->pattern = std::move(binding);
  bind->initializer = std::move(placeholder);

  auto some_call = make_variant_call("some", temp_name, call.span);
  auto none_value = make_variant_call("none", {}, call.span);
  const auto *some_expr = some_call.get();
  const auto *none_expr = none_value.get();
  const auto *predicate_expr = predicate->get();

  auto branch = ast::if_branch{};
  branch.span = call.span;
  branch.condition = std::move(*predicate);
  branch.body.push_back(wrap_in_stmt(std::move(some_call)));

  auto conditional = ast::make<ast::if_expr>();
  conditional->span = call.span;
  conditional->branches.push_back(std::move(branch));
  conditional->else_body.push_back(wrap_in_stmt(std::move(none_value)));
  const auto *conditional_expr = conditional.get();

  auto block = ast::make<ast::block_expr>();
  block->span = call.span;
  const auto *bound_pattern = bind->pattern.get();
  block->stmts.push_back(std::move(bind));
  block->stmts.push_back(wrap_in_stmt(std::move(conditional)));

  // Type the fragment the way the checker types anything else, so every
  // synthesized node lands in `node_types` and lowering can read it back.
  // The binding is introduced twice, deliberately: as the *base* type while
  // the predicate is checked (the predicate asks a question about a plain
  // `int32`), then as the *refined* type inside the `@some`, where the
  // branch we are in is itself the proof that the predicate holds. That
  // second binding is the entire mechanism — it is why unwrapping the
  // option hands back a value that simply *has* the refined type, with no
  // further obligation and no flow analysis.
  push_scope();
  bind_value(temp_name, base, binding_origin::let_binding, call.span);
  record_expr_type(*bound_pattern, base);
  require_bool(*predicate_expr, "a refinement predicate");
  bind_value(temp_name, refined, binding_origin::let_binding, call.span);
  const auto result_type = session_.types_.builtin_generic("option", {refined});
  infer_expr(*some_expr, result_type);
  infer_expr(*none_expr, result_type);
  record_expr_type(*conditional_expr, result_type);
  record_expr_type(*block, result_type);
  pop_scope();

  const auto *fragment = block.get();
  session_.proof_fragments_.push_back(std::move(block));
  return fragment;
}

/// Rewrites a cloned predicate so it speaks about *this* proof: `self`
/// becomes the local the value was bound to, and each of the refinement's
/// value parameters becomes the constant this instantiation supplied.
///
/// Both substitutions are needed, and for the same reason. The predicate of
/// `index[n]` is `self < n`; the check generated for `index[8].try_from(x)`
/// has to be `#proof0 < 8`. Neither `self` nor `n` exists at runtime — one
/// is the abstract value, the other a compile-time parameter — so both must
/// be gone before the predicate can be compiled as ordinary code.
///
/// Takes the owning pointer, not a reference, because replacing `n` with
/// `8` swaps one node for another rather than editing it in place.
auto checker::specialize_predicate(
    ast::ptr<ast::expr> &slot, const std::string &value_binding,
    const std::unordered_map<std::string, int64_t> &parameters) -> void {
  if (slot == nullptr) {
    return;
  }
  switch (slot->kind) {
  case ast::node_kind::ident_expr: {
    auto &ident = dynamic_cast<ast::ident_expr &>(*slot);
    if (ident.name == "self") {
      ident.name = value_binding;
      return;
    }
    if (const auto it = parameters.find(ident.name); it != parameters.end()) {
      auto literal = ast::make<ast::literal_expr>();
      literal->span = ident.span;
      literal->lit_kind = token_kind::int_lit;
      literal->value = std::to_string(it->second);
      slot = std::move(literal);
    }
    return;
  }
  case ast::node_kind::binary_expr: {
    auto &binary = dynamic_cast<ast::binary_expr &>(*slot);
    specialize_predicate(binary.lhs, value_binding, parameters);
    specialize_predicate(binary.rhs, value_binding, parameters);
    return;
  }
  case ast::node_kind::unary_expr: {
    auto &unary = dynamic_cast<ast::unary_expr &>(*slot);
    specialize_predicate(unary.operand, value_binding, parameters);
    return;
  }
  case ast::node_kind::field_expr: {
    auto &field = dynamic_cast<ast::field_expr &>(*slot);
    specialize_predicate(field.object, value_binding, parameters);
    return;
  }
  case ast::node_kind::module_path_expr: {
    // `self.value` is a path, not a field access — see `atom_key`.
    auto &path = dynamic_cast<ast::module_path_expr &>(*slot);
    if (!path.segments.empty() && path.segments.front() == "self") {
      path.segments.front() = value_binding;
    }
    return;
  }
  case ast::node_kind::index_expr: {
    auto &index = dynamic_cast<ast::index_expr &>(*slot);
    specialize_predicate(index.object, value_binding, parameters);
    specialize_predicate(index.index, value_binding, parameters);
    return;
  }
  case ast::node_kind::call_expr: {
    auto &call = dynamic_cast<ast::call_expr &>(*slot);
    specialize_predicate(call.callee, value_binding, parameters);
    for (auto &arg : call.args) {
      specialize_predicate(arg.value, value_binding, parameters);
    }
    return;
  }
  default:
    return;
  }
}

/// The constant each of a refinement's value parameters was instantiated
/// with at this use. `nullopt` when one of them is still open (an `index[n]`
/// inside a function generic over `n`) — which means no runtime check can be
/// generated *here*, because the value it would compare against does not
/// exist until the enclosing template is monomorphized. The instances
/// `instantiate_generic_function` makes of it are where the check does get
/// built, with `n` a real number.
auto checker::refinement_constants(const type_entry &entry)
    -> std::optional<std::unordered_map<std::string, int64_t>> {
  auto constants = std::unordered_map<std::string, int64_t>{};
  if (entry.decl == nullptr) {
    return constants;
  }
  for (size_t i = 0; i < entry.decl->type_params.size(); ++i) {
    const auto &param = entry.decl->type_params[i];
    if (!param.is_value_param || i >= entry.args.size()) {
      continue;
    }
    const auto &slot = session_.types_.entry(entry.args[i]);
    if (slot.kind != type_kind::const_value_kind) {
      return std::nullopt;
    }
    constants.emplace(param.name, slot.value.constant);
  }
  return constants;
}

/// A synthesized identifier.
///
/// The span is not decoration. `is_variant_ident` tells `@some` from `some`
/// by the span being one byte *wider* than the name — the `@` — since the
/// parser lowers both to a plain `ident_expr`. A synthesized name therefore
/// has to carry a span of exactly its own width, or the checker reads it as
/// a variant and hunts for a sum type that declares it. `is_variant` sizes
/// the span accordingly, which is how `@some` and `#proof0` are built by the
/// same function without one being mistaken for the other.
auto checker::make_ident(std::string_view name, source_span span,
                         bool is_variant) -> ast::ptr<ast::ident_expr> {
  auto ident = ast::make<ast::ident_expr>();
  ident->name = std::string(name);
  ident->span = source_span{
      .start = span.start,
      .end = span.start + static_cast<uint32_t>(name.size()) +
             (is_variant ? 1U : 0U),
  };
  return ident;
}

/// `@some(<binding>)`, or a bare `@none` when `binding` is empty.
auto checker::make_variant_call(std::string_view variant,
                                std::string_view binding, source_span span)
    -> ast::ptr<ast::expr> {
  auto name = make_ident(variant, span, /*is_variant=*/true);
  if (binding.empty()) {
    return name;
  }

  auto argument = make_ident(binding, span, /*is_variant=*/false);
  const auto argument_span = argument->span;

  auto call = ast::make<ast::call_expr>();
  call->span = span;
  call->callee = std::move(name);
  call->args.push_back(ast::call_arg{.span = argument_span,
                                     .name = std::nullopt,
                                     .value = std::move(argument)});
  return call;
}

/// Wraps an expression as a statement, the shape every block body wants.
auto checker::wrap_in_stmt(ast::ptr<ast::expr> value) -> ast::ptr<ast::node> {
  auto stmt = ast::make<ast::expr_stmt>();
  stmt->span = value->span;
  stmt->expr = std::move(value);
  return stmt;
}

/// Types an indexing expression `object[key]`. Distinguishes generic
/// instantiation (`size_of[usize]`) from real indexing via
/// `ident_names_callable_decl`, then, for `array`/`list`/`slice`/`str`,
/// requires an integer key (except for a range key, which produces a
/// slice/substring rather than a single element).
/// Resolves `v[i]` on a user struct/sum/opaque receiver against its
/// `std.traits.index` impl, recording the dispatch so `hir::lower_index`
/// emits a call to `at` instead of the direct element addressing every
/// builtin container uses.
///
/// This is what makes indexing an ordinary trait rather than a compiler
/// privilege reserved for `list`/`slice`/`str`/`array`.
/// The builtin receivers do not
/// come through here at all — they keep their direct addressing, so this
/// adds a capability without changing any existing lowering.
/// The element type an index impl yields for *this* receiver.
///
/// `session_.impl_assoc_types_` is keyed by the impl's target type as written,
/// so a generic `impl[T] index[usize] for holder[T]` files its `output = T`
/// under `holder[T]` — with `T` still abstract — and a lookup against the
/// concrete `holder[int32]` misses it entirely. Falling back to the
/// pattern's own binding and substituting the receiver's arguments into it
/// is what makes a generic collection indexable at all; without this the
/// checker types `h[0]` as unknown and lowering fails with "no concrete
/// checked type is available for this node" on correct code.
auto checker::resolve_index_output(type_id target, std::string_view trait_name,
                                   const method_entry &method) -> type_id {
  const auto direct = resolve_operator_return_type(target, trait_name, method);
  // `mentions_unknown`, not `is_unknown`: a generic impl whose `at`
  // returns `slice[T]` resolves here to `slice[_]` when `T` has no
  // binding yet — resolved-looking at the top and empty inside. Reading
  // that as final is what typed `xs[a..b]` as `slice[_]` and then failed
  // to lower for want of an element type.
  if (!mentions_unknown(direct) && !mentions_type_param(direct)) {
    return direct;
  }
  // Resolve the declaration's own return type with the impl block's type
  // parameters in scope (so a literally-written `-> T` resolves to the
  // parameter rather than to `unknown`), then apply the bindings the
  // receiver pins — the same two-step `check_impl_generic_method_call`
  // uses for this method's *parameter* types.
  if (method.block_type_params != nullptr) {
    auto bindings = param_subst{};
    match_params(method.impl_target_pattern, target, bindings);
    const auto declared = signature_return_type(*method.decl, method.owner,
                                                method.block_type_params);
    const auto substituted = substitute_solved(declared, bindings);
    // A leaf is an answer here, not a gap: `[1, 2][0]` is `?a`, the same
    // variable as the list's elements, so a later `let y: int64 = ...`
    // pins both. `is_unknown` counts a variable as unknown, and reading it
    // that way typed the element as `unknown` and cut it off from its list.
    if (!session_.types_.is_unknown(substituted) ||
        session_.types_.entry(substituted).kind == type_kind::type_var_kind) {
      return substituted;
    }
  }
  return direct;
}

/// The declared type of an index method's *key* parameter (`at`/`at_mut`'s
/// `i`, `set_at`'s `i`), resolved for this concrete receiver — the same
/// two-step the value parameter needs in `require_index_set_trait`, and
/// for the same reason: a generic `impl[T] index[usize] for holder[T]`
/// files its parameter types against the still-abstract `holder[T]`.
auto checker::resolve_index_key_type(type_id target,
                                     std::string_view trait_name,
                                     const method_entry &method) -> type_id {
  const auto params = resolve_impl_param_types(target, trait_name, method);
  if (params.size() < 2) {
    return k_unknown_type;
  }
  const auto declared = params[1].type;
  if (!mentions_type_param(declared)) {
    return declared;
  }
  auto bindings = param_subst{};
  match_params(method.impl_target_pattern, target, bindings);
  const auto pattern_params =
      resolve_impl_param_types(method.impl_target_pattern, trait_name, method);
  return pattern_params.size() >= 2
             ? substitute_solved(pattern_params[1].type, bindings)
             : declared;
}

/// Checks the key in `v[k]` against the index impl's declared key type.
///
/// Method lookup finds `at`/`at_mut`/`set_at` by *name*, so without this
/// every key type reaches the one impl a type happens to have and is
/// typed as if it fit. That is how `xs[0..n]` on a type implementing only
/// `index[usize]` came back as a single element instead of a sub-view,
/// silently, with the resulting mismatch surfacing a frame later at
/// whatever the value was passed to. Reported here, at the index itself,
/// where the reader can see which impl was consulted and what it takes.
/// The type already inferred for `index`'s key expression, for the second
/// look `&mut v[k]` takes at an index expression whose key `infer_index`
/// has already typed — re-inferring it there would double-report any
/// diagnostic inside the key itself.
auto checker::recorded_key_type(const ast::index_expr &index) -> type_id {
  if (index.index == nullptr) {
    return k_unknown_type;
  }
  const auto it = session_.node_types_.find(index.index.get());
  return it == session_.node_types_.end() ? k_unknown_type
                                          : strip_refs(it->second);
}

/// What keeps two index impls' monomorphized methods apart. A receiver
/// with both `index[usize]` and `index[range[usize]]` contributes two
/// `at`s, and `instantiate_impl_method_for` names an instance after the
/// receiver alone — so without this both mangle to
/// `list::at$list_int32_`, one definition wins, and every caller of the
/// other gets a function whose signature is not the one it is calling.
/// Empty for a key the impl leaves generic, where there is nothing to
/// tell apart.
auto checker::index_instance_discriminator(type_id target,
                                           std::string_view trait_name,
                                           const method_entry &method)
    -> std::string {
  const auto key = resolve_index_key_type(target, trait_name, method);
  if (session_.types_.is_unknown(key) || mentions_type_param(key)) {
    return {};
  }
  return std::format("${}", mangle_type_for_instance(key));
}

/// A `find_method` filter selecting the index method whose declared key
/// parameter accepts `key` — the impl `v[k]` actually means when a type
/// carries more than one (`index[usize]` *and* `index[range[usize]]`).
auto checker::index_key_filter(type_id target, std::string_view trait_name,
                               type_id key) -> method_filter {
  return [this, target, trait_name, key](const method_entry &method) -> bool {
    if (method.decl->params.size() < 2) {
      return false;
    }
    const auto declared =
        strip_refs(resolve_index_key_type(target, trait_name, method));
    // An unresolved or still-abstract key type is no evidence either way;
    // let the one candidate through rather than rejecting every impl and
    // reporting a type as unindexable when it plainly is not.
    return session_.types_.is_unknown(declared) ||
           mentions_type_param(declared) ||
           session_.types_.compatible(declared, strip_refs(key));
  };
}

/// Returns whether the key fits; a mismatch is reported here and makes the
/// whole index expression an error, so nothing downstream sees a type
/// invented from the wrong impl.
auto checker::check_index_key(const ast::index_expr &index, type_id target,
                              std::string_view trait_name,
                              const method_entry &method, type_id key) -> bool {
  const auto expected =
      strip_refs(resolve_index_key_type(target, trait_name, method));
  const auto found = strip_refs(key);
  if (session_.types_.is_unknown(expected) ||
      session_.types_.is_unknown(found) || expected == k_error_type ||
      found == k_error_type || mentions_type_param(expected) ||
      session_.types_.compatible(expected, found)) {
    return true;
  }
  const auto key_span = index.index != nullptr ? index.index->span : index.span;
  const auto key_is_range = is_range_type(found);
  error_with_help(
      key_span,
      std::format("cannot index `{}` with `{}`",
                  session_.types_.display(target),
                  session_.types_.display(found)),
      std::format("expected an index of type `{}`",
                  session_.types_.display(expected)),
      key_is_range
          ? std::format(
                "`{}` implements `{}[{}]`, which indexes one element at a "
                "time. Indexing by a range is a separate impl with its own "
                "`output` — a sub-view rather than an element:\n\n    impl "
                "{}[range[{}]] for {}:\n        type output = ...\n",
                session_.types_.display(target), trait_name,
                session_.types_.display(expected), trait_name,
                session_.types_.display(expected),
                session_.types_.display(target))
          : std::format(
                "`{}` implements `{}[{}]`, so `[...]` takes a `{}`. Indexing "
                "it by another type means adding an `{}[{}]` impl of its "
                "own.",
                session_.types_.display(target), trait_name,
                session_.types_.display(expected),
                session_.types_.display(expected), trait_name,
                session_.types_.display(found)));
  return false;
}

/// Records which `at`/`at_ref`/`at_mut`/`set_at` an index expression
/// calls: the instance for `target`, or — on a receiver written in a
/// template's parameters — the template's method now and each instance's
/// own once it is made.
auto checker::dispatch_index(
    std::unordered_map<const ast::index_expr *, resolved_callee> &dispatches,
    const ast::index_expr &index, const method_entry &method, type_id target,
    std::string_view trait) -> void {
  const auto entry =
      session_.types_.entry(target); // copy: instantiating can intern
  const auto *callee = instantiate_impl_method_for(
      index, method, entry, target,
      index_instance_discriminator(target, trait, method));
  dispatches[&index] =
      resolved_callee{.decl = callee != nullptr ? callee : method.decl,
                      .owner_module = method.owner->module_name,
                      .impl_target_type = callee != nullptr ? "" : entry.name,
                      .receiver = index.object.get()};
  if (callee == nullptr && mentions_template_param(target)) {
    defer_to_instances(
        index,
        [this, &dispatches, &index, &method, target,
         trait = std::string(trait)](instance_subst &subst) -> void {
          dispatch_index(dispatches, *clone_of(subst, &index), method,
                         substitute_type(target, subst), trait);
        });
  }
}

/// `dispatch_index`, once the receiver can name an instance.
///
/// The impl is chosen from the head alone, but its instance is named after
/// the whole receiver: `list[?a]::at` is a function nothing ever compiles.
/// So a receiver whose elements are still open (`[1, 2][0]` before
/// `let y: int64 = ...` has said what they are) records the template now
/// and names the instance once the leaves settle — the same deferral an
/// ordinary method call on an open receiver gets. Shared by all four
/// index traits, so reading, borrowing and writing an element all leave
/// the element type to whatever later statement says it.
auto checker::dispatch_index_when_settled(
    std::unordered_map<const ast::index_expr *, resolved_callee> &dispatches,
    const ast::index_expr &index, const method_entry &method, type_id target,
    std::string_view trait) -> void {
  if (!mentions_type_var(target)) {
    dispatch_index(dispatches, index, method, target, trait);
    return;
  }
  dispatches[&index] =
      resolved_callee{.decl = method.decl,
                      .owner_module = method.owner->module_name,
                      .impl_target_type = session_.types_.entry(target).name,
                      .receiver = index.object.get()};
  defer_method_call(method.decl->name, target, std::vector<type_id>{},
                    [this, &dispatches, &index, &method,
                     trait = std::string(trait)](type_id receiver) -> void {
                      dispatch_index(dispatches, index, method, receiver,
                                     trait);
                    });
}

auto checker::require_index_trait(const ast::index_expr &index, type_id object,
                                  const type_entry &entry, type_id key)
    -> type_id {
  const auto target = strip_refs(object);
  if (!type_has_trait(entry, "index")) {
    error_with_help(
        index.span,
        std::format("`{}` cannot be indexed with `[...]`",
                    session_.types_.display(target)),
        std::format("this is a `{}`, which has no `index` impl",
                    session_.types_.display(target)),
        std::format("Indexing is a trait, not a builtin. Add an impl that says "
                    "what `{}[...]` means:\n\n    impl index[usize] for {}:\n"
                    "        type output = ...\n"
                    "        def at(self, i: usize) -> ...:\n            ...",
                    session_.types_.display(target), entry.name));
    return k_error_type;
  }
  // The impl that takes *this* key, falling back to any `at` at all so a
  // key no impl accepts is reported against a real signature rather than
  // read as "not indexable".
  const auto *method =
      find_method(entry, "at", target, index_key_filter(target, "index", key));
  const auto *any =
      method != nullptr ? method : find_method(entry, "at", target);
  if (any == nullptr || any->decl->params.empty() ||
      param_name_of(any->decl->params.front()) != "self") {
    // `index` is implemented but `at` is not resolvable for this receiver
    // — already diagnosed by impl checking, so stay quiet rather than
    // reporting the same missing method twice in different words.
    return k_unknown_type;
  }
  if (method == nullptr) {
    check_index_key(index, target, "index", *any, key);
    return k_error_type;
  }
  dispatch_index_when_settled(session_.index_dispatches_, index, *method,
                              target, "index");
  // A read of an element that is not `copy` cannot take it out of the
  // container (ch. 14, Moving out of places): it reads the element in
  // place, through `at_ref`'s address, so a borrow or field read of it
  // does not see a bitwise copy. Lowering picks it when the element type
  // is not `copy`.
  if (type_has_trait(entry, "index_ref")) {
    if (const auto *by_ref =
            find_method(entry, "at_ref", target,
                        index_key_filter(target, "index_ref", key));
        by_ref != nullptr && !by_ref->decl->params.empty() &&
        param_name_of(by_ref->decl->params.front()) == "self") {
      dispatch_index_when_settled(session_.index_ref_dispatches_, index,
                                  *by_ref, target, "index_ref");
    }
  }
  // Resolved against *this* impl's bindings: a type with both
  // `index[usize]` and `index[range[usize]]` has two `output`s, and the
  // bare trait name names whichever was checked last.
  return resolve_index_output(
      target,
      parameterized_trait_key("index",
                              resolve_index_key_type(target, "index", *method)),
      *method);
}

/// The read-borrow half: `&v[i]` on a user receiver, against `index_ref`.
/// Called from `infer_unary`'s `addr_of` case rather than from
/// `infer_index` itself, since only the enclosing `&` decides whether a
/// read (`at`) or a read-borrow (`at_ref`) is meant — the two share syntax
/// up to that point (mirrors `require_index_mut_trait`'s relationship to
/// `addr_of_mut`).
///
/// Unlike `index_mut`'s `output_mut`, `index_ref`'s `at_ref` has no
/// associated type of its own to resolve here: its return type is written
/// directly against `index`'s own `output`, but `self.output` inside
/// `index_ref`'s trait declaration resolves to `unknown` (no code path
/// threads an assoc type through a `requires` bound) — harmlessly, since
/// every real impl states its own concrete `cell[T]` return type in the
/// signature `find_method` hands back, exactly as `require_index_trait`'s
/// `at` does. So this reuses `resolve_index_output`, not a `index_ref`
/// counterpart of `resolve_index_mut_output`.
///
/// Returns `k_unknown_type` when the receiver has no `index_ref` impl at
/// all, so the caller can fall back to its own diagnostic (mirroring the
/// missing-method hush in `require_index_trait`).
auto checker::require_index_ref_trait(const ast::index_expr &index,
                                      type_id object, const type_entry &entry,
                                      type_id key) -> type_id {
  const auto target = strip_refs(object);
  if (!type_has_trait(entry, "index_ref")) {
    return k_unknown_type;
  }
  const auto *method = find_method(entry, "at_ref", target,
                                   index_key_filter(target, "index_ref", key));
  const auto *any =
      method != nullptr ? method : find_method(entry, "at_ref", target);
  if (any == nullptr || any->decl->params.empty() ||
      param_name_of(any->decl->params.front()) != "self") {
    return k_unknown_type;
  }
  if (method == nullptr) {
    check_index_key(index, target, "index_ref", *any, key);
    return k_error_type;
  }
  dispatch_index_when_settled(session_.index_ref_dispatches_, index, *method,
                              target, "index_ref");
  return resolve_index_output(
      target,
      parameterized_trait_key(
          "index_ref", resolve_index_key_type(target, "index_ref", *method)),
      *method);
}

/// `at_mut`'s return type, `-> self.output_mut`. `index_mut` declares its
/// own associated type rather than reusing `index`'s `output`, because a
/// mutable borrow is not uniformly "a cell around what a read yields": a
/// sub-view impl hands back a `slice_mut[T]`, which is already a mutable
/// view and wants no cell around it. So this resolves against
/// `index_mut`'s own bindings for this key, not `index`'s.
auto checker::resolve_index_mut_output(type_id target,
                                       std::string_view trait_key,
                                       const method_entry &method) -> type_id {
  const auto target_it = session_.impl_assoc_types_.find(target);
  if (target_it != session_.impl_assoc_types_.end()) {
    if (const auto trait_it = target_it->second.find(std::string(trait_key));
        trait_it != target_it->second.end()) {
      const auto saved_assoc = function_.self_assoc_types_;
      function_.self_assoc_types_ = trait_it->second;
      const auto result = signature_return_type(*method.decl, method.owner);
      function_.self_assoc_types_ = saved_assoc;
      if (!session_.types_.is_unknown(result) && !mentions_type_param(result)) {
        return result;
      }
    }
  }
  if (method.block_type_params != nullptr) {
    auto bindings = param_subst{};
    match_params(method.impl_target_pattern, target, bindings);
    const auto declared = signature_return_type(*method.decl, method.owner,
                                                method.block_type_params);
    const auto substituted = substitute_solved(declared, bindings);
    if (!session_.types_.is_unknown(substituted)) {
      return substituted;
    }
  }
  return resolve_operator_return_type(target, "index_mut", method);
}

/// The mutable-borrow half: `&mut v[i]` on a user receiver, against
/// `index_mut`. Called from `infer_unary`'s `addr_of_mut` case rather than
/// from `infer_index` itself, since only the enclosing `&mut` decides
/// whether a read (`at`) or a mutable borrow (`at_mut`) is meant — the two
/// share syntax up to that point. Returns `k_unknown_type` when the
/// receiver has no `index_mut` impl at all, so the caller can fall back to
/// its own diagnostic for that case (mirroring the missing-method hush in
/// `require_index_trait`, but the "not indexable at all" case is the
/// caller's to report since it also knows about plain `index`).
auto checker::require_index_mut_trait(const ast::index_expr &index,
                                      type_id object, const type_entry &entry,
                                      type_id key) -> type_id {
  const auto target = strip_refs(object);
  if (!type_has_trait(entry, "index_mut")) {
    return k_unknown_type;
  }
  const auto *method = find_method(entry, "at_mut", target,
                                   index_key_filter(target, "index_mut", key));
  const auto *any =
      method != nullptr ? method : find_method(entry, "at_mut", target);
  if (any == nullptr || any->decl->params.empty() ||
      param_name_of(any->decl->params.front()) != "self") {
    return k_unknown_type;
  }
  if (method == nullptr) {
    check_index_key(index, target, "index_mut", *any, key);
    return k_error_type;
  }
  dispatch_index_when_settled(session_.index_mut_dispatches_, index, *method,
                              target, "index_mut");
  return resolve_index_mut_output(
      target,
      parameterized_trait_key(
          "index_mut", resolve_index_key_type(target, "index_mut", *method)),
      *method);
}

/// The write half: `v[i] = x` on a user receiver, against `index_set`.
///
/// Reported separately from `index` so a type that supports reads but not
/// writes gets told *that*, rather than "not indexable" — which would be
/// false, and would send the user looking at their working `v[i]` reads.
/// `signature_params` for a method looked up from outside its own impl,
/// with that impl's associated-type bindings reinstated — the parameter
/// counterpart of `resolve_operator_return_type`, and needed for the same
/// reason: `set_at`'s `value: self.output` resolves to `unknown` without
/// them.
auto checker::resolve_impl_param_types(type_id target,
                                       std::string_view trait_name,
                                       const method_entry &method)
    -> std::vector<fn_param_info> {
  const auto target_it = session_.impl_assoc_types_.find(target);
  const auto trait_it = target_it == session_.impl_assoc_types_.end()
                            ? decltype(target_it->second.end()){}
                            : target_it->second.find(std::string(trait_name));
  if (target_it == session_.impl_assoc_types_.end() ||
      trait_it == target_it->second.end()) {
    return signature_params(*method.decl, method.owner, false);
  }
  const auto saved_assoc = function_.self_assoc_types_;
  function_.self_assoc_types_ = trait_it->second;
  auto result = signature_params(*method.decl, method.owner, false);
  function_.self_assoc_types_ = saved_assoc;
  return result;
}

auto checker::require_index_set_trait(const ast::index_expr &index,
                                      type_id object, const type_entry &entry,
                                      type_id key) -> type_id {
  const auto target = strip_refs(object);
  if (!type_has_trait(entry, "index_set")) {
    const auto reads = type_has_trait(entry, "index");
    error_with_help(
        index.span,
        std::format("cannot assign to `{}[...]`",
                    session_.types_.display(target)),
        reads ? std::format("`{}` can be read at an index but not written",
                            session_.types_.display(target))
              : std::format("this is a `{}`, which has no `index_set` impl",
                            session_.types_.display(target)),
        std::format(
            "Writing through `[...]` is a separate capability from "
            "reading it. Add:\n\n    impl index_set[usize] for {}:\n"
            "        type output = ...\n"
            "        def set_at(mut self, i: usize, value: ...) -> unit:\n"
            "            ...",
            entry.name));
    return k_error_type;
  }
  const auto *method = find_method(entry, "set_at", target,
                                   index_key_filter(target, "index_set", key));
  const auto *any =
      method != nullptr ? method : find_method(entry, "set_at", target);
  if (any == nullptr || any->decl->params.empty() ||
      param_name_of(any->decl->params.front()) != "self") {
    return k_unknown_type;
  }
  if (method == nullptr) {
    check_index_key(index, target, "index_set", *any, key);
    return k_error_type;
  }
  dispatch_index_when_settled(session_.index_set_dispatches_, index, *method,
                              target, "index_set");
  // The assigned value's expected type is `set_at`'s `value` parameter,
  // which is `self.output` in the trait and only resolves under the
  // impl's associated-type bindings.
  const auto trait_key = parameterized_trait_key(
      "index_set", resolve_index_key_type(target, "index_set", *method));
  auto params = resolve_impl_param_types(target, trait_key, *method);
  if (params.size() < 2) {
    return k_unknown_type;
  }
  // Same generic-impl substitution the read half needs — `value:
  // self.output` is `T` until the receiver's arguments are applied.
  auto bindings = param_subst{};
  match_params(method->impl_target_pattern, target, bindings);
  // A receiver whose elements are still open (`list[?a]`) has no impl
  // bindings of its own to resolve `self.output` under, so it reads as
  // `_`; the pattern's `T`, substituted, is the element leaf itself — the
  // thing the assigned value is about to say.
  const auto declared = params.back().type;
  if (!mentions_type_param(declared) && !session_.types_.is_unknown(declared)) {
    return declared;
  }
  auto pattern_params =
      resolve_impl_param_types(method->impl_target_pattern, trait_key, *method);
  if (pattern_params.size() >= 2 &&
      session_.types_.is_unknown(pattern_params.back().type) &&
      method->block_type_params != nullptr) {
    // `value: T` written directly, which only resolves with the impl
    // block's own parameters in scope — the same two-step
    // `resolve_index_output` takes for `at`'s result.
    pattern_params = signature_params(*method->decl, method->owner, false,
                                      method->block_type_params);
  }
  return pattern_params.size() >= 2
             ? substitute_solved(pattern_params.back().type, bindings)
             : declared;
}

/// The type to infer an index key *against*. `usize` for an ordinary
/// index, `range[usize]` when the key is written as a range — indices are
/// `usize` either way, and saying so on both bounds is what keeps `v[0..n]`
/// from typing as `range[int32]` off its bare left literal.
auto checker::expected_index_key(const ast::index_expr &index) -> type_id {
  const auto usize = session_.types_.builtin("usize");
  if (index.index == nullptr ||
      index.index->kind != ast::node_kind::binary_expr) {
    return usize;
  }
  const auto &binary = dynamic_cast<const ast::binary_expr &>(*index.index);
  return binary.op == ast::binary_op::range ||
                 binary.op == ast::binary_op::range_inclusive
             ? resolve_range_type(usize)
             : usize;
}

auto checker::infer_index(const ast::index_expr &index) -> type_id {
  if (index.object == nullptr) {
    return k_unknown_type;
  }

  // `size_of[usize]` / `index[n]` — generic instantiation, not indexing.
  if (index.object->kind == ast::node_kind::ident_expr &&
      ident_names_callable_decl(
          dynamic_cast<const ast::ident_expr &>(*index.object))) {
    // A bare identifier naming an in-scope generic parameter (e.g. `T` in
    // `size_of[T]()` inside a concept/generic bound) or a real declared
    // type (e.g. `point` in a compile-time generic call like
    // `derive_show[point]()`) is a type argument, not a value reference —
    // skip ordinary name resolution so it doesn't spuriously report
    // "undefined name".
    const auto is_type_argument =
        index.index != nullptr &&
        index.index->kind == ast::node_kind::ident_expr &&
        (lookup_type_param(
             dynamic_cast<const ast::ident_expr &>(*index.index).name)
             .has_value() ||
         find_type_decl_by_name(
             dynamic_cast<const ast::ident_expr &>(*index.index).name)
             .has_value());
    if (index.index != nullptr && !is_type_argument) {
      infer_expr(*index.index, k_unknown_type);
    }
    return k_unknown_type;
  }

  // Settled, like every other dispatch site reads its receiver. `var xs =
  // []` records `xs` as `list[?a]` and a later `xs.push(7)` solves `?a`
  // into the leaf store — not into the recorded `type_id`, which stays
  // `list[?a]`. Without this the `index` impl is chosen against a
  // variable, the program type-checks, and lowering is handed a node with
  // no concrete type (`codegen_stress/093`).
  const auto object =
      demand_shape(base_shape(infer_expr(*index.object, k_unknown_type)));
  const auto &entry = session_.types_.entry(object);
  const auto key =
      index.index != nullptr
          ? strip_refs(infer_expr(*index.index, expected_index_key(index)))
          : k_unknown_type;
  check_index_in_bounds(index, object, key);
  const auto key_is_range = is_range_type(key);

  const auto key_span = index.index != nullptr ? index.index->span : index.span;
  const auto require_integer_key = [&] -> void {
    if (!key_is_range && !session_.types_.is_unknown(key) &&
        !session_.types_.is_integer(key)) {
      error(key_span,
            std::format("index must be an integer type, found `{}`",
                        session_.types_.display(key)),
            "expected an integer index");
    }
  };

  switch (entry.kind) {
  // `p[i]` on a raw `*T`/`*mut T`: the pointer *is* the data block, so
  // this is a bare load at `p + i * size_of[T]()` with no length to check
  // against -- exactly the unchecked access the `machine` layer exists to
  // permit (spec/specification/03-advanced/38-machine-layer.md). It is
  // also the read/write half of pointer arithmetic: `p[0]` is `*p`, and
  // `&p[i]` is `p + i`.
  case type_kind::ptr_kind:
    require_machine_context(index.span, "indexing a raw pointer");
    require_integer_key();
    if (key_is_range) {
      error(index.span, "a raw pointer cannot be range-indexed",
            "no length is known for a raw pointer");
      return k_error_type;
    }
    return entry.result;
  case type_kind::array_kind:
    require_integer_key();
    return key_is_range
               ? session_.types_.builtin_generic("slice", {entry.result})
               : entry.result;
  case type_kind::builtin_generic_kind: {
    if (entry.name == "uninit") {
      // `buf[i]` on a `uninit[T, N]`: bounds-checked against `N` (a
      // compile-time constant, so the check costs nothing at run time),
      // but saying nothing about whether slot `i` holds a valid `T` --
      // that is the guarantee the type deliberately does not make.
      require_integer_key();
      if (key_is_range) {
        error(index.span, "a `uninit[T, N]` buffer cannot be range-indexed",
              "no slot is known to hold a valid value");
        return k_error_type;
      }
      return entry.args.empty() ? k_unknown_type : entry.args[0];
    }
    if (entry.name == "slice" || entry.name == "slice_mut") {
      require_integer_key();
      const auto element = entry.args.empty() ? k_unknown_type : entry.args[0];
      // A range-sliced `slice_mut` stays a `slice_mut` (a sub-view of a
      // mutable slice is still mutable) — `slice` range-slices to a plain
      // (immutable) `slice`, same as before.
      if (key_is_range) {
        return session_.types_.builtin_generic(
            entry.name == "slice_mut" ? "slice_mut" : "slice", {element});
      }
      return element;
    }
    return k_unknown_type;
  }
  case type_kind::builtin_kind: {
    if (entry.name == "str") {
      require_integer_key();
      // A range slice of a `str` is a `str`; a single scalar index yields
      // the `byte` at that position in the UTF-8 backing bytes (mirroring
      // `slice[byte]`, which the `builtin_generic_kind` arm below already
      // element-indexes to `byte`).
      return key_is_range ? object : session_.types_.builtin("byte");
    }
    if (session_.types_.is_numeric(object) ||
        session_.types_.is_boolean(object)) {
      error(index.span,
            std::format("cannot index a value of type `{}`",
                        session_.types_.display(object)),
            "not an indexable value");
      return k_error_type;
    }
    return k_unknown_type;
  }
  // A user type is indexable exactly when it says so, by implementing
  // `std.traits.index` — see `require_index_trait`.
  case type_kind::struct_kind:
  case type_kind::sum_kind:
  case type_kind::opaque_kind:
    return require_index_trait(index, object, entry, key);
  default:
    return k_unknown_type;
  }
}

/// `trait[args]` as a reader would write it.
auto checker::display_fact(const type_fact &fact) -> std::string {
  auto out = fact.trait->name;
  if (!fact.args.empty()) {
    out += '[';
    for (size_t i = 0; i < fact.args.size(); ++i) {
      if (i != 0) {
        out += ", ";
      }
      out += session_.types_.display(fact.args[i]);
    }
    out += ']';
  }
  return out;
}

} // namespace cinder::semantic::detail
