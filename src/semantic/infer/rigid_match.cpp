#include "src/semantic/infer/rigid_match.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "src/semantic/infer/infer_ctxt.h"

namespace cinder::semantic::infer {

namespace {

/// Every `type_param_kind` id reachable from `id`, and every polynomial
/// variable with the scalar type its slot holds — a value parameter is not a
/// `type_param` in a value slot but a variable inside a `symbolic_value`.
///
/// A generic walk over `args`/`result` rather than a per-kind switch, for the
/// same reason `infer_ctxt::occurs` is one: a kind added later cannot quietly
/// stop being visited.
auto collect_params(const type_table &table, type_id id,
                    std::vector<type_id> &found,
                    std::unordered_map<std::string, type_id> &values,
                    std::unordered_set<type_id> &seen) -> void {
  if (!seen.insert(id).second) {
    return;
  }
  const auto &entry = table.entry(id);
  if (entry.kind == type_kind::type_param_kind) {
    found.push_back(id);
  }
  if (entry.kind == type_kind::symbolic_value_kind) {
    for (const auto &term : entry.value.terms) {
      values.emplace(term.var, entry.result);
    }
  }
  for (const auto arg : entry.args) {
    collect_params(table, arg, found, values, seen);
  }
  if (entry.result != id) {
    collect_params(table, entry.result, found, values, seen);
  }
}

/// The name a pattern's value parameter is solved under when it has to be
/// told apart from the concrete side's variables. See `match_pattern`.
constexpr auto k_renamed_mark = '?';

} // namespace

auto coerce_at_call_site(const type_table &table, type_id expected,
                         type_id found) -> coerced_pair {
  const auto expected_ref = table.entry(expected).kind == type_kind::ref_kind;
  const auto found_ref = table.entry(found).kind == type_kind::ref_kind;
  if (expected_ref == found_ref) {
    return {.expected = expected, .found = found, .adjusted = false};
  }
  if (expected_ref) {
    // `&T` parameter, value argument: the call site borrows.
    return {.expected = table.entry(expected).result,
            .found = found,
            .adjusted = true};
  }
  // A *type parameter* is not a by-value parameter — it is willing to be a
  // reference, and binding it to the referent throws that away. `describe_
  // view[T](x: T)` called with `&n` must infer `T = &int32`, or the nested
  // `is_view[T]()` answers about the referent instead of the argument.
  if (table.entry(expected).kind == type_kind::type_param_kind) {
    return {.expected = expected, .found = found, .adjusted = false};
  }
  // By-value parameter, reference argument: the call site dereferences.
  return {.expected = expected,
          .found = table.entry(found).result,
          .adjusted = true};
}

auto match_pattern(type_table &table, type_id pattern, type_id concrete,
                   const std::unordered_map<std::string, linear_poly> &known)
    -> rigid_match_result {
  const auto coerced = coerce_at_call_site(table, pattern, concrete);
  // Nothing is rewritten in the common case: the pattern's parameters are
  // adopted in place, so this matcher interns nothing. That is not a
  // micro-optimization — every rebuilt type would be permanently in the
  // session's one table, and `resolve_drop_plans` and
  // `compute_borrow_bearing_types` walk every interned type. A matcher running
  // at tens of thousands of sites must leave no trail. (The one exception,
  // renaming value parameters apart, is below.)
  const auto left = coerced.expected;
  const auto right = coerced.found;

  auto params = std::vector<type_id>{};
  auto values = std::unordered_map<std::string, type_id>{};
  auto seen = std::unordered_set<type_id>{};
  collect_params(table, left, params, values, seen);

  // The pattern's value variables are solved under their own names, unless
  // that would be ambiguous. Two things make it so: a variable already
  // `known` (it must be replaced by its value, not solved again), and a
  // recursive call, where the concrete side mentions the *same* declaration's
  // `n` — the caller's, fixed — and the pattern's `n` must still be a
  // separate unknown. Only then is the pattern rewritten, every variable
  // either to its known value or to a marked name, so the common case
  // interns nothing.
  auto concrete_params = std::vector<type_id>{};
  auto concrete_values = std::unordered_map<std::string, type_id>{};
  auto concrete_seen = std::unordered_set<type_id>{};
  collect_params(table, right, concrete_params, concrete_values,
                 concrete_seen);
  const auto must_rename =
      std::ranges::any_of(values, [&](const auto &value) -> bool {
        return known.contains(value.first) ||
               concrete_values.contains(value.first);
      });
  auto unknown_name = [&](const std::string &var) -> std::string {
    return must_rename ? k_renamed_mark + var : var;
  };
  auto matched = left;
  if (must_rename) {
    auto renamer = infer_ctxt{table};
    for (const auto &[var, underlying] : values) {
      const auto known_value = known.find(var);
      const auto replacement = table.symbolic_value(
          underlying, known_value != known.end()
                          ? known_value->second
                          : poly_variable(unknown_name(var)));
      static_cast<void>(renamer.bind(
          renamer.value_param(var, underlying, source_location{}),
          replacement, k_no_cause));
    }
    matched = renamer.zonk(left);
  }

  // Local, and that is load-bearing: a `type_param` id is interned by name,
  // so the `T` of one signature is the same id as the `T` of another.
  // Adopting one in a store that outlived this call would make two unrelated
  // unknowns the same unknown.
  auto ctx = infer_ctxt{table};
  auto engine = unifier{table, ctx};

  for (const auto param : params) {
    const auto &entry = table.entry(param);
    // Arity decides constructor-ness (kinds are arities, ch. 37). Every
    // arity-0 parameter here is a type: a value parameter reaches a pattern
    // only as a variable inside a polynomial, collected into `values`.
    ctx.adopt(param,
              entry.ctor_arity > 0 ? meta_sort::ctor_sort
                                   : meta_sort::type_sort,
              entry.ctor_arity, entry.name, source_location{});
  }

  // The pattern's value parameters are unknowns too; the concrete side's
  // are the caller's own, fixed for the match, and stay rigid.
  for (const auto &[var, underlying] : values) {
    if (!known.contains(var)) {
      ctx.declare_value_param(unknown_name(var));
    }
  }

  auto result = rigid_match_result{};
  if (auto unified = engine.unify(matched, right, k_no_cause);
      !unified.has_value()) {
    result.failure = std::move(unified.error());
  }

  for (const auto param : params) {
    const auto solved = ctx.zonk(param);
    if (solved == param) {
      continue;
    }
    result.bindings.emplace(param, solved);
  }
  for (const auto &[var, underlying] : values) {
    if (known.contains(var)) {
      continue;
    }
    const auto minted = ctx.value_param_named(unknown_name(var));
    if (!minted.has_value()) {
      continue; // Declared, but nothing solved it.
    }
    const auto solved = ctx.zonk(*minted);
    const auto &entry = table.entry(solved);
    if (entry.kind != type_kind::const_value_kind &&
        entry.kind != type_kind::symbolic_value_kind) {
      continue;
    }
    result.values.emplace(var, entry.value);
  }
  return result;
}

} // namespace cinder::semantic::infer
