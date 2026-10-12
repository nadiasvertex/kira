#include "checker.h"

namespace cinder::semantic::detail {

/// The resolve context for types written in the body of the
/// function/type/impl currently being checked: current module plus the
/// live generic-parameter scope stack.
auto checker::current_resolve_ctx() -> resolve_ctx {
  return resolve_ctx{.module = file_.module_,
                     .param_bindings = nullptr,
                     .use_type_param_stack = true,
                     .quiet = false};
}

// ==========================================================================
//  Compile-time value slots (`spec/dependent-types-design.md` §2)
//
//  A type-argument slot may hold a *value* rather than a type: the `3` in
//  `vec[T, 3]`, the `n + 1` in `vec[T, n + 1]`, the `open` in
//  `connection[open]`. Closed values fold; open ones become canonical
//  linear polynomials; variants stay identities. Everything else leaves the
//  fragment and resolves to `unknown`, which unifies with everything and
//  diagnoses nothing — the standing policy for "I don't know".
// ==========================================================================

/// Looks up the value slot an in-scope name denotes — a value
/// type-parameter (`n`), or whatever a caller already substituted for it.
auto checker::lookup_value_binding(std::string_view name,
                                   const resolve_ctx &ctx)
    -> std::optional<type_id> {
  if (ctx.param_bindings != nullptr) {
    if (const auto it = ctx.param_bindings->find(std::string(name));
        it != ctx.param_bindings->end()) {
      return it->second;
    }
  }
  if (ctx.use_type_param_stack) {
    if (const auto param = lookup_type_param(name)) {
      return *param;
    }
  }
  return std::nullopt;
}

/// The polynomial a bare name contributes. An unsubstituted value parameter
/// is the unknown `n` itself; a substituted one contributes whatever the
/// caller passed (a constant, or another polynomial — which is what makes
/// `concat`'s `vec[T, m + n]` compose when `m` is itself `k + 1`).
auto checker::name_poly(std::string_view name, const resolve_ctx &ctx)
    -> std::optional<linear_poly> {
  const auto slot = lookup_value_binding(name, ctx);
  if (!slot.has_value()) {
    return std::nullopt;
  }
  const auto &entry = session_.types_.entry(*slot);
  switch (entry.kind) {
  case type_kind::type_param_kind:
    return poly_variable(value_var(*slot));
  case type_kind::const_value_kind:
  case type_kind::symbolic_value_kind:
    return entry.value;
  default:
    return std::nullopt;
  }
}

/// Builds the canonical polynomial denoted by a compile-time value
/// expression, or `nullopt` when the expression falls outside the linear
/// fragment (`m * n`, `n / 2`, a call, a field access — see the design doc
/// §2.1 and §4.4). A bare name arrives here as a `named_type` rather than
/// an `ident_expr`, because `vec[T, n]`'s `n` parses as a type before the
/// parser has any way to know the slot wants a value; both spellings are
/// accepted.
auto checker::value_poly(const ast::node &node, const resolve_ctx &ctx)
    -> std::optional<linear_poly> {
  switch (node.kind) {
  case ast::node_kind::literal_expr: {
    const auto &lit = dynamic_cast<const ast::literal_expr &>(node);
    if (lit.lit_kind != token_kind::int_lit) {
      return std::nullopt;
    }
    const auto value = parse_integer_literal(lit.value);
    if (!value.has_value()) {
      return std::nullopt;
    }
    return poly_constant(static_cast<int64_t>(*value));
  }
  case ast::node_kind::ident_expr:
    return name_poly(dynamic_cast<const ast::ident_expr &>(node).name, ctx);
  case ast::node_kind::named_type: {
    const auto &named = dynamic_cast<const ast::named_type &>(node);
    if (named.path.size() != 1 || !named.type_args.empty()) {
      return std::nullopt;
    }
    return name_poly(named.path.front(), ctx);
  }
  case ast::node_kind::unary_expr: {
    const auto &unary = dynamic_cast<const ast::unary_expr &>(node);
    if (unary.op != ast::unary_op::neg || unary.operand == nullptr) {
      return std::nullopt;
    }
    const auto operand = value_poly(*unary.operand, ctx);
    return operand.has_value() ? std::optional{poly_negate(*operand)}
                               : std::nullopt;
  }
  case ast::node_kind::binary_expr: {
    const auto &binary = dynamic_cast<const ast::binary_expr &>(node);
    if (binary.lhs == nullptr || binary.rhs == nullptr) {
      return std::nullopt;
    }
    const auto left = value_poly(*binary.lhs, ctx);
    const auto right = value_poly(*binary.rhs, ctx);
    if (!left.has_value() || !right.has_value()) {
      return std::nullopt;
    }
    switch (binary.op) {
    case ast::binary_op::add:
      return poly_add(*left, *right);
    case ast::binary_op::sub:
      return poly_sub(*left, *right);
    case ast::binary_op::mul:
      // Linear means linear: one side has to be closed, or the product
      // isn't a polynomial we can decide anything about.
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
  default:
    return std::nullopt;
  }
}

/// The bare name a value-argument node spells, if it is one — the `open` in
/// `connection[open]` (a `named_type`, since it looks like a type) or in
/// `connection[@open]` (an `ident_expr`, since `@` makes it unambiguously a
/// variant). Both spellings mean the same state.
auto checker::value_arg_name(const ast::node &node)
    -> std::optional<std::string> {
  if (node.kind == ast::node_kind::ident_expr) {
    return dynamic_cast<const ast::ident_expr &>(node).name;
  }
  if (node.kind == ast::node_kind::named_type) {
    const auto &named = dynamic_cast<const ast::named_type &>(node);
    if (named.path.size() == 1 && named.type_args.empty()) {
      return named.path.front();
    }
  }
  return std::nullopt;
}

/// Resolves one argument written for a declared *value* parameter
/// (`n: usize`, `S: conn_state`). A sum-typed parameter takes a variant
/// name; an integer-typed one takes an expression in the linear fragment.
auto checker::resolve_value_arg(const ast::node &node,
                                const ast::type_param &param,
                                const resolve_ctx &ctx, source_span span)
    -> type_id {
  const auto declared = param.bound_or_type != nullptr
                            ? resolve_type(*param.bound_or_type, quiet_ctx(ctx))
                            : session_.types_.usize_type();

  const auto &declared_entry = session_.types_.entry(declared);
  if (declared_entry.kind == type_kind::sum_kind &&
      declared_entry.decl != nullptr) {
    const auto name = value_arg_name(node);
    if (!name.has_value()) {
      return k_unknown_type;
    }
    const auto &sum = dynamic_cast<const ast::sum_type_def &>(
        *declared_entry.decl->definition);
    for (const auto &variant : sum.body.variants) {
      if (variant.name == *name) {
        return session_.types_.const_variant(*declared_entry.decl,
                                             declared_entry.module_name, *name);
      }
    }
    if (!ctx.quiet) {
      auto names = std::vector<std::string>{};
      for (const auto &variant : sum.body.variants) {
        names.push_back(variant.name);
      }
      auto diag = diagnostic(diagnostic_level::error,
                             std::format("`{}` is not a state of `{}`", *name,
                                         declared_entry.name),
                             file_.file_id_);
      diag.with_label(span, "not a variant of this parameter's type");
      diag.with_help(std::format(
          "The `{}` parameter of this type ranges over `{}`, whose states "
          "are {}.",
          param.name, declared_entry.name, join_strings(names, ", ")));
      emit_diag(diag);
      mark_error();
    }
    return k_error_type;
  }

  const auto poly = value_poly(node, ctx);
  if (!poly.has_value()) {
    return k_unknown_type;
  }
  return session_.types_.symbolic_value(declared, *poly);
}

/// A copy of `ctx` that reports nothing — used when resolving a type purely
/// to inspect it (a value parameter's declared type, say), where any
/// diagnostic would be a duplicate of one the declaration's own check emits.
auto checker::quiet_ctx(const resolve_ctx &ctx) -> resolve_ctx {
  auto quiet = ctx;
  quiet.quiet = true;
  return quiet;
}

/// The length slot of an `array[T, n]`: the same value grammar as any const
/// generic argument, over `usize` (an array length has no other type).
auto checker::resolve_length_arg(const ast::node &node, const resolve_ctx &ctx)
    -> type_id {
  const auto poly = value_poly(node, ctx);
  return poly.has_value() ? session_.types_.symbolic_value(
                                session_.types_.usize_type(), *poly)
                          : k_unknown_type;
}

/// The closed length of a value slot, when it has one — what layout and
/// both backends need. `nullopt` for a symbolic length, which is a length
/// no runtime representation can have.
auto checker::closed_length(type_id length) -> std::optional<uint64_t> {
  const auto &entry = session_.types_.entry(length);
  if (entry.kind != type_kind::const_value_kind || entry.value.constant < 0) {
    return std::nullopt;
  }
  return static_cast<uint64_t>(entry.value.constant);
}

/// Interns `array[T, <length>]` from a resolved length value slot, keeping
/// the plain integer size in sync for the backends.
auto checker::array_with_length(type_id element, type_id length) -> type_id {
  return session_.types_.array_of(element, closed_length(length), length);
}

/// Resolves each generic argument slot of a named type. A slot declared as
/// a *value* parameter routes through `resolve_value_arg` (see the section
/// comment above). A slot declared as — or, absent `decl_params`, simply
/// looking like — a type resolves structurally.
///
/// `decl_params` is the target declaration's own type parameters, which is
/// what tells a value slot from a type slot; it may be shorter than
/// `named`'s argument list (arity mismatches are diagnosed by the caller).
/// Without it — a prelude container, whose parameters are all types — the
/// old positional heuristics still apply, so a bare integer literal or an
/// in-scope value parameter still lands as a value.
auto checker::resolve_type_args(const std::vector<ast::type_arg> &type_args,
                                const resolve_ctx &ctx,
                                const std::vector<ast::type_param> *decl_params)
    -> std::vector<type_id> {
  auto args = std::vector<type_id>{};
  args.reserve(type_args.size());
  for (size_t i = 0; i < type_args.size(); ++i) {
    const auto &arg = type_args[i];
    if (arg.value == nullptr) {
      args.push_back(k_unknown_type);
      continue;
    }

    const auto *param = decl_params != nullptr && i < decl_params->size()
                            ? &(*decl_params)[i]
                            : nullptr;
    if (param != nullptr && param->is_value_param) {
      args.push_back(resolve_value_arg(*arg.value, *param, ctx, arg.span));
      continue;
    }

    // A type argument slot may hold a type or a compile-time value; only
    // type expressions resolve to types here.
    switch (arg.value->kind) {
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
      args.push_back(
          resolve_type(dynamic_cast<const ast::type_expr &>(*arg.value), ctx));
      break;
    case ast::node_kind::named_type:
      // A bare name resolves through the ordinary type path, which already
      // consults `param_bindings` and the type-parameter stack — so a slot
      // a caller has substituted a value into comes back as that value.
      args.push_back(as_value_slot(resolve_type(
          dynamic_cast<const ast::named_type &>(*arg.value), ctx)));
      break;
    case ast::node_kind::ident_expr: {
      const auto &ident = dynamic_cast<const ast::ident_expr &>(*arg.value);
      const auto slot = lookup_value_binding(ident.name, ctx);
      args.push_back(slot.has_value() ? as_value_slot(*slot) : k_unknown_type);
      break;
    }
    case ast::node_kind::literal_expr:
    case ast::node_kind::unary_expr:
    case ast::node_kind::binary_expr:
      // An unlabelled value slot (a prelude container, or a declaration
      // whose parameter list we couldn't consult): the value's own type
      // isn't declared anywhere, so it takes the `usize` the language uses
      // for every unannotated compile-time count.
      args.push_back(resolve_length_arg(*arg.value, ctx));
      break;
    default:
      args.push_back(k_unknown_type);
      break;
    }
  }
  return args;
}

// ==========================================================================
//  Struct / sum member queries with generic substitution
// ==========================================================================

/// Maps an instantiated user type's declared generic-parameter names to
/// the concrete argument ids it was instantiated with, for substituting
/// into field/variant-payload type expressions.
auto checker::param_bindings_for_instance(const type_entry &instance)
    -> type_scope {
  auto bindings = type_scope{};
  if (instance.decl == nullptr) {
    return bindings;
  }
  const auto &params = instance.decl->type_params;
  for (size_t i = 0; i < params.size(); ++i) {
    bindings.emplace(params[i].name, i < instance.args.size() ? instance.args[i]
                                                              : k_unknown_type);
  }
  return bindings;
}

/// Builds the resolve context for a struct field or sum variant payload
/// type: the instance's owning module, plus its generic-parameter
/// substitution.
auto checker::member_resolve_ctx(const type_entry &instance,
                                 const type_scope &bindings) -> resolve_ctx {
  return resolve_ctx{.module = index_.find_module(instance.module_name),
                     .param_bindings = &bindings,
                     .use_type_param_stack = false,
                     .quiet = true};
}

/// Makes `file_.file_id_` the file `instance`'s declaration was written in for
/// the lifetime of the returned guard. Name resolution reads imports out
/// of `file_.file_id_`, so a member type written with an imported name
/// (`inner: shared[noisy]` under `use std.shared.shared`) resolved under
/// whatever file the checker last stood in, often unknown, which left the
/// type undroppable.
auto checker::enter_declaring_file(const type_entry &instance) {
  struct guard {
    file_id_type &slot;
    file_id_type saved;
    ~guard() { slot = saved; }
  };
  auto result = guard{.slot = file_.file_id_, .saved = file_.file_id_};
  if (instance.decl != nullptr) {
    if (const auto *module = index_.find_module(instance.module_name)) {
      if (const auto it = module->types.find(instance.decl->name);
          it != module->types.end() && it->second.decl == instance.decl) {
        file_.file_id_ = it->second.file_id;
      }
    }
  }
  return result;
}

/// Returns the field list of a struct-kind instance, or `nullptr` if
/// `instance` is not a struct.
auto checker::struct_fields_of(const type_entry &instance)
    -> const std::vector<ast::struct_field> * {
  if (instance.kind != type_kind::struct_kind || instance.decl == nullptr ||
      instance.decl->definition == nullptr ||
      instance.decl->definition->kind != ast::node_kind::struct_type_def) {
    return nullptr;
  }
  return &dynamic_cast<const ast::struct_type_def &>(*instance.decl->definition)
              .body.fields;
}

/// Looks up a field by name on a struct instance, resolving its declared
/// type with the instance's generic substitution applied. `nullopt` when
/// the field does not exist (distinct from `k_unknown_type`, which means
/// the field exists but has no annotation).
auto checker::struct_field_type(const type_entry &instance,
                                std::string_view name)
    -> std::optional<type_id> {
  const auto *fields = struct_fields_of(instance);
  if (fields == nullptr) {
    return std::nullopt;
  }
  for (const auto &field : *fields) {
    if (field.name == name) {
      if (field.type == nullptr) {
        return k_unknown_type;
      }
      const auto bindings = param_bindings_for_instance(instance);
      const auto in_declaring_file = enter_declaring_file(instance);
      return resolve_type(*field.type, member_resolve_ctx(instance, bindings));
    }
  }
  return std::nullopt;
}

/// Returns the variant list of a sum-kind instance, or `nullptr` if
/// `instance` is not a sum type.
auto checker::sum_variants_of(const type_entry &instance)
    -> const std::vector<ast::sum_variant> * {
  if (instance.kind != type_kind::sum_kind || instance.decl == nullptr ||
      instance.decl->definition == nullptr ||
      instance.decl->definition->kind != ast::node_kind::sum_type_def) {
    return nullptr;
  }
  return &dynamic_cast<const ast::sum_type_def &>(*instance.decl->definition)
              .body.variants;
}

/// Looks up a variant by name on a sum-type instance.
auto checker::find_variant(const type_entry &instance, std::string_view name)
    -> const ast::sum_variant * {
  const auto *variants = sum_variants_of(instance);
  if (variants == nullptr) {
    return nullptr;
  }
  for (const auto &variant : *variants) {
    if (variant.name == name) {
      return &variant;
    }
  }
  return nullptr;
}

/// Resolves a variant's payload types with the instance's generic
/// substitution applied.
auto checker::variant_payload_types(const type_entry &instance,
                                    const ast::sum_variant &variant)
    -> std::vector<type_id> {
  auto payload = std::vector<type_id>{};
  payload.reserve(variant.payload_types.size());
  const auto bindings = param_bindings_for_instance(instance);
  const auto ctx = member_resolve_ctx(instance, bindings);
  const auto in_declaring_file = enter_declaring_file(instance);
  for (const auto &type : variant.payload_types) {
    payload.push_back(type != nullptr ? resolve_type(*type, ctx)
                                      : k_unknown_type);
  }
  return payload;
}

/// Whether values of `id` can carry a borrow — see `checked_types::
/// borrow_bearing_types`: a view, a reference, a callable (a closure may
/// hold `&`/`&mut` captures), or a type parameter, directly or through a
/// pointer inner type, array/tuple element, generic argument, struct
/// field, or sum-variant payload. `str` is deliberately not a view here.
/// `visited` breaks recursive types (`type tree = { kids: list[tree] }`);
/// a genuine borrow is always reachable on an acyclic path, so cycle
/// edges may safely report "no borrow". With `params_bear` false, a type
/// parameter counts as carrying nothing: the question becomes whether the
/// type carries a borrow of its own, whatever its parameters turn out to be.
auto checker::type_carries_borrow(type_id id,
                                  std::unordered_set<type_id> &visited,
                                  bool params_bear) -> bool {
  if (session_.types_.is_unknown(id) || id == k_error_type) {
    return false;
  }
  if (!visited.insert(id).second) {
    return false;
  }
  const auto &e = session_.types_.entry(id);
  if (e.kind == type_kind::type_param_kind) {
    return params_bear;
  }
  if (session_.types_.is_view(id) || e.kind == type_kind::ref_kind ||
      e.kind == type_kind::fn_kind || e.kind == type_kind::param_app_kind) {
    return true;
  }
  if (e.result != k_unknown_type &&
      type_carries_borrow(e.result, visited, params_bear)) {
    return true;
  }
  for (const auto arg : e.args) {
    if (type_carries_borrow(arg, visited, params_bear)) {
      return true;
    }
  }
  if (e.kind == type_kind::struct_kind) {
    if (const auto *fields = struct_fields_of(e)) {
      for (const auto &field : *fields) {
        const auto ft = struct_field_type(e, field.name);
        if (ft.has_value() && type_carries_borrow(*ft, visited, params_bear)) {
          return true;
        }
      }
    }
  } else if (e.kind == type_kind::sum_kind) {
    if (const auto *variants = sum_variants_of(e)) {
      for (const auto &variant : *variants) {
        for (const auto payload : variant_payload_types(e, variant)) {
          if (type_carries_borrow(payload, visited, params_bear)) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

/// Classifies every interned `type_id` with `type_carries_borrow`, for
/// `checked_types::borrow_bearing_types`. Snapshots the table size first: a
/// struct/sum query may intern further types via `resolve_type`, but ids are
/// dense and existing entries are stable, and no expression the ownership
/// checker queries can have a type minted only during this walk.
/// With `params_bear` false, computes `checked_types::
/// own_borrow_bearing_types` instead. `snapshot` is the table size to
/// classify up to, shared by both sets.
auto checker::compute_borrow_bearing_types(std::size_t snapshot,
                                           bool params_bear)
    -> std::unordered_set<type_id> {
  auto result = std::unordered_set<type_id>{};
  for (std::size_t raw = 0; raw < snapshot; ++raw) {
    const auto id = static_cast<type_id>(raw);
    auto visited = std::unordered_set<type_id>{};
    if (type_carries_borrow(id, visited, params_bear)) {
      result.insert(id);
    }
  }
  return result;
}

/// Whether `id`'s own `impl drop` resolves, and if so to what — the leaf
/// case `resolve_drop_plan` recurses down to. Mirrors `require_operand_
/// trait`'s struct/sum handling (`type_has_trait` + `find_method`, then
/// `instantiate_impl_method_for` for a per-instance generic impl), but
/// unconditionally rather than gated on an operator expression's
/// `wire_dispatch` — every droppable type's own drop must be resolved
/// here, not just the ones an operator happens to touch.
auto checker::resolve_own_drop(const type_entry &entry, type_id id)
    -> std::optional<resolved_callee> {
  if (entry.decl == nullptr || !type_has_trait(entry, "drop")) {
    return std::nullopt;
  }
  const auto *method = find_method(entry, "drop", id);
  if (method == nullptr || method->decl->params.empty() ||
      param_name_of(method->decl->params.front()) != "self") {
    return std::nullopt;
  }
  const auto *callee =
      instantiate_impl_method_for(*entry.decl, *method, entry, id);
  return resolved_callee{.decl = callee != nullptr ? callee : method->decl,
                         .owner_module = method->owner->module_name,
                         .impl_target_type =
                             callee != nullptr ? "" : entry.name};
}

/// Whether `id` is droppable — has its own `impl drop`, or (struct only) a
/// field that is; see `drop_plan` in types.h. `visiting` is the current
/// recursion path, not a global "already seen" set — a sibling field of
/// the same type as an earlier one must still be evaluated on its own, so
/// entries are erased on the way back out, unlike `type_carries_borrow`'s
/// `visited` (safe there only because that query is monotonic: once any
/// branch answers "true", nothing downstream needs re-exploring). Cycle
/// detection here is defense-in-depth rather than an expected case — a
/// struct cannot directly contain itself by value, only through `*T`/
/// `ref`, neither of which is ever droppable.
auto checker::resolve_drop_plan(type_id id,
                                std::unordered_set<type_id> &visiting)
    -> std::optional<drop_plan> {
  if (session_.types_.is_unknown(id) || id == k_error_type) {
    return std::nullopt;
  }
  const auto &entry = session_.types_.entry(id);
  if (entry.kind == type_kind::builtin_generic_kind &&
      entry.name == "generator") {
    // What a generator's body still owns depends on where it is
    // suspended, so every generator is dropped by cancelling it.
    return drop_plan{.cancels_generator = true};
  }
  if (entry.kind == type_kind::fn_kind) {
    // What a closure's environment owns depends on the lambda that made
    // it, so every `fn` value is dropped through the glue it carries.
    return drop_plan{.closure_glue = session_.closure_glue_type_};
  }
  const auto is_prelude_sum =
      entry.kind == type_kind::builtin_generic_kind &&
      ((entry.name == "option" && entry.args.size() == 1) ||
       (entry.name == "result" && entry.args.size() == 2));
  if (entry.kind != type_kind::struct_kind &&
      entry.kind != type_kind::sum_kind && !is_prelude_sum) {
    return std::nullopt;
  }
  if (!visiting.insert(id).second) {
    return std::nullopt;
  }
  auto plan = drop_plan{.own_drop = resolve_own_drop(entry, id)};
  if (is_prelude_sum) {
    // `option`/`result` have no declaration to read variants from; their
    // payloads are the generic arguments. `some`/`ok` hold `args[0]`,
    // `err` holds `args[1]`.
    const auto variants =
        entry.name == "option"
            ? std::vector<std::pair<std::string, type_id>>{{"some",
                                                            entry.args[0]}}
            : std::vector<std::pair<std::string, type_id>>{
                  {"ok", entry.args[0]}, {"err", entry.args[1]}};
    for (const auto &[variant, payload] : variants) {
      if (resolve_drop_plan(payload, visiting).has_value()) {
        plan.variant_drops.push_back(
            variant_drop{.variant = variant,
                         .arity = 1,
                         .droppable_payloads = {{0, payload}}});
      }
    }
  }
  if (entry.kind == type_kind::sum_kind) {
    if (const auto *variants = sum_variants_of(entry)) {
      for (const auto &variant : *variants) {
        auto vd = variant_drop{.variant = variant.name,
                               .arity = variant.payload_types.size()};
        const auto payloads = variant_payload_types(entry, variant);
        for (size_t i = 0; i < payloads.size(); ++i) {
          if (resolve_drop_plan(payloads[i], visiting).has_value()) {
            vd.droppable_payloads.emplace_back(i, payloads[i]);
          }
        }
        if (!vd.droppable_payloads.empty()) {
          plan.variant_drops.push_back(std::move(vd));
        }
      }
    }
  }
  if (entry.kind == type_kind::struct_kind) {
    if (const auto *fields = struct_fields_of(entry)) {
      for (const auto &field : *fields) {
        const auto field_type = struct_field_type(entry, field.name);
        if (field_type.has_value() &&
            resolve_drop_plan(*field_type, visiting).has_value()) {
          plan.droppable_fields.emplace_back(field.name, *field_type);
        }
      }
    }
  }
  visiting.erase(id);
  if (!plan.own_drop.has_value() && plan.droppable_fields.empty() &&
      plan.variant_drops.empty()) {
    return std::nullopt;
  }
  return plan;
}

/// Populates `session_.drop_plans_` for `checked_types::drop_plans`. Run once,
/// over the whole type table, from `take_checked_types` — mirroring `compute_
/// borrow_bearing_types` immediately above: a struct/sum type's droppability
/// depends on its fields' resolved types, which needs the same per-
/// instance generic substitution `struct_field_type` does, so this can't
/// run any earlier than the type table being essentially final.
/// Whether `id` mentions an inference variable anywhere inside it.
///
/// Such a type is not a type of the program. It is a `list[?a]` the checker
/// built while an empty `[]` was waiting to be pinned (phase 8), and it
/// stays interned afterwards even though the variable it names has since
/// been solved. Anything that enumerates *every* interned type has to skip
/// it, or it asks for the drop plan of a type no value ever has — which
/// mints `list::drop$list___`, a function the backends are told to compile
/// and nothing ever compiles.
auto checker::mentions_type_var(type_id id, std::unordered_set<type_id> &seen)
    -> bool {
  if (!seen.insert(id).second) {
    return false;
  }
  const auto entry = session_.types_.entry(id); // copy: recursion may intern
  if (entry.kind == type_kind::type_var_kind) {
    return true;
  }
  for (const auto arg : entry.args) {
    if (mentions_type_var(arg, seen)) {
      return true;
    }
  }
  return entry.result != id && mentions_type_var(entry.result, seen);
}

/// `mentions_type_var` without the caller having to supply the scratch set.
auto checker::mentions_type_var(type_id id) -> bool {
  auto seen = std::unordered_set<type_id>{};
  return mentions_type_var(id, seen);
}

/// Every metavariable `id` mentions, in first-seen order — what an
/// obligation watches, so it is retried exactly when one of them gains a
/// solution and never otherwise.
auto checker::collect_type_vars(type_id id, std::vector<type_id> &out,
                                std::unordered_set<type_id> &seen) -> void {
  if (!seen.insert(id).second) {
    return;
  }
  const auto entry = session_.types_.entry(id); // copy: recursion may intern
  if (entry.kind == type_kind::type_var_kind &&
      std::ranges::find(out, id) == out.end()) {
    out.push_back(id);
  }
  for (const auto arg : entry.args) {
    collect_type_vars(arg, out, seen);
  }
  if (entry.result != id) {
    collect_type_vars(entry.result, out, seen);
  }
}

auto checker::resolve_drop_plans() -> void {
  session_.closure_glue_type_ =
      session_.types_.fn_of({}, session_.types_.builtin("unit"));
  const auto snapshot = session_.types_.count();
  for (std::size_t raw = 0; raw < snapshot; ++raw) {
    const auto id = static_cast<type_id>(raw);
    auto scratch = std::unordered_set<type_id>{};
    if (mentions_type_var(id, scratch)) {
      continue;
    }
    auto visiting = std::unordered_set<type_id>{};
    if (auto plan = resolve_drop_plan(id, visiting); plan.has_value()) {
      session_.drop_plans_.emplace(id, std::move(*plan));
    }
  }
}

// ==========================================================================
//  Function signatures
// ==========================================================================

/// Returns a parameter's bound name if it is a simple identifier
/// pattern, or an empty string for a destructuring parameter pattern.
auto checker::param_name_of(const ast::param &param) -> std::string {
  if (param.pattern != nullptr &&
      param.pattern->kind == ast::node_kind::binding_pattern) {
    return dynamic_cast<const ast::binding_pattern &>(*param.pattern).name;
  }
  return {};
}

/// Returns `decl`'s parameter types with unannotated, non-`self`
/// parameters filled in from local body-usage inference where possible
/// (`param_usage_inferrer` above); indices align 1:1 with `decl.params`.
/// Annotated parameters and `self` are untouched by this cache — callers
/// still resolve those the way they always have. Generic functions,
/// `pub` functions (which must annotate everything already), and a
/// function already being inferred higher up the call stack (recursion
/// guard) skip inference and simply report `k_unknown_type`, identical
/// to today's behavior.
auto checker::param_types_for(const ast::func_decl &decl,
                              const module_members *owner)
    -> const std::vector<type_id> & {
  if (const auto it = cache_.inferred_param_types_.find(&decl);
      it != cache_.inferred_param_types_.end()) {
    return it->second;
  }
  auto &stored = cache_.inferred_param_types_
                     .emplace(&decl, std::vector<type_id>(decl.params.size(),
                                                          k_unknown_type))
                     .first->second;
  (void)owner;
  if (!has_unannotated_params(decl)) {
    return stored;
  }
  // One leaf per unannotated parameter, in the session store. The body's
  // own uses solve it — the same solver every other leaf has — and a
  // caller that meets it earlier ties its argument to it. Nothing here
  // walks the body.
  for (size_t i = 0; i < decl.params.size(); ++i) {
    const auto &param = decl.params[i];
    const auto name = param_name_of(param);
    if (param.type_annotation != nullptr || name.empty() ||
        (i == 0 && name == "self")) {
      continue;
    }
    stored[i] = leaf_ctxt_.fresh_type(
        std::format("the type of parameter `{}`", name),
        source_location{.file_id = file_.file_id_, .span = param.span});
  }
  return stored;
}

/// Whether `decl` has an unannotated, non-`self` parameter to infer from
/// its body. `pub` functions must annotate everything, and a generic
/// function's parameters are its own type parameters' business.
auto checker::has_unannotated_params(const ast::func_decl &decl) -> bool {
  if (decl.visibility == ast::visibility::pub || !decl.type_params.empty()) {
    return false;
  }
  for (size_t i = 0; i < decl.params.size(); ++i) {
    const auto name = param_name_of(decl.params[i]);
    if (decl.params[i].type_annotation == nullptr && !name.empty() &&
        !(i == 0 && name == "self")) {
      return true;
    }
  }
  return false;
}

/// The rigid `type_param` stand-ins a declaration's signature resolves
/// against: its own parameters, plus — when it is a method — those of the
/// `impl` or `extend` block enclosing it, which are equally in scope over the
/// signature and are the only thing distinguishing `impl[T] ... for
/// holder[T]` from an impl for one concrete instantiation.
auto checker::generic_param_bindings(
    const ast::func_decl &decl,
    const std::vector<ast::type_param> *enclosing_block_params) -> type_scope {
  auto bindings = type_scope{};
  const auto seed = [&](const ast::type_param &type_param) -> void {
    if (!type_param.name.empty()) {
      bindings.emplace(
          type_param.name,
          session_.types_.type_param(
              type_param.name, type_param.higher_kinded_arity, &type_param));
    }
  };
  if (enclosing_block_params != nullptr) {
    for (const auto &type_param : *enclosing_block_params) {
      seed(type_param);
    }
  }
  // Order between the two is immaterial: a method parameter shadowing an
  // impl one of the same name resolves to a `type_param` stand-in that is
  // interned identically either way. What matters is only that both sets
  // are present.
  for (const auto &type_param : decl.type_params) {
    seed(type_param);
  }
  return bindings;
}

/// Normalizes a function declaration's parameter list into
/// `fn_param_info`s with their types resolved against the function's own
/// generic parameters. `skip_self` drops a leading `self` parameter,
/// since call-argument checking never expects the caller to pass it.
///
/// `enclosing_block_params` seeds the enclosing `impl`/`extend` block's
/// parameters as well. A method of `impl[T] get_it[T] for holder[T]`
/// mentions `T` in its signature without declaring it — the parameter
/// belongs to the block — so resolving the signature against
/// `decl.type_params` alone leaves `T` unresolvable, and the method reads as
/// if it had no generic content at all.
auto checker::signature_params(
    const ast::func_decl &decl, const module_members *owner, bool skip_self,
    const std::vector<ast::type_param> *enclosing_block_params)
    -> std::vector<fn_param_info> {
  auto param_bindings = generic_param_bindings(decl, enclosing_block_params);
  const auto ctx = resolve_ctx{.module = owner,
                               .param_bindings = &param_bindings,
                               .use_type_param_stack = false,
                               .quiet = true};

  const auto &inferred = param_types_for(decl, owner);
  auto params = std::vector<fn_param_info>{};
  params.reserve(decl.params.size());
  for (size_t i = 0; i < decl.params.size(); ++i) {
    const auto &param = decl.params[i];
    auto name = param_name_of(param);
    if (i == 0 && skip_self && name == "self") {
      continue;
    }
    auto type = k_unknown_type;
    if (param.type_annotation != nullptr) {
      type = resolve_type(*param.type_annotation, ctx);
    } else if (i < inferred.size()) {
      type = inferred[i];
      // A caller gets its own copy of an unannotated parameter's leaf, so
      // `double(3)` and `double(3.5)` each choose at their own call rather
      // than the first pinning the shared one the body is checked against.
      // Always, once the body is known to leave it open: the body's leaf
      // is then the implicit type parameter, and a later default of it
      // answers nothing about a call.
      if (type != k_unknown_type && !name.empty()) {
        const auto settled = leaf_ctxt_.zonk(type);
        const auto classified = open_param_decls_.find(&decl);
        const auto generic =
            classified != open_param_decls_.end() && classified->second;
        type = !generic && !mentions_type_var(settled)
                   ? settled
                   : leaf_ctxt_.fresh_type(
                         std::format("the type of parameter `{}` at this call",
                                     name),
                         source_location{.file_id = file_.file_id_,
                                         .span = param.span});
      }
    }
    params.push_back(fn_param_info{
        .name = name,
        .type = type,
        .has_default = param.default_value != nullptr,
        .span = param.span,
        .default_value = param.default_value.get(),
    });
  }
  return params;
}

/// Folds one returned value's type into the inferred return type. The
/// first fixes it; each later one must agree, which also pins a literal's
/// leaf to the width an earlier `return` gave it. A `never` path says
/// nothing about the result.
auto checker::note_inferred_return(type_id found, source_span span) -> void {
  if (session_.types_.entry(found).name == "never") {
    return;
  }
  if (!function_.inferred_return_seen_) {
    function_.inferred_return_seen_ = true;
    function_.inferred_return_ = found;
    return;
  }
  type_mismatch(span, function_.inferred_return_, found,
                "as a result of this function, like its other returns");
}

/// The return type of a function declared without one, taken from its
/// body (`spec/specification/01-core/04-functions.md`). A callee written
/// after its caller is checked now, in its own module and file, and skipped
/// when its turn comes; a call back into a body still being checked
/// (recursion) sees `unknown`, which unifies with everything.
auto checker::inferred_return_of(const ast::func_decl &decl,
                                 const module_members *owner) -> type_id {
  const auto found = inferred_returns_.find(&decl);
  if (found != inferred_returns_.end()) {
    return found->second;
  }
  if (owner == nullptr || !decl.type_params.empty() ||
      decl.modifiers.is_generator || decl.modifiers.is_intrinsic ||
      decl.has_error ||
      (decl.body_expr == nullptr && decl.body_stmts.empty())) {
    return k_unknown_type;
  }
  if (!return_inference_started_.insert(&decl).second) {
    // A call from inside the body being checked: its result is whatever the
    // body's own returns turn out to be, so it waits as a leaf that they
    // pin (`return n * fact(n - 1)` beside `return 1`).
    const auto [slot, added] = recursive_returns_.try_emplace(&decl, 0);
    if (added) {
      slot->second = leaf_ctxt_.fresh_type(
          std::format("the result of `{}`, called from its own body",
                      decl.name),
          source_location{.file_id = file_.file_id_, .span = decl.span});
    }
    return slot->second;
  }
  const auto listed = owner->functions.find(decl.name);
  if (listed == owner->functions.end() || listed->second.decl != &decl) {
    return k_unknown_type;
  }
  early_checked_.insert(&decl);
  const auto saved_module = file_.module_;
  const auto saved_file = file_.file_id_;
  const auto saved_self = function_.self_type_;
  const auto saved_probe = std::exchange(probe_capture_, nullptr);
  file_.module_ = owner;
  file_.file_id_ = listed->second.file_id;
  function_.self_type_ = k_unknown_type;
  check_function(decl, /*at_module_scope=*/true);
  file_.module_ = saved_module;
  file_.file_id_ = saved_file;
  function_.self_type_ = saved_self;
  probe_capture_ = saved_probe;
  const auto done = inferred_returns_.find(&decl);
  return done != inferred_returns_.end() ? done->second : k_unknown_type;
}

/// Resolves a function's declared return type against its own generic
/// parameters; `unknown` for an unannotated return type (inferred from the
/// body, never guessed from this signature alone).
auto checker::signature_return_type(
    const ast::func_decl &decl, const module_members *owner,
    const std::vector<ast::type_param> *enclosing_block_params) -> type_id {
  if (decl.return_type == nullptr) {
    return inferred_return_of(decl, owner);
  }
  auto param_bindings = generic_param_bindings(decl, enclosing_block_params);
  const auto ctx =
      resolve_ctx{.module = owner,
                  .param_bindings = &param_bindings,
                  .use_type_param_stack = false,
                  .quiet = true,
                  .existential_allowed = true,
                  .is_generator_return = decl.modifiers.is_generator};
  return resolve_type(*decl.return_type, ctx);
}

/// Builds the `fn(...)` value type of a function declaration, used when
/// a function name is referenced as a value (passed as an argument,
/// assigned to a binding) rather than called directly.
auto checker::fn_type_of(const ast::func_decl &decl,
                         const module_members *owner) -> type_id {
  auto params = std::vector<type_id>{};
  for (const auto &param : signature_params(decl, owner, false)) {
    params.push_back(param.type);
  }
  auto result = signature_return_type(decl, owner);
  if (result == k_unknown_type && decl.return_type == nullptr &&
      decl.body_expr == nullptr && decl.body_stmts.empty()) {
    result = session_.types_.builtin("unit");
  }
  return session_.types_.fn_of(std::move(params), result);
}

/// Comma-separated `` `Trait[Args]` `` list for an existential type's
/// bound, used in "only exposes ..." diagnostics.
auto checker::existential_bound_names(const type_entry &entry) -> std::string {
  auto out = std::string{};
  for (const auto &required : entry.existential_bound) {
    if (!out.empty()) {
      out += ", ";
    }
    out += std::format("`{}`", required.trait_name);
  }
  return out;
}

} // namespace cinder::semantic::detail
