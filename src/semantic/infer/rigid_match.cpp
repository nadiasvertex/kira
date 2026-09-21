#include "src/semantic/infer/rigid_match.h"

#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "src/semantic/infer/infer_ctxt.h"

namespace kira::semantic::infer {

namespace {

/// Every `type_param_kind` id reachable from `id`.
///
/// A generic walk over `args`/`result` rather than a per-kind switch, for the
/// same reason `infer_ctxt::occurs` is one: a kind added later cannot quietly
/// stop being visited.
auto collect_params(const type_table &table, type_id id,
                    std::vector<type_id> &found,
                    std::unordered_set<type_id> &seen) -> void {
  if (!seen.insert(id).second) {
    return;
  }
  const auto &entry = table.entry(id);
  if (entry.kind == type_kind::type_param_kind) {
    found.push_back(id);
  }
  for (const auto arg : entry.args) {
    collect_params(table, arg, found, seen);
  }
  if (entry.result != id) {
    collect_params(table, entry.result, found, seen);
  }
}

} // namespace

auto match_pattern(type_table &table, type_id pattern, type_id concrete,
                   const legacy_compat &compat) -> rigid_match_result {
  // Nothing is rewritten: the pattern's parameters are adopted in place, so
  // this matcher interns nothing at all. That is not a micro-optimization —
  // every rebuilt type would be permanently in the session's one table, and
  // `resolve_drop_plans` and `compute_view_bearing_types` walk every interned
  // type. A matcher running at tens of thousands of sites must leave no trail.
  const auto left = pattern;
  const auto right = concrete;

  auto params = std::vector<type_id>{};
  auto seen = std::unordered_set<type_id>{};
  collect_params(table, left, params, seen);

  // Local, and that is load-bearing: a `type_param` id is interned by name,
  // so the `T` of one signature is the same id as the `T` of another.
  // Adopting one in a store that outlived this call would make two unrelated
  // unknowns the same unknown.
  auto ctx = infer_ctxt{table};
  auto engine = unifier{table, ctx};
  engine.set_legacy_ref_coercion(compat.ref_coercion);

  for (const auto param : params) {
    const auto &entry = table.entry(param);
    // Arity decides constructor-ness (kinds are arities, ch. 37). For an
    // arity-0 parameter `adopt` records the sort as *ambiguous*, because the
    // spelling genuinely does not say whether it is a type or a compile-time
    // value — guessing would reject half of all const generics.
    ctx.adopt(param,
              entry.ctor_arity > 0 ? meta_sort::ctor_sort
                                   : meta_sort::type_sort,
              entry.ctor_arity, entry.name, source_location{});
  }

  auto result = rigid_match_result{};
  if (auto unified = engine.unify(left, right, k_no_cause);
      !unified.has_value()) {
    result.failure = std::move(unified.error());
  }

  for (const auto param : params) {
    const auto solved = ctx.zonk(param);
    if (solved == param) {
      continue;
    }
    result.bindings.emplace(table.entry(param).name, solved);
  }
  return result;
}

} // namespace kira::semantic::infer
