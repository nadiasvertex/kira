#include "src/semantic/infer/unify.h"

#include <format>
#include <utility>

#include "src/semantic/infer/value_solver.h"

namespace kira::semantic::infer {

namespace {

/// Whether `entry` is an application of a nominal constructor — the shape the
/// pattern-fragment rule matches a flex head against.
[[nodiscard]] auto is_nominal_application(const type_entry &entry) -> bool {
  return entry.kind == type_kind::builtin_generic_kind ||
         entry.kind == type_kind::struct_kind ||
         entry.kind == type_kind::sum_kind ||
         entry.kind == type_kind::opaque_kind;
}

/// Whether `entry` is one of the two types that stand for "don't know", which
/// unify with everything by design so a single gap never cascades
/// (`types.h`). Deliberately *not* `type_table::is_unknown`, which also
/// answers true for type parameters and parameter applications — both of
/// which are rigid here and must not silently satisfy every constraint.
[[nodiscard]] auto is_absent(const type_entry &entry) -> bool {
  return entry.kind == type_kind::unknown_kind ||
         entry.kind == type_kind::error_kind;
}

/// Whether a value slot's unknowns range over an unsigned type, so the
/// solver may assume `v >= 0` — the fact that makes `n + 1 ~ 0` refutable.
[[nodiscard]] auto value_is_unsigned(const type_table &table,
                                     const type_entry &slot) -> bool {
  const auto &underlying = table.entry(slot.result);
  return underlying.kind == type_kind::builtin_kind &&
         (underlying.name.starts_with("uint") || underlying.name == "usize" ||
          underlying.name == "byte");
}

/// Whether two nominal types name the same constructor. Declarations compare
/// by identity; prelude generics have none and compare by name.
[[nodiscard]] auto same_constructor(const type_entry &a, const type_entry &b)
    -> bool {
  if (a.decl != nullptr || b.decl != nullptr) {
    return a.decl == b.decl;
  }
  return a.name == b.name && a.module_name == b.module_name;
}

} // namespace

unifier::unifier(type_table &table, infer_ctxt &ctx)
    : table_(&table), ctx_(&ctx) {}

auto unifier::deferred() const -> const std::vector<deferred_constraint> & {
  return deferred_;
}

auto unifier::refuse(unify_failure failure, type_id expected, type_id found,
                     type_id root_expected, type_id root_found, cause_id why,
                     std::string detail) const -> unify_error {
  return unify_error{.failure = failure,
                     .expected = root_expected,
                     .found = root_found,
                     .expected_part = expected,
                     .found_part = found,
                     .why = why,
                     .detail = std::move(detail)};
}

auto unifier::postpone(type_id left, type_id right, cause_id why,
                       std::string waiting_for) -> void {
  deferred_.push_back(
      deferred_constraint{.left = left,
                          .right = right,
                          .why = why,
                          .waiting_for = std::move(waiting_for)});
}

auto unifier::unify(type_id expected, type_id found, cause_id why)
    -> std::expected<void, unify_error> {
  return step(expected, found, why, expected, found);
}

auto unifier::retry_deferred() -> std::expected<bool, unify_error> {
  auto pending = std::vector<deferred_constraint>{};
  pending.swap(deferred_);
  const auto before = pending.size();
  for (const auto &constraint : pending) {
    auto result = unify(constraint.left, constraint.right, constraint.why);
    if (!result.has_value()) {
      return std::unexpected(std::move(result.error()));
    }
  }
  return deferred_.size() < before;
}

/// One step of the recursion.
///
/// Both sides are zonked first, which is what makes the three rules below
/// exhaustive: after zonking, a metavariable is necessarily *unsolved*, and
/// an `F[A]` whose head has already been solved has collapsed into the
/// ordinary application it denotes, so neither needs a case of its own.
auto unifier::step(type_id expected, type_id found, cause_id why,
                   type_id root_expected, type_id root_found)
    -> std::expected<void, unify_error> {
  const auto left = ctx_->zonk(expected);
  const auto right = ctx_->zonk(found);
  if (left == right) {
    return {};
  }

  const auto left_entry = table_->entry(left);   // copies: recursion interns
  const auto right_entry = table_->entry(right); // and the deque rule is easy
                                                 // to lose track of.

  // Absence outranks binding. `unknown` already unifies with everything, so
  // solving `?a := unknown` buys nothing and costs the variable: it is now
  // answered, and no later constraint can teach it anything. Leaving it open
  // is strictly more informative, and it is what the matcher this replaces
  // did by bailing out on an unknown right-hand side.
  //
  // A metavariable's own id is a `type_var_kind`, not `unknown`, so this
  // cannot swallow rule 0 below.
  if (is_absent(left_entry) || is_absent(right_entry)) {
    return {};
  }

  // Rule 0: a variable on either side takes the other side as its solution.
  // Sorts, arities and the occurs check are the store's business.
  auto bind_side = [&](type_id var,
                       type_id value) -> std::expected<void, unify_error> {
    auto bound = ctx_->bind(var, value, why);
    if (bound.has_value()) {
      return {};
    }
    return std::unexpected(refuse(unify_failure::variable, var, value,
                                  root_expected, root_found, why,
                                  std::move(bound.error().detail)));
  };
  if (ctx_->is_meta(left)) {
    return bind_side(left, right);
  }
  if (ctx_->is_meta(right)) {
    return bind_side(right, left);
  }
  // A `type_var_kind` that this store never minted belongs to the old
  // engine; it means exactly as much as `unknown` does here.
  if (left_entry.kind == type_kind::type_var_kind ||
      right_entry.kind == type_kind::type_var_kind) {
    return {};
  }

  // A refinement *is* its base for every shape question; the predicate is a
  // proof obligation, raised elsewhere, never a unification failure. Were
  // this to compare predicates, every narrowing site would report a type
  // mismatch and the solver would never get to prove anything.
  if (left_entry.kind == type_kind::refinement_kind) {
    return step(left_entry.result, right, why, root_expected, root_found);
  }
  if (right_entry.kind == type_kind::refinement_kind) {
    return step(left, right_entry.result, why, root_expected, root_found);
  }

  // Rule 3: value slots.
  if (is_value_kind(left_entry.kind) || is_value_kind(right_entry.kind)) {
    return unify_values(left, right, why, root_expected, root_found);
  }

  // Rule 2: a flex head. After zonking, a `param_app` head is either an
  // unsolved constructor variable (flexible) or a real in-scope parameter
  // (rigid).
  const auto left_flex = left_entry.kind == type_kind::param_app_kind &&
                         ctx_->is_meta(ctx_->zonk(left_entry.result));
  const auto right_flex = right_entry.kind == type_kind::param_app_kind &&
                          ctx_->is_meta(ctx_->zonk(right_entry.result));
  if (left_flex && right_flex) {
    // Two flexible heads: outside the pattern fragment, and no most-general
    // solution exists. Wait rather than guess.
    postpone(left, right, why, "both sides have an unsolved type constructor");
    return {};
  }
  if (left_flex) {
    return unify_flex_app(left, right, true, why, root_expected, root_found);
  }
  if (right_flex) {
    return unify_flex_app(right, left, false, why, root_expected, root_found);
  }
  // A rigid `F[A]` — inside a generic body, where `F` is a real parameter —
  // is deliberately abstract: it stands for whatever the instantiation
  // supplies, so it can neither match nor refute a concrete type yet.
  if (left_entry.kind == type_kind::param_app_kind ||
      right_entry.kind == type_kind::param_app_kind) {
    if (left_entry.kind != right_entry.kind) {
      postpone(left, right, why,
               "an application of a higher-kinded parameter is still abstract");
      return {};
    }
    if (left_entry.result != right_entry.result ||
        left_entry.args.size() != right_entry.args.size()) {
      return std::unexpected(refuse(
          unify_failure::mismatch, left, right, root_expected, root_found, why,
          std::format("`{}` and `{}` apply different type constructors",
                      table_->display(left), table_->display(right))));
    }
    for (size_t i = 0; i < left_entry.args.size(); ++i) {
      auto arg = step(left_entry.args[i], right_entry.args[i], why,
                      root_expected, root_found);
      if (!arg.has_value()) {
        return arg;
      }
    }
    return {};
  }

  // Rule 1: rigid-rigid.
  if (left_entry.kind != right_entry.kind) {
    return std::unexpected(refuse(
        unify_failure::mismatch, left, right, root_expected, root_found, why,
        std::format("expected `{}`, found `{}`", table_->display(left),
                    table_->display(right))));
  }

  auto unify_args = [&]() -> std::expected<void, unify_error> {
    if (left_entry.args.size() != right_entry.args.size()) {
      return std::unexpected(refuse(
          unify_failure::arity, left, right, root_expected, root_found, why,
          std::format("`{}` takes {} argument(s), `{}` takes {}",
                      table_->display(left), left_entry.args.size(),
                      table_->display(right), right_entry.args.size())));
    }
    for (size_t i = 0; i < left_entry.args.size(); ++i) {
      auto arg = step(left_entry.args[i], right_entry.args[i], why,
                      root_expected, root_found);
      if (!arg.has_value()) {
        return arg;
      }
    }
    return {};
  };

  switch (left_entry.kind) {
  case type_kind::builtin_kind:
  case type_kind::type_param_kind:
  case type_kind::ctor_ref_kind:
  case type_kind::existential_kind:
    // Leaves, all interned (or, for an existential, deliberately nominal):
    // equal ids were handled above, so reaching here is a real mismatch.
    return std::unexpected(refuse(
        unify_failure::mismatch, left, right, root_expected, root_found, why,
        std::format("expected `{}`, found `{}`", table_->display(left),
                    table_->display(right))));
  case type_kind::builtin_generic_kind:
  case type_kind::struct_kind:
  case type_kind::sum_kind:
  case type_kind::opaque_kind:
    if (!same_constructor(left_entry, right_entry)) {
      return std::unexpected(refuse(
          unify_failure::mismatch, left, right, root_expected, root_found, why,
          std::format("`{}` and `{}` are different types", left_entry.name,
                      right_entry.name)));
    }
    return unify_args();
  case type_kind::tuple_kind:
    return unify_args();
  case type_kind::fn_kind: {
    auto params = unify_args();
    if (!params.has_value()) {
      return params;
    }
    return step(left_entry.result, right_entry.result, why, root_expected,
                root_found);
  }
  case type_kind::ref_kind:
  case type_kind::ptr_kind: {
    if (left_entry.is_mut != right_entry.is_mut) {
      return std::unexpected(
          refuse(unify_failure::mutability, left, right, root_expected,
                 root_found, why,
                 std::format("`{}` and `{}` differ in mutability",
                             table_->display(left), table_->display(right))));
    }
    return step(left_entry.result, right_entry.result, why, root_expected,
                root_found);
  }
  case type_kind::array_kind: {
    // Both the element type *and* the length. The old `unify_rigid` descended
    // into the element only, which is exactly why an `n` in `array[T, n]`
    // came back looking unsolved and needed a second solver beside it.
    auto element = step(left_entry.result, right_entry.result, why,
                        root_expected, root_found);
    if (!element.has_value()) {
      return element;
    }
    return unify_args();
  }
  default:
    return {};
  }
}

/// Rule 2, the pattern fragment. `app` is `F[A, ...]` with `F` an unsolved
/// constructor variable; `other` is whatever it must equal.
///
/// Solving is by matching the *outermost nominal constructor* and recursing
/// positionally — which is what makes the fragment unitary: there is exactly
/// one way to split `option[int32]` into a head and an argument at a given
/// arity, so there is one most-general solution, no backtracking, and no
/// ambiguity error to report.
auto unifier::unify_flex_app(type_id app, type_id other, bool app_is_expected,
                             cause_id why, type_id root_expected,
                             type_id root_found)
    -> std::expected<void, unify_error> {
  const auto app_entry = table_->entry(app);
  const auto other_entry = table_->entry(other);
  const auto head = ctx_->zonk(app_entry.result);

  auto ordered = [&](type_id from_app, type_id from_other) {
    return app_is_expected ? std::pair{from_app, from_other}
                           : std::pair{from_other, from_app};
  };

  if (!is_nominal_application(other_entry)) {
    // A flex head can only ever equal an application. Against something that
    // is *definitely* not one — `int32`, a tuple, a function — this is a real
    // failure, and ch. 37 is right to call it one.
    const auto [expected_part, found_part] = ordered(app, other);
    return std::unexpected(refuse(
        unify_failure::out_of_scope, expected_part, found_part, root_expected,
        root_found, why,
        std::format("`{}` is not an application of a type constructor, so it "
                    "cannot give `{}` a value",
                    table_->display(other), table_->display(head))));
  }
  if (other_entry.args.size() != app_entry.args.size()) {
    const auto [expected_part, found_part] = ordered(app, other);
    return std::unexpected(refuse(
        unify_failure::arity, expected_part, found_part, root_expected,
        root_found, why,
        std::format("`{}` takes {} type argument(s), but {} were applied",
                    other_entry.name, other_entry.args.size(),
                    app_entry.args.size())));
  }

  const auto ctor = table_->ctor_ref(other_entry.name, other_entry.module_name,
                                     other_entry.decl, other_entry.args.size());
  auto bound = ctx_->bind(head, ctor, why);
  if (!bound.has_value()) {
    const auto [expected_part, found_part] = ordered(head, ctor);
    return std::unexpected(refuse(unify_failure::variable, expected_part,
                                  found_part, root_expected, root_found, why,
                                  std::move(bound.error().detail)));
  }
  for (size_t i = 0; i < app_entry.args.size(); ++i) {
    const auto [expected_arg, found_arg] =
        ordered(app_entry.args[i], other_entry.args[i]);
    auto arg = step(expected_arg, found_arg, why, root_expected, root_found);
    if (!arg.has_value()) {
      return arg;
    }
  }
  return {};
}

/// Rule 3, value slots.
///
/// Phase 2 only *decides* these: equal slots agree, provably-unsolvable ones
/// fail, and anything still open is postponed. Phase 3 replaces the
/// postponement with an actual solution for the unknown.
auto unifier::unify_values(type_id expected, type_id found, cause_id why,
                           type_id root_expected, type_id root_found)
    -> std::expected<void, unify_error> {
  const auto expected_entry = table_->entry(expected);
  const auto found_entry = table_->entry(found);

  // A value parameter that hasn't been given a variable yet is spelled as an
  // arity-0 type parameter; it is rigid and unknown, so wait.
  if (expected_entry.kind == type_kind::type_param_kind ||
      found_entry.kind == type_kind::type_param_kind) {
    postpone(expected, found, why, "a value parameter is not yet known");
    return {};
  }
  if (!is_value_kind(expected_entry.kind) || !is_value_kind(found_entry.kind)) {
    return std::unexpected(refuse(
        unify_failure::mismatch, expected, found, root_expected, root_found,
        why,
        std::format(
            "`{}` is a compile-time value and `{}` is a type",
            table_->display(is_value_kind(expected_entry.kind) ? expected
                                                               : found),
            table_->display(is_value_kind(expected_entry.kind) ? found
                                                               : expected))));
  }

  // A variant value is an identity, not a quantity — which is what makes
  // `send(c: connection[open])` reject a `connection[closed]`. Equal ids were
  // handled by the caller, so two variants here are two different states.
  if (expected_entry.kind == type_kind::const_variant_kind ||
      found_entry.kind == type_kind::const_variant_kind) {
    return std::unexpected(refuse(
        unify_failure::value, expected, found, root_expected, root_found, why,
        std::format("`{}` and `{}` are different states", expected_entry.name,
                    found_entry.name)));
  }

  const auto unsigned_domain = value_is_unsigned(*table_, expected_entry) ||
                               value_is_unsigned(*table_, found_entry);
  const auto solution = solve_value_equation(
      expected_entry.value, found_entry.value, unsigned_domain);
  switch (solution.answer) {
  case value_answer::agreed:
    return {};
  case value_answer::unsatisfiable:
    return std::unexpected(refuse(unify_failure::value, expected, found,
                                  root_expected, root_found, why,
                                  solution.detail));
  case value_answer::underdetermined:
    // `m + n ~ 5` determines neither unknown; another constraint may yet.
    postpone(expected, found, why, "a value equation is not yet determined");
    return {};
  case value_answer::solved:
    break;
  }

  // The unknown is now known. It is recorded in the *same* store as every
  // other solution — a polynomial's name is a lookup key into the one
  // union-find, not a binding map of its own — so zonking any type that
  // mentions it sees the answer with no second substitution pass.
  const auto underlying = expected_entry.result != k_unknown_type
                              ? expected_entry.result
                              : found_entry.result;
  const auto var =
      ctx_->value_param(solution.var, underlying, ctx_->cause_at(why).where);
  const auto value = table_->symbolic_value(underlying, solution.value);
  auto bound = ctx_->bind(var, value, why);
  if (!bound.has_value()) {
    return std::unexpected(refuse(unify_failure::variable, expected, found,
                                  root_expected, root_found, why,
                                  std::move(bound.error().detail)));
  }
  return {};
}

} // namespace kira::semantic::infer
