#include "src/semantic/infer/infer_ctxt.h"

#include <format>
#include <utility>

namespace kira::semantic::infer {

namespace {

/// Which sort a *concrete* type can stand for.
///
/// One case is genuinely ambiguous and is resolved in `check_sort` rather
/// than here: a `type_param_kind` of arity 0 is how the table spells both an
/// ordinary type parameter `T` and a value parameter `n` before either is
/// known, so it is accepted for both `type_sort` and `value_sort`.
[[nodiscard]] auto concrete_sort(const type_entry &entry)
    -> std::optional<meta_sort> {
  if (is_value_kind(entry.kind)) {
    return meta_sort::value_sort;
  }
  if (entry.kind == type_kind::ctor_ref_kind) {
    return meta_sort::ctor_sort;
  }
  if (entry.kind == type_kind::type_param_kind) {
    return entry.ctor_arity > 0 ? std::optional(meta_sort::ctor_sort)
                                : std::nullopt;
  }
  return meta_sort::type_sort;
}

} // namespace

auto sort_name(meta_sort sort) -> std::string_view {
  switch (sort) {
  case meta_sort::type_sort:
    return "type";
  case meta_sort::ctor_sort:
    return "type constructor";
  case meta_sort::value_sort:
    return "compile-time value";
  }
  return "type";
}

infer_ctxt::infer_ctxt(type_table &table) : table_(&table) {
  // Index 0 is `k_no_cause`, so `cause_at` is total.
  causes_.push_back(cause{});
}

/// Every sort mints the same way — a fresh, never-interned table variable —
/// and differs only in the record kept beside it.
auto infer_ctxt::fresh_type(std::string origin, source_location where)
    -> type_id {
  const auto id = table_->fresh_type_var();
  vars_.emplace(id, meta_var{.sort = meta_sort::type_sort,
                             .origin = std::move(origin),
                             .where = where});
  mint_order_.push_back(id);
  return id;
}

auto infer_ctxt::fresh_ctor(size_t arity, std::string origin,
                            source_location where) -> type_id {
  const auto id = table_->fresh_type_var();
  vars_.emplace(id, meta_var{.sort = meta_sort::ctor_sort,
                             .arity = arity,
                             .origin = std::move(origin),
                             .where = where});
  mint_order_.push_back(id);
  return id;
}

auto infer_ctxt::fresh_value(type_id underlying, std::string origin,
                             source_location where) -> type_id {
  const auto id = table_->fresh_type_var();
  vars_.emplace(id, meta_var{.sort = meta_sort::value_sort,
                             .underlying = underlying,
                             .origin = std::move(origin),
                             .where = where});
  mint_order_.push_back(id);
  return id;
}

/// Minted on first use and remembered, so every mention of `n` in every
/// polynomial in the session refers to one variable in one union-find.
auto infer_ctxt::value_param(std::string name, type_id underlying,
                             source_location where) -> type_id {
  if (const auto it = value_params_.find(name); it != value_params_.end()) {
    return it->second;
  }
  const auto id = fresh_value(underlying, name, where);
  value_params_.emplace(std::move(name), id);
  return id;
}

auto infer_ctxt::value_param_named(std::string_view name) const
    -> std::optional<type_id> {
  const auto it = value_params_.find(std::string(name));
  return it == value_params_.end() ? std::nullopt : std::optional(it->second);
}

auto infer_ctxt::is_meta(type_id id) const -> bool {
  return vars_.contains(id);
}

auto infer_ctxt::meta(type_id id) const -> const meta_var * {
  const auto it = vars_.find(id);
  return it == vars_.end() ? nullptr : &it->second;
}

/// Iterative two-pass compression: walk to the root, then point every link
/// on the path straight at it. Iterative rather than recursive because a
/// chain's length is bounded only by how many variables a session mints.
auto infer_ctxt::find(type_id id) const -> type_id {
  if (!is_meta(id)) {
    return id;
  }
  auto root = id;
  while (true) {
    const auto it = parent_.find(root);
    if (it == parent_.end()) {
      break;
    }
    root = it->second;
  }
  auto walk = id;
  while (walk != root) {
    const auto next = parent_[walk];
    parent_[walk] = root;
    walk = next;
  }
  return root;
}

auto infer_ctxt::solution(type_id id) const -> std::optional<type_id> {
  const auto root = find(id);
  const auto it = solution_.find(root);
  return it == solution_.end() ? std::nullopt : std::optional(it->second);
}

auto infer_ctxt::shallow_resolve(type_id id) const -> type_id {
  const auto root = find(id);
  const auto it = solution_.find(root);
  return it == solution_.end() ? root : it->second;
}

/// A value is acceptable for `var` when it can stand for `var`'s sort, and —
/// for a constructor — at `var`'s arity. A variable on the right is checked
/// against its own record instead of its (absent) shape.
auto infer_ctxt::check_sort(const meta_var &var, type_id value) const
    -> std::optional<bind_error> {
  auto refuse = [&](bind_failure failure, std::string detail) {
    return bind_error{
        .failure = failure, .value = value, .detail = std::move(detail)};
  };

  if (const auto *other = meta(value); other != nullptr) {
    if (other->sort != var.sort) {
      return refuse(bind_failure::sort_mismatch,
                    std::format("a {} cannot stand for a {}",
                                sort_name(other->sort), sort_name(var.sort)));
    }
    if (var.sort == meta_sort::ctor_sort && other->arity != var.arity) {
      return refuse(bind_failure::arity_mismatch,
                    std::format("a constructor taking {} argument(s) cannot "
                                "stand for one taking {}",
                                other->arity, var.arity));
    }
    return std::nullopt;
  }

  const auto &entry = table_->entry(value);
  // `unknown` and `error` stand for anything by design — one gap in
  // knowledge must not cascade (`types.h`), and that rule outranks sorts.
  if (entry.kind == type_kind::unknown_kind ||
      entry.kind == type_kind::error_kind) {
    return std::nullopt;
  }

  const auto sort = concrete_sort(entry);
  if (!sort.has_value()) {
    // An arity-0 `type_param_kind`: both a `T` and an `n` are spelled this
    // way, so it is accepted for either, and rejected only for `ctor_sort`
    // where the arity settles it.
    if (var.sort == meta_sort::ctor_sort) {
      return refuse(bind_failure::arity_mismatch,
                    std::format("`{}` takes no type arguments, but a "
                                "constructor taking {} was required",
                                entry.name, var.arity));
    }
    return std::nullopt;
  }
  if (*sort != var.sort) {
    return refuse(bind_failure::sort_mismatch,
                  std::format("`{}` is a {}, but a {} was required",
                              table_->display(value), sort_name(*sort),
                              sort_name(var.sort)));
  }
  if (var.sort == meta_sort::ctor_sort && entry.ctor_arity != var.arity) {
    return refuse(bind_failure::arity_mismatch,
                  std::format("`{}` takes {} type argument(s), but a "
                              "constructor taking {} was required",
                              entry.name, entry.ctor_arity, var.arity));
  }
  return std::nullopt;
}

/// Structural walk under the current substitution. Every kind's children are
/// visited through `args`/`result`, which between them cover every composite
/// kind in the table — deliberately not a per-kind switch, since a kind added
/// later would then silently stop being checked and let an infinite type
/// through.
auto infer_ctxt::occurs(type_id root, type_id value) const -> bool {
  const auto resolved = shallow_resolve(value);
  if (resolved == root) {
    return true;
  }
  if (is_meta(resolved)) {
    return false;
  }
  const auto &entry = table_->entry(resolved);
  for (const auto arg : entry.args) {
    if (occurs(root, arg)) {
      return true;
    }
  }
  return entry.result != resolved && occurs(root, entry.result);
}

auto infer_ctxt::bind(type_id var, type_id value, cause_id why)
    -> std::expected<void, bind_error> {
  const auto root = find(var);
  const auto *record = meta(root);
  if (record == nullptr) {
    return std::unexpected(
        bind_error{.failure = bind_failure::not_a_variable,
                   .var = var,
                   .value = value,
                   .why = why,
                   .detail = std::format("`{}` is not an inference variable",
                                         table_->display(var))});
  }
  if (solution_.contains(root)) {
    return std::unexpected(
        bind_error{.failure = bind_failure::not_a_variable,
                   .var = var,
                   .value = value,
                   .why = why,
                   .detail = "the variable already has a solution"});
  }

  const auto target = shallow_resolve(value);
  if (target == root) {
    return {}; // Already the same variable; nothing to record.
  }

  if (auto refusal = check_sort(*record, target); refusal.has_value()) {
    refusal->var = var;
    refusal->why = why;
    return std::unexpected(std::move(*refusal));
  }

  if (occurs(root, target)) {
    return std::unexpected(bind_error{
        .failure = bind_failure::occurs_check,
        .var = var,
        .value = target,
        .why = why,
        .detail = std::format("`{}` would contain itself",
                              record->origin.empty() ? table_->display(var)
                                                     : record->origin)});
  }

  if (is_meta(target)) {
    // Merge classes rather than recording a solution, so a later solution
    // for either reaches both. Direction is arbitrary but fixed: the bound
    // variable points at the target.
    parent_[root] = target;
  } else {
    solution_[root] = target;
  }
  zonk_memo_.clear();
  return {};
}

/// Applies a constructor to arguments, producing the *ordinary* interned
/// application — which is what keeps id-equality equal to type-equality once
/// a higher-kinded head is solved. A prelude constructor has no declaration;
/// a user one does.
auto infer_ctxt::apply_ctor(type_id ctor, std::vector<type_id> args)
    -> type_id {
  const auto &entry = table_->entry(ctor);
  if (entry.decl == nullptr) {
    return table_->builtin_generic(entry.name, std::move(args));
  }
  return table_->user_type(*entry.decl, entry.module_name, std::move(args));
}

auto infer_ctxt::zonk(type_id id) -> type_id {
  const auto resolved = shallow_resolve(id);
  if (is_meta(resolved)) {
    return resolved; // Unsolved: stands for itself.
  }
  if (const auto it = zonk_memo_.find(resolved); it != zonk_memo_.end()) {
    return it->second;
  }
  if (zonking_.contains(resolved)) {
    return resolved; // Already being rebuilt; see `zonking_`.
  }
  zonking_.insert(resolved);
  const auto rebuilt = rebuild(resolved);
  zonking_.erase(resolved);
  zonk_memo_[resolved] = rebuilt;
  return rebuilt;
}

/// Rebuilds one level through the table's constructors, with children
/// already zonked.
///
/// `existential_kind` is deliberately returned untouched: it is *nominal* —
/// minted fresh per `some Trait` written in source and never interned — so
/// rebuilding one would mint a second, distinct type for the same
/// declaration.
auto infer_ctxt::rebuild(type_id id) -> type_id {
  const auto &entry = table_->entry(id);
  auto zonk_args = [&] {
    auto args = std::vector<type_id>{};
    args.reserve(entry.args.size());
    for (const auto arg : entry.args) {
      args.push_back(zonk(arg));
    }
    return args;
  };

  switch (entry.kind) {
  case type_kind::unknown_kind:
  case type_kind::error_kind:
  case type_kind::builtin_kind:
  case type_kind::type_param_kind:
  case type_kind::ctor_ref_kind:
  case type_kind::type_var_kind:
  case type_kind::const_value_kind:
  case type_kind::const_variant_kind:
  case type_kind::existential_kind:
    return id;
  case type_kind::symbolic_value_kind: {
    // A polynomial's unknowns are *named*, so substituting into one means
    // looking each name up in the store and asking what it was solved to.
    // Re-interning through `symbolic_value` is what keeps the result
    // canonical: a polynomial that closes degrades to a `const_value`, so
    // `n + 1` with `n := 2` becomes the very id that `3` has.
    auto bindings = std::unordered_map<std::string, linear_poly>{};
    for (const auto &term : entry.value.terms) {
      const auto var = value_param_named(term.var);
      if (!var.has_value()) {
        continue;
      }
      const auto solved = zonk(*var);
      if (solved == *var) {
        continue; // Still open: leave the term as it stands.
      }
      bindings.emplace(term.var, table_->entry(solved).value);
    }
    if (bindings.empty()) {
      return id;
    }
    return table_->symbolic_value(entry.result,
                                  poly_substitute(entry.value, bindings));
  }
  case type_kind::builtin_generic_kind:
    return table_->builtin_generic(entry.name, zonk_args());
  case type_kind::tuple_kind:
    return table_->tuple_of(zonk_args());
  case type_kind::array_kind: {
    const auto length =
        entry.args.empty() ? k_unknown_type : zonk(entry.args.front());
    return table_->array_of(zonk(entry.result), entry.array_size, length);
  }
  case type_kind::fn_kind:
    return table_->fn_of(zonk_args(), zonk(entry.result));
  case type_kind::ref_kind:
    return table_->ref_to(zonk(entry.result), entry.is_mut);
  case type_kind::ptr_kind:
    return table_->ptr_to(zonk(entry.result), entry.is_mut);
  case type_kind::struct_kind:
  case type_kind::sum_kind:
  case type_kind::opaque_kind: {
    if (entry.decl == nullptr) {
      return id;
    }
    const auto *decl = entry.decl;
    const auto module_name = entry.module_name;
    return table_->user_type(*decl, module_name, zonk_args());
  }
  case type_kind::param_app_kind: {
    // The one rewrite that matters: once the head is solved to a real
    // constructor, `F[A]` must become the ordinary `option[int32]` id, not a
    // `param_app` carrying a solved head. Otherwise two spellings of one type
    // have two ids and canonicity is gone.
    const auto head = zonk(entry.result);
    auto args = zonk_args();
    if (table_->entry(head).kind == type_kind::ctor_ref_kind) {
      return apply_ctor(head, std::move(args));
    }
    return table_->param_app(head, std::move(args));
  }
  case type_kind::refinement_kind: {
    const auto *decl = entry.decl;
    const auto name = entry.name;
    const auto module_name = entry.module_name;
    const auto *predicate = entry.predicate;
    return table_->refinement_of(decl, name, module_name, zonk(entry.result),
                                 zonk_args(), predicate);
  }
  }
  return id;
}

auto infer_ctxt::add_cause(cause why) -> cause_id {
  causes_.push_back(std::move(why));
  return static_cast<cause_id>(causes_.size() - 1);
}

auto infer_ctxt::cause_at(cause_id id) const -> const cause & {
  if (static_cast<size_t>(id) >= causes_.size()) {
    return causes_[k_no_cause];
  }
  return causes_[id];
}

/// Innermost first, stopping at `k_no_cause`. A malformed parent chain
/// cannot loop forever: every parent was recorded before its child, so the
/// walk strictly decreases.
auto infer_ctxt::cause_chain(cause_id id) const -> std::vector<cause_id> {
  auto chain = std::vector<cause_id>{};
  auto walk = id;
  while (walk != k_no_cause && static_cast<size_t>(walk) < causes_.size()) {
    chain.push_back(walk);
    const auto parent = causes_[walk].parent;
    if (parent >= walk) {
      break;
    }
    walk = parent;
  }
  return chain;
}

auto infer_ctxt::meta_count() const -> size_t { return mint_order_.size(); }

auto infer_ctxt::unsolved() const -> std::vector<type_id> {
  auto open = std::vector<type_id>{};
  for (const auto id : mint_order_) {
    if (!solution(id).has_value()) {
      open.push_back(id);
    }
  }
  return open;
}

} // namespace kira::semantic::infer
