#include "checker.h"

namespace cinder::semantic::detail {

/// Defers `run` to every instance of the template being checked.
///
/// A no-op outside a template: code that is not generic has nothing to
/// instantiate and makes its decisions in place.
auto checker::defer_to_instances(const ast::node &site,
                                 std::function<void(instance_subst &)> run)
    -> void {
  if (function_.current_template_ == nullptr) {
    return;
  }
  template_elaborations_[function_.current_template_].push_back(
      template_elaboration{.span = site.span, .run = std::move(run)});
}

/// `id` with every abstract parameter replaced by the instance's answer.
auto checker::substitute_type(type_id id, instance_subst &subst) -> type_id {
  return detail::substitute_type(
      session_.types_, [this](type_id t) -> type_id { return settle(t); },
      subst, id);
}

/// The record tables an instance copies from its template.
auto checker::template_records() -> template_record_tables {
  return template_record_tables{
      .node_types = session_.node_types_,
      .node_files = session_.node_files_,
      .struct_pattern_field_types = session_.struct_pattern_field_types_,
      .struct_literal_field_types = session_.struct_literal_field_types_,
      .struct_literal_field_expected = session_.struct_literal_field_expected_,
      .call_expected_types = session_.call_expected_types_,
      .call_argument_mappings = session_.call_argument_mappings_,
      .resolved_callees = session_.resolved_callees_,
      .resolved_fn_values = session_.resolved_fn_values_,
      .operator_dispatches = session_.operator_dispatches_,
      .ord_dispatch_result_types = session_.ord_dispatch_result_types_,
      .index_dispatches = session_.index_dispatches_,
      .index_set_dispatches = session_.index_set_dispatches_,
      .index_mut_dispatches = session_.index_mut_dispatches_,
      .index_ref_dispatches = session_.index_ref_dispatches_,
      .try_conversions = session_.try_conversions_,
      .try_conversion_types = session_.try_conversion_types_,
      .array_literal_conversions = session_.array_literal_conversions_,
      .interp_dispatches = session_.interp_dispatches_,
      .type_param_reflections = session_.type_param_reflections_,
      .for_iterator_dispatches = session_.for_iterator_dispatches_,
      .comprehension_iterator_dispatches =
          session_.comprehension_iterator_dispatches_,
      .comprehension_dispatches = session_.comprehension_dispatches_,
      .runtime_fill_dispatches = session_.runtime_fill_dispatches_,
      .folded_comptime_calls = session_.folded_comptime_calls_,
      .layout_queries = session_.layout_queries_,
      .ptr_casts = session_.ptr_casts_,
      .slice_from_raw_parts_calls = session_.slice_from_raw_parts_calls_,
      .stack_buffers = session_.stack_buffers_,
      .static_if_taken_branch = session_.static_if_taken_branch_,
      .static_global_refs = session_.static_global_refs_,
      .static_const_values = session_.static_const_values_,
      .value_path_types = session_.value_path_types_,
      .proven_in_bounds = session_.proven_in_bounds_,
      .elided_contracts = session_.elided_contracts_,
      .spliced_fragments = session_.spliced_fragments_,
  };
}

/// Copies every record the template made onto the instance's clone, with
/// its types substituted.
auto checker::copy_all_template_records(instance_subst &subst) -> void {
  copy_template_records(
      template_records(), session_.types_,
      [this](type_id t) -> type_id { return settle(t); }, subst);
}

auto checker::drop_all_dead_records(const instance_subst &subst) -> void {
  drop_dead_records(template_records(), subst);
}

/// Each abstract parameter of `item`'s template, matched to the answer the
/// instance gives it.
auto checker::instance_subst_for(const pending_instance &item)
    -> instance_subst {
  auto subst = instance_subst{.pairs = item.clone_pairs.get(),
                              .tmpl = item.tmpl,
                              .instance = item.instance};
  const auto abstract = template_abstract_params_.find(item.tmpl);
  if (abstract == template_abstract_params_.end()) {
    return subst;
  }
  const auto answer = [&](const std::string &name) -> std::optional<type_id> {
    if (const auto it = item.solution.type_slots.find(name);
        it != item.solution.type_slots.end()) {
      return it->second;
    }
    if (const auto it = item.solution.const_slots.find(name);
        it != item.solution.const_slots.end()) {
      return it->second;
    }
    if (item.fixed_type_params.has_value()) {
      if (const auto it = item.fixed_type_params->find(name);
          it != item.fixed_type_params->end()) {
        return it->second;
      }
    }
    for (const auto &scope : std::views::reverse(item.enclosing_type_params)) {
      if (const auto it = scope.find(name); it != scope.end()) {
        return it->second;
      }
    }
    return std::nullopt;
  };
  for (const auto &[name, id] : abstract->second) {
    if (const auto found = answer(name); found.has_value()) {
      subst.params.emplace(id, *found);
      const auto &entry = session_.types_.entry(*found);
      if (entry.kind == type_kind::const_value_kind) {
        subst.values.emplace(value_var(id), entry.value);
      }
    }
    if (const auto it = item.solution.values.find(name);
        it != item.solution.values.end()) {
      subst.values.insert_or_assign(value_var(id), poly_constant(it->second));
    }
  }
  return subst;
}

/// Makes `item`'s records from its template's, and runs the decisions the
/// template deferred to it.
auto checker::substitute_instance(pending_instance &item) -> void {
  clone_generic_lambdas_for(item);
  auto subst = instance_subst_for(item);
  copy_all_template_records(subst);
  // The instance's own parameters, as the compile-time values a `static
  // if` or a `let n = T.name()` evaluates against.
  auto type_param_values = type_param_comptime_values(
      item.instance->type_params, session_.type_param_slots_);
  comptime_eval_.push_locals(
      {type_param_values.begin(), type_param_values.end()});
  const auto found = template_elaborations_.find(item.tmpl);
  if (found != template_elaborations_.end()) {
    // By index: an elaboration can instantiate, which can check a template
    // on demand and so append to this very list.
    for (const auto &elaboration : found->second) {
      if (!is_dead(subst, elaboration.span)) {
        elaboration.run(subst);
      }
    }
  }
  comptime_eval_.pop_locals();
  drop_all_dead_records(subst);
  // An inferred return is the template's, under the substitution.
  if (const auto returned = inferred_returns_.find(item.tmpl);
      returned != inferred_returns_.end()) {
    inferred_returns_[item.instance] = substitute_type(returned->second, subst);
  }
}

/// Reads the explicit compile-time arguments out of a call's callee, and
/// hands back the identifier the callee really names.
///
/// The parser can't tell `values[0]` from `zeros[8]` and doesn't try
/// (`parse_postfix`): a single unnamed bracket argument becomes an
/// `index_expr`, several become a `call_expr`. Both shapes mean explicit
/// instantiation when the base turns out to name a generic declaration,
/// which only the checker knows — so both are unwrapped here.
auto checker::explicit_generic_callee(const ast::expr &callee,
                                      explicit_generic_args &args_out)
    -> const ast::expr * {
  if (callee.kind == ast::node_kind::index_expr) {
    const auto &index = dynamic_cast<const ast::index_expr &>(callee);
    if (index.object == nullptr || index.index == nullptr) {
      return nullptr;
    }
    args_out.push_back(explicit_generic_arg{.value = index.index.get(),
                                            .span = index.index->span});
    return index.object.get();
  }
  if (callee.kind == ast::node_kind::call_expr) {
    const auto &applied = dynamic_cast<const ast::call_expr &>(callee);
    if (applied.callee == nullptr) {
      return nullptr;
    }
    for (const auto &arg : applied.args) {
      if (arg.value == nullptr || arg.name.has_value()) {
        return nullptr; // a named bracket argument isn't this shape
      }
      args_out.push_back(explicit_generic_arg{.value = arg.value.get(),
                                              .span = arg.value->span});
    }
    return applied.callee.get();
  }
  return nullptr;
}

/// Resolves one explicit *type* argument, written as an expression because
/// that is all the parser could know it might be. Routed back through
/// `resolve_type` on a synthesized `named_type` rather than resolved by
/// hand, so an explicit argument reaches exactly the same builtins, user
/// types, imports, and in-scope parameters an annotation would.
///
/// `already_reported`, when given, is set when resolution failed with a
/// diagnostic (an undefined name), so a caller can skip a second message
/// at the same span.
auto checker::explicit_type_argument(const explicit_generic_arg &arg,
                                     bool *already_reported)
    -> std::optional<type_id> {
  const auto finish = [&](type_id resolved) -> std::optional<type_id> {
    if (already_reported != nullptr && resolved == k_error_type) {
      *already_reported = true;
    }
    return session_.types_.is_unknown(resolved) || resolved == k_error_type
               ? std::nullopt
               : std::optional{resolved};
  };
  // Already a type on the way in (`b.take[int64](5)`): nothing to
  // reinterpret, just resolve it.
  if (arg.type != nullptr) {
    return finish(resolve_type(*arg.type, current_resolve_ctx()));
  }
  if (arg.value == nullptr) {
    return std::nullopt;
  }
  auto named = expr_as_named_type(*arg.value);
  if (named == nullptr) {
    return std::nullopt;
  }
  const auto &type = *named;
  session_.synthesized_types_.push_back(std::move(named));
  return finish(resolve_type(type, current_resolve_ctx()));
}

/// Rewrites an expression that was *meant* as a type into the `named_type`
/// the parser would have built had it been in type position — `int32` from
/// an `ident_expr`, `std.list` from a `module_path_expr`, and
/// `list[int32]` / `map[str, int32]` from the index and application shapes
/// `parse_postfix` gives one and several bracket arguments.
///
/// The grammar already sanctions all of these: `type_arg` admits any
/// `type_expr` and `named_type` admits a nested `"[" type_arg_list "]"`, so
/// `list[int32]` is one well-formed type argument by the grammar's own
/// rules. Only the checker's expression-to-type mapping was narrower, which
/// is what made nested generic type arguments a defect rather than a
/// missing feature (§3.2).
///
/// Hands back ownership rather than a borrowed pointer, so a nested result
/// can be parked straight into its parent's `type_args` and the caller
/// decides what keeps the outermost node alive.
auto checker::expr_as_named_type(const ast::expr &arg)
    -> ast::ptr<ast::named_type> {
  auto named = ast::make<ast::named_type>();
  named->span = arg.span;

  // Reads one bracket argument into `named`'s own type arguments, failing
  // the whole rewrite if it isn't itself expressible as a type.
  const auto take_arg = [&](const ast::expr &inner) -> bool {
    auto nested = expr_as_named_type(inner);
    if (nested == nullptr) {
      return false;
    }
    auto slot = ast::type_arg{};
    slot.span = inner.span;
    slot.value = std::move(nested);
    named->type_args.push_back(std::move(slot));
    return true;
  };

  switch (arg.kind) {
  case ast::node_kind::ident_expr:
    named->path = {dynamic_cast<const ast::ident_expr &>(arg).name};
    break;
  case ast::node_kind::module_path_expr:
    named->path = dynamic_cast<const ast::module_path_expr &>(arg).segments;
    break;
  case ast::node_kind::index_expr: {
    // `list[int32]` — one bracket argument, which the parser spells as an
    // index because in expression position that is all it could be.
    const auto &index = dynamic_cast<const ast::index_expr &>(arg);
    if (index.object == nullptr || index.index == nullptr) {
      return nullptr;
    }
    auto base = expr_as_named_type(*index.object);
    if (base == nullptr || !base->type_args.empty()) {
      return nullptr;
    }
    named->path = base->path;
    if (!take_arg(*index.index)) {
      return nullptr;
    }
    break;
  }
  case ast::node_kind::call_expr: {
    // `map[str, int32]` — several bracket arguments, which the parser
    // spells as an application instead.
    const auto &applied = dynamic_cast<const ast::call_expr &>(arg);
    if (applied.callee == nullptr) {
      return nullptr;
    }
    auto base = expr_as_named_type(*applied.callee);
    if (base == nullptr || !base->type_args.empty()) {
      return nullptr;
    }
    named->path = base->path;
    for (const auto &inner : applied.args) {
      if (inner.value == nullptr || inner.name.has_value() ||
          !take_arg(*inner.value)) {
        return nullptr;
      }
    }
    break;
  }
  default:
    return nullptr;
  }
  return named->path.empty() ? nullptr : std::move(named);
}

/// Folds one explicit *value* argument down to the constant it denotes.
/// `solver_poly` does the folding, under the constants the *enclosing*
/// instance was compiled with — which is what lets a template forward its
/// own value parameter (`f[n + 1]()` inside an instance where `n` is 3
/// instantiates `f[4]`).
auto checker::explicit_value_argument(const ast::expr &arg)
    -> std::optional<int64_t> {
  auto subst = predicate_subst{};
  for (const auto &[name, value] : session_.const_param_values_) {
    subst.values.emplace(name, poly_constant(value));
  }
  const auto poly = solver_poly(arg, subst);
  return poly.has_value() && poly->is_constant() ? std::optional{poly->constant}
                                                 : std::nullopt;
}

/// The type a value parameter's constants live in — `[n: usize]`'s `usize`.
/// `nullopt` when it isn't a number, which is diagnosed by the caller: a
/// value parameter of a sum type (`[S: conn_state]`, the state machines of
/// `spec/dependent-types-design.md` §State Machines in Types) is a
/// compile-time value too, but not one there is a constant to name an
/// instance after or to compile a body against.
auto checker::value_param_underlying(const ast::type_param &param,
                                     const module_members *owner)
    -> std::optional<type_id> {
  const auto underlying =
      param.bound_or_type != nullptr
          ? resolve_type(*param.bound_or_type,
                         resolve_ctx{.module = owner,
                                     .param_bindings = nullptr,
                                     .use_type_param_stack = false,
                                     .quiet = true})
          : session_.types_.usize_type();
  return session_.types_.is_integer(underlying) ? std::optional{underlying}
                                                : std::nullopt;
}

/// Substitutes leaf solutions into a call's solved bindings. A binding
/// still open stays a leaf: solving is not a point of demand (see
/// `when_solution_settles`).
auto checker::settle_bindings(param_subst &bindings) -> void {
  if (leaf_ctxt_.meta_count() == 0) {
    return;
  }
  for (auto &[param, bound] : bindings) {
    bound = leaf_ctxt_.zonk(bound);
  }
}

/// Solves every one of `decl`'s compile-time parameters for one call.
///
/// There are three places an answer can come from, tried in this order
/// because that is the order of decreasing explicitness:
///
///   1. an explicit argument (`zeros[8]()`), which says so outright;
///   2. `match_pattern`'s solution against the argument types, which pins a
///      type parameter (`x: T` given an `int32`) and also a value parameter
///      mentioned in a type (`v: array[int32, n]` given an `array[int32,
///      3]`, whose `n` solves to a `const_value`);
///   3. `match_value_params`' polynomial solution, already computed while
///      checking the arguments, for a value parameter a refinement or
///      length constrains.
///
/// `nullopt` when some parameter is left unanswered by all three — the one
/// way monomorphization fails that is the *caller's* to fix, so it is
/// diagnosed here, naming the parameter and how to pin it down.
auto checker::solve_generic_params(const ast::call_expr &call,
                                   const ast::func_decl &decl,
                                   const module_members *owner,
                                   const value_bindings &solved,
                                   param_subst &type_bindings,
                                   const explicit_generic_args &explicit_args)
    -> std::optional<generic_solution> {
  if (explicit_args.size() > decl.type_params.size()) {
    error_with_help(
        call.span,
        std::format("`{}` takes {} compile-time argument(s), and this call "
                    "gives {}",
                    decl.name, decl.type_params.size(), explicit_args.size()),
        "too many arguments in brackets",
        std::format("The brackets after `{}` name its compile-time "
                    "parameters, in declaration order. Drop the extra "
                    "argument(s), or leave the brackets off entirely and "
                    "let the call's arguments determine them.",
                    decl.name));
    return std::nullopt;
  }

  // A parameter solved to a leaf that is still open keeps the leaf: the
  // call's type is known in terms of it, and only naming the instance has
  // to wait for it (`when_solution_settles`). A later `let c: int64 = r`
  // is still free to say what the `5` in `let r = pick(5)` is.
  settle_bindings(type_bindings);

  auto solution = generic_solution{};
  for (size_t i = 0; i < decl.type_params.size(); ++i) {
    const auto &param = decl.type_params[i];
    const auto *explicit_arg =
        i < explicit_args.size() ? &explicit_args[i] : nullptr;

    if (param.is_value_param) {
      const auto underlying = value_param_underlying(param, owner);
      if (!underlying.has_value()) {
        report_non_numeric_value_param(call, decl, param);
        return std::nullopt;
      }
      if (explicit_arg != nullptr) {
        // Written in type position (`.m[...]`), where the grammar parses a
        // `type_arg_list` — a type never folds to a constant, so this
        // parameter simply cannot be answered in that spelling.
        const auto constant =
            explicit_arg->value != nullptr
                ? explicit_value_argument(*explicit_arg->value)
                : std::optional<int64_t>{};
        // `climb[n + 1]` inside a body generic over `n`: the argument is
        // written in the caller's own value parameters, a symbolic answer
        // like any other a generic body's call has.
        if (!constant.has_value() && explicit_arg->value != nullptr) {
          const auto symbolic =
              resolve_length_arg(*explicit_arg->value, current_resolve_ctx());
          if (session_.types_.entry(symbolic).kind ==
              type_kind::symbolic_value_kind) {
            solution.const_slots.emplace(param.name, symbolic);
            solution.suffix += "$?";
            continue;
          }
        }
        if (!constant.has_value()) {
          error_with_help(
              explicit_arg->span,
              std::format("`{}`'s compile-time argument `{}` is not a "
                          "constant",
                          decl.name, param.name),
              "this has to be a definite number here",
              std::format("`{}` is compiled once per constant `{}`, so the "
                          "argument in brackets has to fold to a number at "
                          "compile time — a literal, or arithmetic over "
                          "literals and value parameters already fixed.",
                          decl.name, param.name));
          return std::nullopt;
        }
        bind_generic_constant(solution, param, *underlying, *constant);
        continue;
      }
      const auto own_var = value_var(param_id(param));
      if (const auto found = solved.find(own_var);
          found != solved.end() && found->second.is_constant()) {
        bind_generic_constant(solution, param, *underlying,
                              found->second.constant);
        continue;
      }
      // Solved to a polynomial over the caller's own value parameters
      // (`concat(v, w)` inside a body generic over `n` and `m`): an answer
      // written in symbols, which is what a generic body's call has. It
      // names no instance — `solution_mentions_template_param` keeps it
      // from being compiled — but it solves the call.
      if (const auto found = solved.find(own_var);
          found != solved.end() &&
          std::ranges::all_of(found->second.terms,
                              [&](const auto &term) -> auto {
                                const auto own = poly_var_identity(term.var);
                                return own.has_value() && is_rigid_param(*own);
                              })) {
        solution.const_slots.emplace(
            param.name,
            session_.types_.symbolic_value(*underlying, found->second));
        solution.suffix += "$?";
        continue;
      }
      if (!session_.mismatched_argument_calls_.contains(&call)) {
        report_unsolved_value_param(call, decl, param);
      }
      return std::nullopt;
    }

    if (explicit_arg != nullptr) {
      bool already_reported = false;
      const auto resolved =
          explicit_type_argument(*explicit_arg, &already_reported);
      if (!resolved.has_value()) {
        if (already_reported) {
          return std::nullopt;
        }
        error_with_help(
            explicit_arg->span,
            std::format("`{}`'s compile-time argument `{}` is not a type "
                        "this call can name",
                        decl.name, param.name),
            "expected a type here",
            std::format("`{}`'s `{}` is a type parameter, so the argument "
                        "in brackets has to name a type that is in scope.",
                        decl.name, param.name));
        return std::nullopt;
      }
      // The brackets win over the arguments — they are the more explicit
      // of the two — but only after saying so when the two disagree. The
      // binding is written back so the caller's own substitution into the
      // declared return type sees the type the call was *actually*
      // instantiated at, rather than the one unification happened to find.
      if (const auto found = type_bindings.find(param_id(param));
          found != type_bindings.end() && found->second != *resolved &&
          !session_.types_.is_unknown(found->second) &&
          !mentions_abstract_type(found->second)) {
        error_with_help(
            explicit_arg->span,
            std::format("`{}`'s `{}` is given as `{}` in brackets, but the "
                        "arguments make it `{}`",
                        decl.name, param.name,
                        session_.types_.display(*resolved),
                        session_.types_.display(found->second)),
            std::format("this says `{}` is `{}`", param.name,
                        session_.types_.display(*resolved)),
            std::format("A compile-time argument in brackets pins `{}` "
                        "down, so the arguments have to agree with it. "
                        "Either pass arguments that are `{}`, or drop the "
                        "brackets and let the arguments decide.",
                        param.name, session_.types_.display(*resolved)));
        return std::nullopt;
      }
      type_bindings[param_id(param)] = *resolved;
      bind_generic_type(solution, param, *resolved);
      continue;
    }
    const auto found = type_bindings.find(param_id(param));
    if (found != type_bindings.end() && mentions_type_var(found->second) &&
        !mentions_abstract_type(found->second,
                                /*leaves_are_answers=*/true)) {
      bind_generic_type(solution, param, found->second);
      continue;
    }
    if (found != type_bindings.end() &&
        !session_.types_.is_unknown(found->second) &&
        mentions_abstract_type(found->second)) {
      // The arguments *did* determine `T` — as another type parameter that
      // is still abstract. That can only happen while checking a template:
      // a generic `extend`/`impl` block's method body, or a generic
      // function's, before monomorphization has picked anything concrete.
      // There is no instance to compile here and none is wanted; the
      // instantiated copy reaches this same code with real types.
      //
      // Reporting it blamed the user for the compiler looking a phase too
      // early, and pointed them at a fix ("give it outright in brackets")
      // that does not apply — the same mistake `check_impl_generic_method_
      // call`'s `mentions_type_param` bail-out already avoids for *method*
      // calls. A free generic call in the same body had no such guard, so
      // `extend[T] vec[T]` could not call any generic function at all.
      return std::nullopt;
    }
    // A template calling itself: the callee's `T` is the caller's own,
    // rigid in this body, and matching it against itself binds nothing.
    // The answer is that same `T`.
    if (found == type_bindings.end()) {
      const auto own = session_.types_.type_param(
          param.name, param.higher_kinded_arity, &param);
      if (is_rigid_param(own)) {
        bind_generic_type(solution, param, own);
        continue;
      }
    }
    if (found == type_bindings.end() ||
        session_.types_.is_unknown(found->second)) {
      report_unsolved_type_param(call, decl, param);
      return std::nullopt;
    }
    bind_generic_type(solution, param, found->second);
  }
  return solution;
}

/// Solves the type parameters `decl`'s *arguments* determine, by unifying
/// each declared parameter type against the type the argument was inferred
/// to have — an `x: T` parameter given an `int32` argument solves `T :=
/// int32`, and a `v: array[int32, n]` parameter given an `array[int32, 3]`
/// solves `n := 3`. Shared by the free-function and receiver-call paths,
/// which differ only in which parameters they hand it.
///
/// `bind_open` answers a parameter with an argument's still-open leaf when
/// nothing settled answers it. Solving never defaults a leaf: a call is not
/// a point of demand for its arguments' types, only for the instance it
/// names, and naming the instance waits for the leaves to settle
/// (`when_solution_settles`). `let r = pick(5)` followed by
/// `let c: int64 = r` makes the `5` an `int64`.
auto checker::solve_from_argument_types(
    const ast::call_expr &call, const std::vector<fn_param_info> &params,
    param_subst &bindings, const ast::expr *ufcs_receiver, bool bind_open)
    -> void {
  const auto mapping = session_.call_argument_mappings_.find(&call);
  if (mapping == session_.call_argument_mappings_.end()) {
    return;
  }
  const auto &args_by_param = mapping->second.args_by_param;
  auto arg_types = std::vector<type_id>(params.size(), k_unknown_type);
  auto open_leaf = false;
  for (size_t i = 0; i < params.size(); ++i) {
    const auto *argument = ufcs_argument_for(i, args_by_param, ufcs_receiver);
    if (argument == nullptr) {
      continue;
    }
    const auto found = session_.node_types_.find(argument);
    if (found == session_.node_types_.end()) {
      continue;
    }
    // Through the leaf store: a `list[?a]` whose `?a` a `push` already
    // pinned is a `list[int32]` here. Reading the raw recorded type binds
    // `T := ?a`, a parameter recorded as *solved to a variable*, which the
    // binding map cannot tell from a real answer.
    arg_types[i] = settle(found->second);
    // A parameter with structure (`xs: list[T]`) is matched by the
    // argument's head, so an argument whose head is still a leaf (`[5, 6]`
    // before its literal is wired) owes its shape here. A bare `x: T`
    // takes the leaf as it is.
    if (session_.types_.entry(strip_refs(params[i].type)).kind !=
        type_kind::type_param_kind) {
      arg_types[i] = demand_shape(arg_types[i]);
    }
    auto seen = std::unordered_set<type_id>{};
    open_leaf = open_leaf || mentions_type_var(arg_types[i], seen);
  }
  // Arguments with a settled type answer first, and an argument still
  // carrying a leaf then learns from them: in `pick(1, x)` with `x: int64`
  // the literal is an `int64`, because `x` says what `T` is. Defaulting the
  // literal to `int32` first would have `T` solved from it instead — and
  // `x` then either rejected for a conflict the user never wrote, or, before
  // every argument was re-checked against the solution, passed silently as
  // the wrong width.
  if (open_leaf) {
    auto concrete = param_subst{};
    for (size_t i = 0; i < params.size(); ++i) {
      auto seen = std::unordered_set<type_id>{};
      if (!session_.types_.is_unknown(arg_types[i]) &&
          !mentions_type_var(arg_types[i], seen)) {
        match_params(params[i].type, arg_types[i], concrete);
      }
    }
    for (size_t i = 0; i < params.size(); ++i) {
      auto seen = std::unordered_set<type_id>{};
      if (!mentions_type_var(arg_types[i], seen)) {
        continue;
      }
      const auto expected = substitute_solved(params[i].type, concrete);
      if (!mentions_type_param(expected)) {
        solve_leaves(expected, arg_types[i]);
        arg_types[i] = settle(arg_types[i]);
      }
    }
  }
  for (size_t i = 0; i < params.size(); ++i) {
    auto seen = std::unordered_set<type_id>{};
    if (session_.types_.is_unknown(arg_types[i]) ||
        mentions_type_var(arg_types[i], seen)) {
      continue;
    }
    match_params(params[i].type, arg_types[i], bindings);
  }
  // An argument still open answers too, with the leaf itself.
  // `fold(0, (n, w) => n + w.len())` checks its lambda against `fn(A, T)`,
  // and `A` answered with the `0`'s leaf lets the body's `n + usize` say
  // what the `0` is; answered with nothing, `A` stays abstract; answered
  // with the default, the `0` is an `int32` before the body is read.
  // Settled arguments answered first, so a leaf never displaces a real
  // type.
  if (bind_open) {
    for (size_t i = 0; i < params.size(); ++i) {
      auto seen = std::unordered_set<type_id>{};
      if (arg_types[i] == k_unknown_type ||
          !mentions_type_var(arg_types[i], seen)) {
        continue;
      }
      const auto matched =
          infer::match_pattern(session_.types_, params[i].type, arg_types[i]);
      for (const auto &[param, solved] : matched.bindings) {
        bindings.try_emplace(param, solved);
      }
    }
  }
}

/// The bindings available *partway* through checking a call's arguments —
/// everything the brackets, the already-checked arguments, and the declared
/// bounds can answer between them. Used only to substitute into a deferred
/// lambda's expected type; the authoritative solution is still
/// `solve_generic_params`, which runs once every argument has been seen and
/// is the only one that reports an unsolved parameter.
///
/// Quiet by construction: nothing here diagnoses. A parameter this cannot
/// answer simply stays abstract, and the lambda is checked against what is
/// known — which is exactly what happened for every call before deferral
/// existed.
auto checker::preliminary_type_bindings(
    const ast::call_expr &call, const std::vector<fn_param_info> &params,
    const generic_call_context &generic) -> param_subst {
  auto bindings = generic.seed != nullptr ? *generic.seed : param_subst{};
  const auto &decl = *generic.decl;
  if (generic.explicit_args != nullptr) {
    for (size_t i = 0;
         i < generic.explicit_args->size() && i < decl.type_params.size();
         ++i) {
      const auto &param = decl.type_params[i];
      if (param.is_value_param || param.name.empty()) {
        continue;
      }
      if (const auto resolved =
              explicit_type_argument((*generic.explicit_args)[i])) {
        bindings.emplace(param_id(param), *resolved);
      }
    }
  }
  solve_from_argument_types(call, params, bindings, generic.ufcs_receiver,
                            /*bind_open=*/true);
  solve_from_bounds(decl, generic.owner, bindings);
  return bindings;
}

/// The argument expression supplied for declared parameter `i`.
///
/// A UFCS call (`recv.f(a)` reaching `f(recv, a)`) writes one fewer
/// argument than `f` declares parameters: the receiver fills parameter 0
/// and lives outside `args_by_param`, which is built against the *written*
/// arguments only. Everything that walks parameters against arguments —
/// generic solving, precondition substitution — has to shift by one here,
/// or it reads each argument against the wrong parameter. `nullptr` when
/// no argument reaches this parameter (a default, or past the end).
auto checker::ufcs_argument_for(
    size_t index, const std::vector<const ast::expr *> &args_by_param,
    const ast::expr *ufcs_receiver) -> const ast::expr * {
  if (ufcs_receiver == nullptr) {
    return index < args_by_param.size() ? args_by_param[index] : nullptr;
  }
  if (index == 0) {
    return ufcs_receiver;
  }
  return index - 1 < args_by_param.size() ? args_by_param[index - 1] : nullptr;
}

/// Re-checks each argument of a generic call against its parameter type
/// *under the final solution*.
///
/// Arguments are first checked against the declared parameter types while
/// `T` is still abstract, and an abstract `T` accepts anything. Solving then
/// takes the first answer each parameter gets (`match_params`), so in
/// `pick(1, "x")` against `def pick[T](a: T, b: T)` the `str` was never
/// compared with anything: `T := int32` came from `a`, and `b` went through
/// unexamined. This is the comparison that was missing.
///
/// Only an argument the first pass accepted is reported here, so a
/// structural mismatch (`list[T]` given a `str`) that pass already
/// diagnosed is not reported twice.
///
/// A disagreement the unifier can explain better is reported first
/// (`report_argument_conflicts`), and the arguments it covers are not
/// compared again. `fixed_params` are the parameters the brackets answered;
/// their arguments are simply compared with that answer.
///
/// Returns whether such a conflict was reported. The solution then holds
/// whichever answer came first, which may be the one blame just called the
/// mistake, so a caller should not type the call's result from it.
auto checker::check_args_against_solution(
    const ast::call_expr &call, const std::vector<fn_param_info> &params,
    const param_subst &bindings, std::string_view callee_name,
    const ast::expr *ufcs_receiver,
    const std::unordered_set<type_id> &fixed_params) -> bool {
  const auto mapping = session_.call_argument_mappings_.find(&call);
  if (mapping == session_.call_argument_mappings_.end()) {
    return false;
  }
  struct mismatch {
    const ast::expr *argument = nullptr;
    type_id declared = k_unknown_type;
    type_id expected = k_unknown_type;
    type_id found = k_unknown_type;
  };
  auto mismatches = std::vector<mismatch>{};
  for (size_t i = 0; i < params.size(); ++i) {
    const auto *argument =
        ufcs_argument_for(i, mapping->second.args_by_param, ufcs_receiver);
    if (argument == nullptr) {
      continue;
    }
    const auto recorded = session_.node_types_.find(argument);
    if (recorded == session_.node_types_.end()) {
      continue;
    }
    const auto declared = settle(params[i].type);
    const auto expected = settle(substitute_solved(declared, bindings));
    const auto found = settle(recorded->second);
    // Skipped: a parameter the solution did not touch; one still written in
    // the callee's own unsolved parameters (nothing to compare yet); and
    // one whose *shape* already failed against the argument, which the
    // argument check reported. The caller's own parameters are answers,
    // not gaps, so they are compared like any other type.
    if (expected == declared || open_foreign_params(expected) != expected ||
        session_.types_.is_unknown(found) || found == k_error_type ||
        !session_.types_.compatible(open_foreign_params(declared), found) ||
        agrees(expected, found)) {
      continue;
    }
    mismatches.push_back(mismatch{.argument = argument,
                                  .declared = declared,
                                  .expected = expected,
                                  .found = found});
  }
  // Only a call that is wrong somewhere is matched again: the matcher can
  // intern, and a correct call must leave no trail in the type table.
  if (mismatches.empty()) {
    return false;
  }
  const auto reported = report_argument_conflicts(params, bindings, callee_name,
                                                  mapping->second.args_by_param,
                                                  ufcs_receiver, fixed_params);
  for (const auto &[argument, declared, expected, found] : mismatches) {
    if (reported.contains(argument)) {
      continue;
    }
    const auto &declared_entry = session_.types_.entry(declared);
    if (declared_entry.kind != type_kind::type_param_kind) {
      type_mismatch(argument->span, expected, found, "for this argument",
                    argument);
      continue;
    }
    const auto param_name = std::string(declared_entry.name);
    const auto bracketed = fixed_params.contains(declared);
    error_with_help(
        argument->span,
        std::format("conflicting types for `{}` in this call to `{}`",
                    param_name, callee_name),
        std::format("this is `{}`, but `{}` is already `{}`",
                    session_.types_.display(found), param_name,
                    session_.types_.display(expected)),
        std::format(
            "`{}` stands for one type throughout a call to `{}`, and {} "
            "fixed it as `{}`. Every argument declared as `{}` has to be "
            "that same type: convert this one (`{}(...)`), or change {}.",
            param_name, callee_name,
            bracketed ? "the brackets" : "another argument",
            session_.types_.display(expected), param_name,
            session_.types_.display(expected),
            bracketed ? "the brackets" : "the other"));
  }
  return !reported.empty();
}

/// Whether two answers for one type parameter are the same answer as far
/// as a call is concerned — a refinement and its base, a borrow and its
/// referent where the language coerces between them.
auto checker::interchangeable(type_id a, type_id b) const -> bool {
  return session_.types_.compatible(a, b) || session_.types_.compatible(b, a);
}

/// Reports the disagreements among a generic call's arguments that the
/// unifier can explain, and returns the arguments it reported.
///
/// Solving takes the first answer each parameter gets, which makes the
/// first argument the authority by accident of position. In
/// `pick3(x, s, s)` that blamed both `s` for disagreeing with `x`. Every
/// argument's answer is collected here instead, and blame
/// (`infer::explain_conflict`) points at the minority, showing the
/// majority as the evidence — or, on an even split, shows both and
/// blames neither.
///
/// The second disagreement is one the unifier finds inside a single
/// argument: `pair[T, T]` given a `pair[int32, str]`. Matching that
/// argument fails, and the failure says which two parts clashed.
///
/// Only parameters the arguments answered are considered. One the brackets
/// fixed, or one a bound answered over the arguments, has an authority of
/// its own, and each argument is compared with it by the caller.
auto checker::report_argument_conflicts(
    const std::vector<fn_param_info> &params, const param_subst &bindings,
    std::string_view callee_name,
    const std::vector<const ast::expr *> &args_by_param,
    const ast::expr *ufcs_receiver,
    const std::unordered_set<type_id> &fixed_params)
    -> std::unordered_set<const ast::expr *> {
  struct argument_answer {
    const ast::expr *argument = nullptr;
    type_id solved = k_unknown_type;
  };
  auto reported = std::unordered_set<const ast::expr *>{};
  // In order of first appearance, so the diagnostics follow the source.
  auto answers =
      std::vector<std::pair<type_id, std::vector<argument_answer>>>{};
  for (size_t i = 0; i < params.size(); ++i) {
    const auto *argument = ufcs_argument_for(i, args_by_param, ufcs_receiver);
    if (argument == nullptr) {
      continue;
    }
    const auto recorded = session_.node_types_.find(argument);
    if (recorded == session_.node_types_.end()) {
      continue;
    }
    const auto declared = settle(params[i].type);
    const auto found = settle(recorded->second);
    // The same skips as the comparison that follows: nothing to match, or
    // a shape the argument check already rejected.
    if (session_.types_.is_unknown(found) || found == k_error_type ||
        !session_.types_.compatible(open_foreign_params(declared), found)) {
      continue;
    }
    const auto matched = infer::match_pattern(session_.types_, declared, found);
    if (matched.failure.has_value() &&
        report_split_argument(*argument, declared, found, matched,
                              callee_name)) {
      reported.insert(argument);
      continue;
    }
    const auto record = [&](type_id param, type_id solved) -> void {
      if (fixed_params.contains(param) || session_.types_.is_unknown(solved) ||
          solved == k_error_type) {
        return;
      }
      auto slot = std::ranges::find_if(answers, [&](const auto &entry) -> bool {
        return entry.first == param;
      });
      if (slot == answers.end()) {
        answers.emplace_back(param, std::vector<argument_answer>{});
        slot = std::prev(answers.end());
      }
      slot->second.push_back(
          argument_answer{.argument = argument, .solved = solved});
    };
    for (const auto &[param, solved] : matched.bindings) {
      record(param, solved);
    }
    // A value parameter's answer is interned as the value slot it would
    // fill, so `n := 3` and `n := 2` are two answers the same way `int32`
    // and `str` are.
    for (const auto &[var, value] : matched.values) {
      if (const auto param = poly_var_identity(var); param.has_value()) {
        record(*param, session_.types_.symbolic_value(
                           session_.types_.usize_type(), value));
      }
    }
  }

  for (const auto &[param, given] : answers) {
    // One representative per group of interchangeable answers, so blame
    // counts `percent` and `int32` as the same vote.
    auto groups = std::vector<type_id>{};
    const auto group_of = [&](type_id solved) -> type_id {
      for (const auto representative : groups) {
        if (interchangeable(representative, solved)) {
          return representative;
        }
      }
      groups.push_back(solved);
      return solved;
    };
    for (const auto &answer : given) {
      static_cast<void>(group_of(answer.solved));
    }
    if (groups.size() < 2) {
      continue;
    }
    const auto &param_entry = session_.types_.entry(param);
    const auto is_value = param_entry.is_value_param;
    // Answered by something other than these arguments — a bound's ground
    // truth replaces an argument's guess — so none of them is the authority.
    // Nothing but the arguments and the brackets answers a value parameter.
    const auto chosen = bindings.find(param);
    if (!is_value &&
        (chosen == bindings.end() ||
         std::ranges::none_of(groups, [&](type_id representative) -> bool {
           return interchangeable(representative, settle(chosen->second));
         }))) {
      continue;
    }

    const auto param_name = std::string(param_entry.name);
    auto ctx = infer::infer_ctxt{session_.types_};
    static_cast<void>(
        ctx.adopt(param,
                  is_value                     ? infer::meta_sort::value_sort
                  : param_entry.ctor_arity > 0 ? infer::meta_sort::ctor_sort
                                               : infer::meta_sort::type_sort,
                  param_entry.ctor_arity, param_name, source_location{}));
    auto graph = infer::constraint_graph{ctx};
    // "this argument is `2`" would misname an array as its length, so a
    // value's label says which part of the argument it is.
    const auto reason = is_value
                            ? std::format("this argument's `{}`", param_name)
                            : std::string{"this argument"};
    for (const auto &answer : given) {
      const auto why = ctx.add_cause(
          infer::cause{.where = source_location{.file_id = file_.file_id_,
                                                .span = answer.argument->span},
                       .reason = reason,
                       .expected_desc = {},
                       .found_desc = {},
                       .parent = infer::k_no_cause});
      static_cast<void>(graph.record(param, group_of(answer.solved), why));
      reported.insert(answer.argument);
    }
    const auto report =
        infer::explain_conflict(graph, ctx, session_.types_, param);
    if (!report.has_value()) {
      continue;
    }
    const auto tie = graph.outliers(param).empty();
    const auto *sort = is_value ? "value" : "type";
    auto diag =
        diagnostic(diagnostic_level::error,
                   std::format("conflicting {}s for `{}` in this call to `{}`",
                               sort, param_name, callee_name),
                   report->primary.file_id);
    diag.with_label(report->primary.span, report->label);
    for (const auto &note : report->notes) {
      diag.with_secondary_label(note.where.span, note.message);
    }
    diag.with_help(
        tie ? std::format(
                  "`{}` stands for one {} throughout a call to `{}`, and "
                  "these arguments give it two, equally often, so the "
                  "compiler will not guess which was meant. Make them "
                  "agree, or give `{}` explicitly to say which was "
                  "intended.",
                  param_name, sort, callee_name, param_name)
            : std::format("`{}` stands for one {} throughout a call to "
                          "`{}`. {}",
                          param_name, sort, callee_name, report->help));
    emit_diag(diag);
    mark_error();
  }
  return reported;
}

/// Reports an argument that disagrees with itself about a type parameter —
/// `pair[T, T]` given `pair[int32, str]` — from the unifier's own failure,
/// and says whether it did.
///
/// The failure names the two parts that clashed; this finds the parameter
/// that had been solved to one of them. Any other failure (a mutability
/// mismatch, say) is left to the ordinary comparison.
auto checker::report_split_argument(const ast::expr &argument, type_id declared,
                                    type_id found,
                                    const infer::rigid_match_result &matched,
                                    std::string_view callee_name) -> bool {
  const auto &failure = *matched.failure;
  if (failure.failure != infer::unify_failure::mismatch) {
    return false;
  }
  const auto clash =
      std::ranges::find_if(matched.bindings, [&](const auto &binding) -> bool {
        return binding.second == failure.expected_part ||
               binding.second == failure.found_part;
      });
  if (clash == matched.bindings.end()) {
    return false;
  }
  const auto param_name = std::string(session_.types_.entry(clash->first).name);
  const auto first = clash->second;
  const auto second = first == failure.expected_part ? failure.found_part
                                                     : failure.expected_part;
  error_with_help(
      argument.span,
      std::format("conflicting types for `{}` in this call to `{}`", param_name,
                  callee_name),
      std::format("`{}` would be both `{}` and `{}` in this argument",
                  param_name, session_.types_.display(first),
                  session_.types_.display(second)),
      std::format("`{}` declares this parameter as `{}`, and `{}` stands for "
                  "one type throughout the call. This `{}` puts `{}` in one "
                  "place and `{}` in another, so no single `{}` fits. Make "
                  "the parts agree.",
                  callee_name, session_.types_.display(declared), param_name,
                  session_.types_.display(found),
                  session_.types_.display(first),
                  session_.types_.display(second), param_name));
  return true;
}

/// Solves whatever the arguments left open from the type the call site
/// *expects* — the last of the four sources, and the only one that can
/// reach a parameter mentioned nowhere but the return type (`def
/// collect[I, C](self: I) -> C`).
///
/// Deliberately last, and deliberately non-overriding: `match_params`
/// declines to rebind a name, so "arguments win over context" falls out of
/// call order rather than needing a rule of its own. That is what makes
/// this safe to add without auditing existing call sites — it can turn an
/// unsolved call into a solved one, never a solved call into a differently
/// solved one, so `let x: int64 = take(5i32)` still reports the mismatch it
/// reports today instead of silently re-solving `T := int64`.
///
/// See `spec/generic-inference-design.md` §2.
auto checker::solve_from_expected_type(
    const ast::call_expr &call, const ast::func_decl &decl,
    const module_members *owner, param_subst &bindings,
    const explicit_generic_args &explicit_args) -> void {
  if (decl.return_type == nullptr) {
    return;
  }
  const auto found = session_.call_expected_types_.find(&call);
  if (found == session_.call_expected_types_.end()) {
    return;
  }
  const auto expected = session_.types_.strip_refinement(found->second);
  if (session_.types_.is_unknown(expected) || expected == k_error_type) {
    return;
  }
  // Resolved in the *template's* own terms — `signature_return_type` binds
  // each type parameter to its abstract `type_param`, which is exactly the
  // form `match_params` knows how to solve, so `-> C` comes back as a
  // bindable `C` rather than as `unknown`.
  //
  // Solved aside and merged in, rather than unified straight into
  // `bindings`, so a parameter the brackets already answer never picks up
  // a competing answer from context. Without that, `solve_generic_params`
  // would see a hint-derived binding where it expects an argument-derived
  // one and report a conflict the user's arguments never had — the hint is
  // a fallback, and a fallback that can raise an error is not one.
  auto from_expected = param_subst{};
  match_params(signature_return_type(decl, owner), expected, from_expected);
  for (size_t i = 0; i < decl.type_params.size(); ++i) {
    const auto &name = decl.type_params[i].name;
    const auto param = param_id(decl.type_params[i]);
    if (name.empty() || i < explicit_args.size() || bindings.contains(param)) {
      continue;
    }
    if (const auto solved = from_expected.find(param);
        solved != from_expected.end()) {
      bindings.emplace(param, solved->second);
      session_.expected_solved_params_[&call].push_back(
          std::format("`{}` was solved to `{}` from the type expected here",
                      name, session_.types_.display(solved->second)));
    }
  }
}

/// Solves one call's compile-time parameters — the one entry point for
/// every generic call, free function or method alike.
///
/// `bindings` arrives holding what the arguments (and, for a method, the
/// receiver) already said, since only the caller knows which parameters
/// its arguments reach. The remaining sources are added here in order of
/// decreasing authority, each unable to overturn an earlier one: the
/// declared bounds (`I` answers `T` through `where I: iterator[T]`), then
/// the type the call site expects; the solution is then read back with
/// the brackets taking precedence (`solve_generic_params`). Every source
/// feeds the one unifier (`match_params`).
auto checker::solve_call(const ast::call_expr &call, const ast::func_decl &decl,
                         const module_members *owner,
                         const value_bindings &solved, param_subst &bindings,
                         const explicit_generic_args &explicit_args)
    -> std::optional<generic_solution> {
  solve_from_bounds(decl, owner, bindings);
  solve_from_expected_type(call, decl, owner, bindings, explicit_args);
  return solve_generic_params(call, decl, owner, solved, bindings,
                              explicit_args);
}

/// Monomorphizes `decl` for this call, returning the call's result type
/// under the instantiation (`-> array[int32, n]` becomes `array[int32, 3]`,
/// `-> T` becomes `int32`) — or `nullopt` when the call cannot be
/// monomorphized, having said why.
///
/// The instance is registered as this call's `resolved_callee`, so lowering
/// emits a call to `get$3` with no idea that a template was ever involved.
/// Solves a call to a generic function and checks it against the solution:
/// every argument, and every bound the callee declares (phase 9, rule 3).
/// Shared by the instance path and a template's own calls, which solve the
/// same way and differ only in whether an instance is made.
auto checker::solve_generic_call(
    const ast::call_expr &call, const ast::func_decl &decl,
    const module_members *owner, file_id_type decl_file,
    const value_bindings &solved, const std::vector<fn_param_info> &params,
    const explicit_generic_args &explicit_args, const ast::expr *ufcs_receiver)
    -> std::optional<generic_solution> {
  auto type_bindings = param_subst{};
  solve_from_argument_types(call, params, type_bindings, ufcs_receiver,
                            /*bind_open=*/true);
  // Between the arguments and the expected type, because a bound is solved
  // *from* an argument-derived binding (`I` answers `T` via
  // `where I: iterator[T]`) and should still lose to an explicit annotation
  // the way every argument-derived binding does. `def sum[I, T](it: I)`
  // has no argument mentioning `T` at all, so without this the terminal of
  // every adapter chain is unsolvable.
  auto solution =
      solve_call(call, decl, owner, solved, type_bindings, explicit_args);
  if (!solution.has_value()) {
    return std::nullopt;
  }
  auto bracketed = std::unordered_set<type_id>{};
  for (size_t i = 0; i < explicit_args.size() && i < decl.type_params.size();
       ++i) {
    bracketed.insert(param_id(decl.type_params[i]));
  }
  if (check_args_against_solution(call, params,
                                  solution_subst(decl.type_params, *solution),
                                  decl.name, ufcs_receiver, bracketed)) {
    return std::nullopt;
  }
  // A bound on an answer that is still an open leaf is a decision that
  // waits with the instance (`when_solution_settles`): `list_iter[?a]` is
  // not yet an `iterator[&int32]`, nor yet refused as one.
  solution->bounds_hold =
      solution_is_open(*solution) ||
      check_call_bounds(call, decl, owner, decl_file, *solution);
  return solution;
}

/// The callee's parameter and result types under `solution`, read in the
/// file that declared them. The result is `k_unknown_type` when the callee
/// leaves its return type to inference.
auto checker::solved_signature(const ast::func_decl &decl,
                               const module_members *owner,
                               file_id_type decl_file,
                               const generic_solution &solution)
    -> std::pair<std::vector<type_id>, type_id> {
  auto bindings = solution.const_slots;
  bindings.insert(solution.type_slots.begin(), solution.type_slots.end());
  const auto ctx = resolve_ctx{.module = owner,
                               .param_bindings = &bindings,
                               .use_type_param_stack = false,
                               .quiet = true,
                               .existential_allowed = true};
  const auto saved_signature_file = file_.file_id_;
  file_.file_id_ = decl_file;
  auto param_types = std::vector<type_id>{};
  for (const auto &param : decl.params) {
    param_types.push_back(param.type_annotation != nullptr
                              ? resolve_type(*param.type_annotation, ctx)
                              : k_unknown_type);
  }
  const auto result = decl.return_type != nullptr
                          ? resolve_type(*decl.return_type, ctx)
                          : k_unknown_type;
  file_.file_id_ = saved_signature_file;
  return {std::move(param_types), result};
}

/// Whether `id` mentions a type parameter of the body being checked.
auto checker::mentions_rigid_param(type_id id) -> bool {
  if (is_rigid_param(id)) {
    return true;
  }
  const auto entry =
      session_.types_.entry(id); // copy: callers may intern after
  for (const auto arg : entry.args) {
    if (mentions_rigid_param(arg)) {
      return true;
    }
  }
  if (entry.kind == type_kind::fn_kind || entry.kind == type_kind::ref_kind ||
      entry.kind == type_kind::ptr_kind ||
      entry.kind == type_kind::array_kind) {
    return entry.result != k_unknown_type && mentions_rigid_param(entry.result);
  }
  return false;
}

/// `id` with every type parameter that is *not* the body's own replaced by
/// `unknown`. At a call, such a parameter can only be the callee's, showing
/// through its signature before the call has solved it; it stands for
/// nothing yet, so it matches anything until the solution says otherwise.
auto checker::open_foreign_params(type_id id) -> type_id {
  auto foreign = std::unordered_map<type_id, type_id>{};
  const auto collect = [&](auto &self, type_id at) -> void {
    const auto entry =
        session_.types_.entry(at); // copy: collecting never interns
    if (entry.kind == type_kind::type_param_kind) {
      if (!is_rigid_param(at)) {
        foreign.emplace(at, k_unknown_type);
      }
      return;
    }
    for (const auto arg : entry.args) {
      self(self, arg);
    }
    if ((entry.kind == type_kind::fn_kind ||
         entry.kind == type_kind::ref_kind ||
         entry.kind == type_kind::ptr_kind ||
         entry.kind == type_kind::array_kind) &&
        entry.result != k_unknown_type) {
      self(self, entry.result);
    }
  };
  collect(collect, id);
  return substitute_solved(id, foreign);
}

/// Whether any type a call solved to is written in the body's own
/// parameters.
auto checker::solution_mentions_template_param(const generic_solution &solution)
    -> bool {
  const auto open = [&](const auto &slot) -> auto {
    return mentions_template_param(slot.second);
  };
  return std::ranges::any_of(solution.type_slots, open) ||
         std::ranges::any_of(solution.const_slots, open);
}

// ==========================================================================
//  Higher-kinded-trait method monomorphization
//  (`spec/higher-kinded-traits-implementation-plan.md` phase 5)
//
//  A method an `impl monad for option` provides is generic over its own
//  type parameters (`def pure[A](a: A) -> option[A]`), and — like every
//  function generic over *types* — has no compiled form of its own
//  (`hir::lower_function` refuses type-generic declarations). So each call
//  whose receiver/arguments pin every parameter down to a concrete type
//  gets an instance: the declaration cloned, re-checked under that
//  substitution (`session_.type_param_slots_`), named `option::pure$int32`, and
//  registered exactly like a const-generic instance so lowering and both
//  backends see only fully-applied types (Strategy #4: HKTs erase at
//  monomorphization).
// ==========================================================================

/// Renders `id` in symbol-safe characters for an instance name —
/// `option[int32]` becomes `option_int32_`. Purely a naming aid; identity
/// is the cache key built from the solved `type_id`s themselves.
auto checker::mangle_type_for_instance(type_id id) -> std::string {
  auto out = std::string{};
  for (const auto c : session_.types_.display(id)) {
    out += std::isalnum(static_cast<unsigned char>(c)) != 0 ? c : '_';
  }
  return out;
}

/// Monomorphizes an impl/extend-provided generic method for one call's
/// solved bindings, returning the checked instance — reused per (method,
/// solution) — or `nullptr` when a parameter is unsolved or the body can't
/// be cloned (both diagnosed here).
///
/// This is `instantiate_generic_function` with a receiver: the same
/// solution, the same instance builder, the same registry. It differs only
/// in the instance's name, which carries the impl target
/// (`option::pure$int32`, `vec::at$3`) because that is how `hir::lower_impl_
/// associated_functions` names an impl member, and in taking a
/// `method_entry` — which records no declaring file, and may carry a trait
/// default's own fixed bindings.
///
/// A value parameter is solved here exactly as a type parameter is: `n` in
/// a `def at[n: usize](self, i: index[n])` is unified out of the receiver's
/// own type and arrives as an interned constant, which
/// `solve_generic_params` reads back.
///
/// `bindings` is taken by mutable reference because the expected-type
/// solution (§2) is folded into it here: the callers substitute the same
/// map into the declared return type afterward, and a `C` solved only from
/// context has to reach that substitution too or the call would come back
/// typed as the abstract parameter.
/// `extra_fixed` carries bindings the *impl block* supplies, for a method
/// that is generic on both counts: `static def from_iter[I]` inside
/// `impl[T] from_iter[T] for list[T]` declares `I` and inherits `T`.
/// `solve_generic_params` only ever walks the declaration's own
/// `type_params`, so it solves `I` from the arguments and never learns `T`
/// exists — the instance then fails to check with ``undefined type `T` ``.
/// The impl's parameters have to arrive the way
/// `check_impl_generic_method_call` delivers them: as scoped fixed
/// bindings, not as solution slots.
///
/// The instance is recorded as the call's callee — `callee` with its
/// `decl` filled in — once the solution is concrete, which may be later
/// (`when_solution_settles`). False when the call cannot be monomorphized
/// (diagnosed here) or names no instance of its own (a template's call).
auto checker::instantiate_hk_method(
    const ast::call_expr &call, const method_entry &method,
    std::string_view target_type_name, param_subst &bindings,
    const resolved_callee &callee, const explicit_generic_args &explicit_args,
    const value_bindings &solved, type_id self_type,
    const type_scope *extra_fixed) -> bool {
  const auto &decl = *method.decl;
  const auto solution =
      solve_call(call, decl, method.owner, solved, bindings, explicit_args);
  if (!solution.has_value()) {
    return false;
  }
  auto scoped_params = method.fixed_type_params;
  if (extra_fixed != nullptr) {
    scoped_params.insert(extra_fixed->begin(), extra_fixed->end());
  }
  const auto name_instance = [this, &call, &method,
                              target = std::string(target_type_name),
                              scoped_params, self_type,
                              callee](const generic_solution &settled) -> bool {
    if (!check_call_bounds(call, *method.decl, method.owner, method.file_id,
                           settled, &scoped_params)) {
      return false;
    }
    const auto *instance = find_or_check_generic_instance(
        call, *method.decl, method.owner, method.file_id, settled,
        method_instance_name(target, method.decl->name, settled.suffix),
        &scoped_params, self_type);
    if (instance == nullptr) {
      return false;
    }
    auto named = callee;
    named.decl = instance;
    session_.resolved_callees_[&call] = named;
    return true;
  };
  if (!solution_is_open(*solution)) {
    return name_instance(*solution);
  }
  when_solution_settles(
      decl, *solution,
      [name_instance](const generic_solution &settled) -> void {
        (void)name_instance(settled);
      });
  return true;
}

/// Holds a callee's `pre` conditions to account at the call site.
///
/// A precondition has two faces, and they are the whole of contract
/// reasoning: inside the callee it is a *fact* it may assume
/// (`collect_function_facts`), and at every call site it is an *obligation*
/// the caller must meet. This is the obligation half.
///
/// Refuted: a compile error, because the call is wrong on every execution
/// that reaches it, and the spec says so outright ("When a condition is
/// statically knowable, violation is a compile error"). Proved or unproven:
/// silence. Neither one *elides* anything, and that is deliberate — the
/// runtime check lives at the callee's entry, where it stands in for every
/// call site at once, so a proof about this one call says nothing about the
/// next one. The elision a callee-entry check can honour is the one
/// `collect_function_facts` computes instead: a `pre` that follows from the
/// parameters' own types, and so cannot fail on any call at all.
auto checker::check_call_preconditions(const ast::call_expr &call,
                                       const ast::func_decl &decl,
                                       const std::vector<fn_param_info> &params,
                                       const ast::expr *ufcs_receiver) -> void {
  if (decl.contracts.empty()) {
    return;
  }
  const auto mapping = session_.call_argument_mappings_.find(&call);
  if (mapping == session_.call_argument_mappings_.end()) {
    return;
  }

  // Restate the precondition in the caller's terms: each parameter becomes
  // the argument actually supplied for it. A parameter left to its default,
  // or one whose argument falls outside the fragment, simply doesn't enter
  // the substitution — the condition then mentions a name nothing is known
  // about, and comes back `unknown`, which is exactly right.
  auto subst = predicate_subst{};
  for (size_t i = 0; i < params.size(); ++i) {
    const auto *argument =
        ufcs_argument_for(i, mapping->second.args_by_param, ufcs_receiver);
    if (argument == nullptr || params[i].name.empty()) {
      continue;
    }
    if (const auto poly = solver_poly(*argument, predicate_subst{})) {
      subst.values.emplace(params[i].name, *poly);
      subst.spellings.emplace(params[i].name,
                              describe(*argument, predicate_subst{}));
    }
  }

  for (const auto &contract : decl.contracts) {
    if (!contract.is_pre || contract.condition == nullptr ||
        contract.condition->has_error) {
      continue;
    }
    const auto goal = goal_from(*contract.condition, subst, false);
    if (goal.empty()) {
      continue;
    }
    switch (solve(function_.facts_, goal)) {
    case proof_result::proved:
      // Nothing to do: this call is safe, and the callee's own entry check
      // (which every *other* call site also relies on) stays put.
      break;
    case proof_result::refuted: {
      auto diag =
          diagnostic(diagnostic_level::error,
                     std::format("precondition `{}` is never true at this call",
                                 goal_label(goal, "the precondition")),
                     file_.file_id_);
      diag.with_label(call.span,
                      std::format("calling `{}` here always violates its "
                                  "contract",
                                  decl.name));
      if (const auto known = known_facts_note(goal)) {
        diag.children.emplace_back(diagnostic_level::note, *known,
                                   file_.file_id_);
      }
      diag.with_help(
          contract.message.has_value()
              ? std::format("`{}` documents this as: {}", decl.name,
                            *contract.message)
              : std::format("`{}` requires this to hold on entry. The "
                            "compiler can see that it doesn't — this is not "
                            "a check it failed to prove, but one it "
                            "disproved.",
                            decl.name));
      emit_diag(diag);
      mark_error();
      break;
    }
    case proof_result::unknown:
      // The check stays, and runs. This is what a contract is for.
      break;
    }
  }
}

/// Infers each argument's type with no expectation, for a call whose
/// callee could not be resolved — keeps names inside the arguments from
/// going unchecked even though the call itself can't be validated.
auto checker::infer_call_args_loosely(const ast::call_expr &call) -> void {
  for (const auto &arg : call.args) {
    if (arg.value != nullptr) {
      infer_expr(*arg.value, k_unknown_type);
    }
  }
}

/// Checks a sum-type variant constructor call's arguments against the
/// variant's declared payload types, and — if the sum type is generic and
/// wasn't already instantiated — infers its type arguments from payload
/// slots that name a bare type parameter directly.
auto checker::check_variant_construction(const ast::call_expr &call,
                                         const type_entry &instance,
                                         const ast::sum_variant &variant,
                                         type_id instance_id) -> type_id {
  const auto payload = variant_payload_types(instance, variant);
  if (call.args.size() != payload.size()) {
    error(call.span,
          std::format("variant `@{}` of `{}` takes {} value{}, found {}",
                      variant.name, instance.name, payload.size(),
                      payload.size() == 1 ? "" : "s", call.args.size()),
          "wrong number of constructor values");
    infer_call_args_loosely(call);
    return instance_id;
  }
  auto inferred_args = std::vector<type_id>{};
  for (size_t i = 0; i < call.args.size(); ++i) {
    const auto expected = payload[i];
    if (call.args[i].value == nullptr) {
      inferred_args.push_back(k_unknown_type);
      continue;
    }
    const auto found = infer_expr(*call.args[i].value, expected);
    type_mismatch(call.args[i].value->span, expected, found,
                  "for this constructor value");
    inferred_args.push_back(found);
  }

  // Instantiate a generic sum when a payload slot names a bare parameter.
  if (instance.decl != nullptr && !instance.decl->type_params.empty() &&
      instance.args.empty()) {
    auto args = std::vector<type_id>{};
    for (const auto &param : instance.decl->type_params) {
      auto bound = k_unknown_type;
      for (size_t i = 0; i < variant.payload_types.size(); ++i) {
        const auto *payload_type = variant.payload_types[i].get();
        if (payload_type != nullptr &&
            payload_type->kind == ast::node_kind::named_type) {
          const auto &named =
              dynamic_cast<const ast::named_type &>(*payload_type);
          if (named.path.size() == 1 && named.path.front() == param.name &&
              i < inferred_args.size()) {
            bound = inferred_args[i];
            break;
          }
        }
      }
      args.push_back(bound);
    }
    return make_user_type(*instance.decl, instance.module_name,
                          std::move(args));
  }
  return instance_id;
}

/// Handles `@some(x)` / `@ok(x)` / `@err(e)` / `@none` (and their bare
/// spellings) against an optional expected option/result instance.
auto checker::check_prelude_variant(std::string_view name,
                                    const ast::call_expr *call,
                                    source_span span, type_id expected)
    -> type_id {
  const auto &expected_entry = session_.types_.entry(expected);
  const auto expected_is = [&](std::string_view generic) -> auto {
    return expected_entry.kind == type_kind::builtin_generic_kind &&
           expected_entry.name == generic;
  };

  auto payload_expected = k_unknown_type;
  if ((name == "some" && expected_is("option")) ||
      (name == "ok" && expected_is("result"))) {
    payload_expected =
        expected_entry.args.empty() ? k_unknown_type : expected_entry.args[0];
  } else if (name == "err" && expected_is("result")) {
    payload_expected = expected_entry.args.size() > 1 ? expected_entry.args[1]
                                                      : k_unknown_type;
  }

  auto payload_found = k_unknown_type;
  if (call != nullptr) {
    if (name == "none" && !call->args.empty()) {
      error(span, "variant `@none` carries no value",
            "unexpected constructor value");
    }
    if (call->args.size() > 1) {
      error(span,
            std::format("variant `@{}` takes one value, found {}", name,
                        call->args.size()),
            "wrong number of constructor values");
    }
    for (const auto &arg : call->args) {
      if (arg.value != nullptr) {
        payload_found = infer_expr(*arg.value, payload_expected);
        if (payload_expected != k_unknown_type) {
          type_mismatch(arg.value->span, payload_expected, payload_found,
                        "for this constructor value", arg.value.get());
        }
      }
    }
  }

  // When the expected slot is itself still abstract (a callee's unsolved
  // type parameter — `option[B]` from a generic signature) but the payload
  // is concrete, answer with the concrete instantiation: echoing the
  // abstract expectation back would tell rigid inference (`match_params`)
  // nothing, leaving `B` unsolvable at a call that plainly determines it.
  if (expected_is("option") && (name == "some" || name == "none")) {
    if (name == "some" && !expected_entry.args.empty() &&
        session_.types_.is_unknown(expected_entry.args[0]) &&
        !session_.types_.is_unknown(payload_found)) {
      return session_.types_.builtin_generic("option", {payload_found});
    }
    return expected;
  }
  if (expected_is("result") && (name == "ok" || name == "err")) {
    if (!session_.types_.is_unknown(payload_found) &&
        expected_entry.args.size() == 2) {
      const auto slot = name == "ok" ? size_t{0} : size_t{1};
      if (session_.types_.is_unknown(expected_entry.args[slot])) {
        auto args = std::vector<type_id>{expected_entry.args[0],
                                         expected_entry.args[1]};
        args[slot] = payload_found;
        return session_.types_.builtin_generic("result", std::move(args));
      }
    }
    return expected;
  }
  if (name == "some" || name == "none") {
    return session_.types_.builtin_generic("option", {payload_found});
  }
  return session_.types_.builtin_generic(
      "result", name == "ok"
                    ? std::vector<type_id>{payload_found, k_unknown_type}
                    : std::vector<type_id>{k_unknown_type, payload_found});
}

/// Finds a sum-type variant named `name` declared by the current module,
/// or reachable through a non-wildcard import of its owning type.
auto checker::find_module_variant(std::string_view name) -> std::optional<
    std::pair<const ast::type_decl *, const ast::sum_variant *>> {
  if (file_.module_ == nullptr) {
    return std::nullopt;
  }
  if (const auto it = file_.module_->variants.find(std::string(name));
      it != file_.module_->variants.end()) {
    return std::pair{it->second.sum_decl, it->second.variant};
  }
  // Variants of imported sum types.
  const auto *imports = imports_for_current_file();
  if (imports != nullptr) {
    for (const auto &binding : *imports) {
      if (binding.is_wildcard) {
        continue;
      }
      if (const auto *source = import_source_module(binding)) {
        const auto member = imported_member_name(binding);
        if (const auto it = source->types.find(member);
            it != source->types.end()) {
          const auto member_it = source->variants.find(std::string(name));
          if (member_it != source->variants.end() &&
              member_it->second.sum_decl == it->second.decl) {
            return std::pair{member_it->second.sum_decl,
                             member_it->second.variant};
          }
        }
      }
    }
  }
  // Variants of sum types re-exported by a session-owned wildcard import
  // (`use a.b.*` brings every type along, so every variant comes too).
  for (const auto *source : wildcard_import_sources()) {
    if (const auto it = source->variants.find(std::string(name));
        it != source->variants.end()) {
      return std::pair{it->second.sum_decl, it->second.variant};
    }
  }
  return std::nullopt;
}

/// Resolves a variant name (`@some`/`@ok`/`@err`/`@none`, or a
/// user sum-type variant) to its constructed type. Prefers a variant of
/// the expected sum type when one matches, then falls back to any
/// module-visible sum type declaring that variant name. `call` is
/// non-null when the variant is being constructed with arguments
/// (`@some(x)`) rather than referenced bare (`@none`). Returns `nullopt`
/// when no sum type declares this variant.
auto checker::resolve_variant_value(std::string_view name,
                                    const ast::call_expr *call,
                                    source_span span, type_id expected)
    -> std::optional<type_id> {
  if (name == "some" || name == "none" || name == "ok" || name == "err") {
    return check_prelude_variant(name, call, span, expected);
  }

  // Prefer a variant of the expected sum type, then module-level sums.
  const auto expected_entry = session_.types_.entry(strip_refs(expected));
  if (expected_entry.kind == type_kind::sum_kind) {
    if (const auto *variant = find_variant(expected_entry, name)) {
      if (call != nullptr) {
        return check_variant_construction(*call, expected_entry, *variant,
                                          strip_refs(expected));
      }
      if (!variant->payload_types.empty()) {
        error(span,
              std::format("variant `@{}` of `{}` carries {} value{} and "
                          "must be constructed with `@{}(...)`",
                          name, expected_entry.name,
                          variant->payload_types.size(),
                          variant->payload_types.size() == 1 ? "" : "s", name),
              "missing constructor values");
      }
      return strip_refs(expected);
    }
  }

  if (const auto found = find_module_variant(name)) {
    const auto *decl = found->first;
    const auto instance_id = make_user_type(*decl, file_.module_name_, {});
    const auto &instance = session_.types_.entry(instance_id);
    if (call != nullptr) {
      return check_variant_construction(*call, instance, *found->second,
                                        instance_id);
    }
    if (!found->second->payload_types.empty()) {
      error(span,
            std::format("variant `@{}` of `{}` carries {} value{} and must "
                        "be constructed with `@{}(...)`",
                        name, decl->name, found->second->payload_types.size(),
                        found->second->payload_types.size() == 1 ? "" : "s",
                        name),
            "missing constructor values");
    }
    return instance_id;
  }

  // A variant of a prelude-re-exported sum type (`@less` of `ordering`),
  // reachable without a `use` exactly as the type itself is.
  if (const auto found = find_prelude_variant(name)) {
    const auto instance_id =
        make_user_type(*found->sum_decl, found->module_name, {});
    const auto &instance = session_.types_.entry(instance_id);
    if (call != nullptr) {
      return check_variant_construction(*call, instance, *found->variant,
                                        instance_id);
    }
    if (!found->variant->payload_types.empty()) {
      error(span,
            std::format("variant `@{}` of `{}` carries {} value{} and must "
                        "be constructed with `@{}(...)`",
                        name, found->sum_decl->name,
                        found->variant->payload_types.size(),
                        found->variant->payload_types.size() == 1 ? "" : "s",
                        name),
            "missing constructor values");
    }
    return instance_id;
  }

  return std::nullopt;
}

} // namespace cinder::semantic::detail
