#include "checker.h"

#include <cassert>

#include "src/parser/ast_traversal.h"

namespace cinder::semantic::detail {

// ==========================================================================
//  Call checking
// ==========================================================================

/// How a parameter of declared type `type` receives its argument — see
/// `call_argument_mapping::passing_by_param`. Only the outermost type
/// constructor matters: a `&`/`&mut` parameter borrows, anything else
/// takes its argument by value.
auto checker::passing_of(type_id type) const -> param_passing {
  if (session_.types_.is_unknown(type)) {
    return param_passing::by_value;
  }
  const auto &entry = session_.types_.entry(type);
  if (entry.kind != type_kind::ref_kind) {
    return param_passing::by_value;
  }
  return entry.is_mut ? param_passing::mut_ref : param_passing::shared_ref;
}

/// The value parameters a call gave in brackets, as the bindings argument
/// solving starts from: `split[3](a)` knows `m` before it looks at `a`, and
/// so solves `3 + n = 5` rather than the underdetermined `m + n = 5`.
auto checker::explicit_value_bindings(const generic_call_context *generic)
    -> value_bindings {
  auto bindings = value_bindings{};
  if (generic == nullptr || generic->decl == nullptr ||
      generic->explicit_args == nullptr) {
    return bindings;
  }
  const auto &type_params = generic->decl->type_params;
  for (size_t i = 0;
       i < generic->explicit_args->size() && i < type_params.size(); ++i) {
    const auto &param = type_params[i];
    const auto *value = (*generic->explicit_args)[i].value;
    if (!param.is_value_param || value == nullptr) {
      continue;
    }
    // The same readings `solve_generic_params` gives it: a constant, or a
    // polynomial in the caller's own value parameters (`climb[n + 1]`).
    auto poly = std::optional<linear_poly>{};
    if (const auto constant = explicit_value_argument(*value)) {
      poly = poly_constant(*constant);
    } else {
      poly = value_poly(*value, quiet_ctx(current_resolve_ctx()));
    }
    if (poly.has_value()) {
      bindings.emplace(value_var(param_id(param)), *poly);
    }
  }
  return bindings;
}

/// Matches a call's arguments against a parameter list: positional
/// arguments fill the next unused parameter in order, named arguments
/// bind by name (reporting unknown/duplicate names), and each argument's
/// inferred type is checked against its target parameter's type. Reports
/// too many arguments, and any required parameter left unfilled. Also
/// records the resulting argument-to-parameter assignment into
/// `session_.call_argument_mappings_` (see `call_argument_mapping`'s doc
/// comment in types.h), since this is the only place that assignment is known.
/// `solved_out`, when given, receives the callee's value parameters as the
/// arguments determined them (`match_value_params`) — the same bindings the
/// refinement obligations below are discharged under, handed back so
/// `check_call` can monomorphize the callee with them.
auto checker::check_call_args_against(
    const ast::call_expr &call, const std::vector<fn_param_info> &params,
    std::string_view callee_name, source_location decl_location,
    value_bindings *solved_out, const generic_call_context *generic) -> void {
  auto param_used = std::vector<bool>(params.size(), false);
  auto args_by_param = std::vector<const ast::expr *>(params.size(), nullptr);
  auto next_positional = size_t{0};
  auto seen_named = false;
  // See the deferral comment in the argument loop below.
  auto solved_values = explicit_value_bindings(generic);
  auto deferred = std::vector<deferred_narrowing>{};
  auto pending = std::vector<pending_argument>{};

  for (const auto &arg : call.args) {
    const fn_param_info *target = nullptr;
    if (arg.name.has_value()) {
      const auto &arg_name = *arg.name;
      seen_named = true;
      for (size_t i = 0; i < params.size(); ++i) {
        if (params[i].name == arg_name) {
          if (param_used[i]) {
            error(arg.span,
                  std::format("argument `{}` is provided more than once",
                              arg_name),
                  "duplicate argument");
          }
          param_used[i] = true;
          args_by_param[i] = arg.value.get();
          target = &params[i];
          break;
        }
      }
      if (target == nullptr) {
        auto diag = diagnostic(
            diagnostic_level::error,
            std::format("unknown named argument `{}` in call to `{}`", arg_name,
                        callee_name),
            file_.file_id_);
        diag.with_label(arg.span, "no parameter has this name");
        auto names = std::vector<std::string>{};
        for (const auto &param : params) {
          names.push_back(param.name);
        }
        if (const auto suggestion = best_suggestion(arg_name, names)) {
          diag.with_help(std::format("did you mean `{}:`?", *suggestion));
        }
        emit_diag(diag);
        mark_error();
      }
    } else {
      if (seen_named) {
        error(arg.span, "positional arguments may not follow named arguments",
              "positional argument after a named one");
      }
      while (next_positional < params.size() && param_used[next_positional]) {
        ++next_positional;
      }
      if (next_positional < params.size()) {
        param_used[next_positional] = true;
        args_by_param[next_positional] = arg.value.get();
        target = &params[next_positional];
        ++next_positional;
      } else {
        error(arg.span,
              std::format("too many arguments to `{}`: expected {}, found {}",
                          callee_name, params.size(), call.args.size()),
              "unexpected extra argument");
        if (arg.value != nullptr) {
          infer_expr(*arg.value, k_unknown_type);
        }
        continue;
      }
    }

    if (arg.value != nullptr) {
      pending.push_back(
          pending_argument{.value = arg.value.get(), .target = target});
    }
  }

  // `session_.call_argument_mappings_` is published before the arguments are
  // checked, not after, because the generic solving between the two passes
  // below reads it back to find which argument reached which parameter.
  auto defaults_by_param = std::vector<const ast::expr *>{};
  auto param_names = std::vector<std::string>{};
  auto passing_by_param = std::vector<param_passing>{};
  defaults_by_param.reserve(params.size());
  param_names.reserve(params.size());
  passing_by_param.reserve(params.size());
  for (const auto &param : params) {
    defaults_by_param.push_back(param.default_value);
    param_names.push_back(param.name);
    passing_by_param.push_back(passing_of(param.type));
  }
  session_.call_argument_mappings_[&call] =
      call_argument_mapping{.args_by_param = args_by_param,
                            .defaults_by_param = std::move(defaults_by_param),
                            .param_names = std::move(param_names),
                            .passing_by_param = std::move(passing_by_param)};

  // A call instantiates the callee's own type parameters afresh, so while
  // they are unsolved they stand for nothing yet — not for the parameter of
  // the same name the caller may itself have, and not for the callee's
  // own `T` inside a recursive call. Opened to `unknown` for the check
  // below; the solution re-check (`check_args_against_solution`) compares
  // against what they solved to.
  auto opened = std::unordered_map<type_id, type_id>{};
  if (generic != nullptr && generic->decl != nullptr) {
    for (const auto &param : generic->decl->type_params) {
      // Value parameters are solved *from* this check
      // (`match_value_params`), so they stay as they are.
      const auto is_type = !param.is_value_param ||
                           (param.bound_or_type != nullptr &&
                            names_trait_or_concept(*param.bound_or_type));
      if (!param.name.empty() && is_type) {
        opened.emplace(session_.types_.type_param(
                           param.name, param.higher_kinded_arity, &param),
                       k_unknown_type);
      }
    }
  }
  // A recursive call's signature names the caller's own value parameters,
  // which `value_slots_hold` would hold fixed; there the callee's are solved
  // apart by `match_value_params` instead.
  const auto callee_values_rigid =
      generic != nullptr && generic->decl != nullptr &&
      std::ranges::any_of(generic->decl->type_params,
                          [&](const ast::type_param &param) -> bool {
                            return param.is_value_param && !param.name.empty() &&
                                   is_rigid_param(param_id(param));
                          });
  // `hint` types the argument without being checked against: a literal
  // takes a bracketed `int64` from it, and a disagreement with the brackets
  // is left to `check_args_against_solution`, which says which bracket.
  const auto check_argument = [&](const pending_argument &item,
                                  const param_subst &bindings,
                                  const param_subst &hint) -> void {
    const auto declared = item.target != nullptr
                              ? substitute_solved(item.target->type, bindings)
                              : k_unknown_type;
    const auto expected =
        open_foreign_params(substitute_solved(declared, opened));
    const auto hinted =
        hint.empty() ? expected
                     : open_foreign_params(substitute_solved(
                           substitute_solved(declared, hint), opened));
    const auto found = infer_expr(*item.value, hinted);
    if (item.target == nullptr) {
      return;
    }
    // Checked against the opened type; reported, if it fails even so,
    // against the callee's own spelling (`vec[T, n + 1]`, not `vec[_, ...]`),
    // which is what the reader can find in its signature.
    const auto shown =
        !session_.types_.compatible(settle(expected), settle(found)) &&
                declared != expected
            ? declared
            : expected;
    // Structure is decided now; a *proof* has to wait. A refined
    // parameter's predicate is written in the callee's value parameters
    // (`index[n]`'s `n`), and which arguments pin those down isn't known
    // until every argument has been seen — `safe_get(v, i)` learns `n`
    // from `v`, which for a differently-ordered signature could just as
    // easily come after `i`. So the shape check runs in place and the
    // obligation is collected for the second pass below.
    // The value is not handed to `type_mismatch`, which would also check a
    // refinement now, before the value parameters are solved (the deferred
    // pass below does that), so the missing-`*` hint is asked for here.
    if (!session_.types_.compatible(settle(expected), settle(found))) {
      session_.mismatched_argument_calls_.insert(&call);
      if (report_deref_if_missing(item.value, shown, found)) {
        match_value_params(expected, found, solved_values);
        return;
      }
    }
    type_mismatch(item.value->span, shown, found, "for this argument",
                  nullptr, /*rigid_values=*/!callee_values_rigid);
    match_value_params(expected, found, solved_values);
    if (session_.types_.entry(expected).kind == type_kind::refinement_kind) {
      deferred.push_back(deferred_narrowing{
          .expected = expected,
          .found = found,
          .value = item.value,
          .span = item.value->span,
      });
    }
  };

  // Lambda arguments go last, and against a *substituted* parameter type.
  //
  // A lambda is the one argument whose type is decided by what is expected
  // of it rather than by what it contains: `x => x * 2` passed to an
  // `f: fn(T) -> U` has no type of its own until `T` is known. Checked in
  // declaration order it would be inferred against the literal `fn(T) -> U`
  // and bind its parameter to the type *parameter* `T`, which then reaches
  // lowering as an abstract type and fails there rather than here ("type
  // `T` has no concrete representation"). Every other argument
  // determines its own type, so running those first — and solving the call's
  // generic parameters from them plus the declared bounds — is what makes
  // `T` known by the time the lambda needs it.
  //
  // Each argument is still inferred exactly once. This reorders when the
  // lambdas are visited, it does not visit them twice: `infer_expr` mints
  // symbols and declares locals, and running it twice over one lambda would
  // register its parameters two times over.
  // Brackets are known before any argument is seen, so a literal argument
  // is typed by them (`ident[int64](2000000000)` is an `int64` literal).
  // Only a concrete bracket hints: one that failed to resolve
  // (`list[nope]`) is already reported, and must not type the argument into
  // a second, cascading conflict.
  auto no_bindings = param_subst{};
  auto bracketed = param_subst{};
  if (generic != nullptr) {
    for (const auto &[param, type] : explicit_type_bindings(*generic)) {
      if (!mentions_abstract_type(type, /*leaves_are_answers=*/true)) {
        bracketed.emplace(param, type);
      }
    }
  }
  for (const auto &item : pending) {
    if (item.value->kind != ast::node_kind::lambda_expr) {
      check_argument(item, no_bindings, bracketed);
    }
  }
  auto bindings = param_subst{};
  if (generic != nullptr) {
    // Not a point of demand: the lambda is checked against these, and a
    // parameter still answered by a literal's leaf is exactly what its
    // body may be about to say (phase 11, defect 2).
    bindings = preliminary_type_bindings(call, params, *generic);
  }
  for (const auto &item : pending) {
    if (item.value->kind == ast::node_kind::lambda_expr) {
      check_argument(item, bindings, no_bindings);
    }
  }

  for (const auto &obligation : deferred) {
    check_narrowing(obligation.expected, obligation.found, obligation.value,
                    obligation.span, "for this argument", &solved_values);
  }

  for (size_t i = 0; i < params.size(); ++i) {
    if (param_used[i] || params[i].has_default) {
      continue;
    }
    auto diag = diagnostic(diagnostic_level::error,
                           std::format("missing argument `{}` in call to `{}`",
                                       params[i].name.empty()
                                           ? std::format("#{}", i + 1)
                                           : params[i].name,
                                       callee_name),
                           file_.file_id_);
    diag.with_label(call.span, "call is missing a required argument");
    if (decl_location.span.len() > 0) {
      diag.children.push_back(
          diagnostic(diagnostic_level::note,
                     std::format("`{}` is declared here", callee_name),
                     decl_location.file_id)
              .with_label(decl_location.span, "declaration"));
    }
    emit_diag(diag);
    mark_error();
  }

  if (solved_out != nullptr) {
    *solved_out = std::move(solved_values);
  }
}

// ==========================================================================
//  One call pipeline
// ==========================================================================

auto checker::member_needs_block_instance(const call_callee &callee) -> bool {
  if (callee.form != callee_form::member || callee.method == nullptr ||
      callee.method->block_type_params == nullptr) {
    return false;
  }
  if (!callee.method->block_type_params->empty()) {
    return true;
  }
  return callee.self_type != k_unknown_type &&
         impl_needs_instance(*callee.method,
                             session_.types_.entry(callee.self_type));
}

auto checker::member_target(const call_callee &callee, const param_subst &block)
    -> type_id {
  if (callee.self_type != k_unknown_type) {
    // Settled: a receiver the arguments pinned (`out.push(i)` on a
    // `list[?a]`) is instantiated at what it now is.
    return settle(callee.self_type);
  }
  // A prelude constructor's static (`option.pure(1)`) has no target type to
  // instantiate a block at, only its own parameters.
  if (!member_needs_block_instance(callee)) {
    return k_unknown_type;
  }
  return settle(substitute_solved(callee.method->impl_target_pattern, block));
}

template <typename Body>
auto checker::in_bound_scope(const call_callee &callee, bool trait_file,
                             Body body) -> decltype(body()) {
  struct restore {
    checker *self;
    const module_members *module;
    file_id_type file;
    type_id self_type;
    decltype(function_.self_assoc_types_) self_assoc_types;
    ~restore() {
      self->file_.module_ = module;
      self->file_.file_id_ = file;
      self->function_.self_type_ = self_type;
      self->function_.self_assoc_types_ = std::move(self_assoc_types);
    }
  } const guard{this, file_.module_, file_.file_id_, function_.self_type_,
                std::exchange(function_.self_assoc_types_, {})};
  if (trait_file) {
    file_.module_ = callee.owner;
    file_.file_id_ = callee.decl_file;
  }
  function_.self_type_ = callee.self_type;
  return body();
}

auto checker::bound_callee_signature(const call_callee &callee)
    -> call_signature {
  const auto &decl = *callee.decl;
  const auto &trait_params = callee.trait->type_params;
  auto sig = call_signature{};
  sig.all_params = in_bound_scope(
      callee, /*trait_file=*/true, [&] -> std::vector<fn_param_info> {
        return signature_params(decl, callee.owner,
                                /*skip_self=*/callee.receiver ==
                                    receiver_use::self_param,
                                &trait_params);
      });
  // The trait's parameters play the block's part: the fact answers them
  // (`scale[int32]` makes `K` an `int32`), and only the method's own
  // parameters are left to the call.
  for (size_t i = 0; i < trait_params.size(); ++i) {
    sig.generic_params.push_back(&trait_params[i]);
    if (i < callee.trait_args.size()) {
      sig.seed.emplace(param_id(trait_params[i]), callee.trait_args[i]);
    }
  }
  for (const auto &param : decl.type_params) {
    sig.generic_params.push_back(&param);
  }
  sig.generic = is_generic_template(decl);
  sig.params = sig.all_params;
  for (auto &param : sig.params) {
    param.type = substitute_solved(param.type, sig.seed);
  }
  return sig;
}

auto checker::callee_signature(const call_callee &callee) -> call_signature {
  if (callee.form == callee_form::fn_value) {
    // A `fn` type has parameter types only: no names and no defaults.
    auto sig = call_signature{};
    const auto args = session_.types_.entry(strip_refs(callee.fn_type)).args;
    for (const auto arg : args) {
      sig.params.push_back(fn_param_info{.type = arg});
    }
    sig.all_params = sig.params;
    return sig;
  }
  if (callee.form == callee_form::bound_member) {
    return bound_callee_signature(callee);
  }
  const auto &decl = *callee.decl;
  const auto *block_params =
      callee.method != nullptr ? callee.method->block_type_params : nullptr;
  auto sig = call_signature{};
  // Under the declaring file, not the caller's: a parameter type naming
  // something the callee's module imported resolves through its imports.
  const auto saved_file = file_.file_id_;
  file_.file_id_ = callee.decl_file;
  // A self method's `self` is the receiver, not a written argument.
  sig.all_params = signature_params(
      decl, callee.owner,
      /*skip_self=*/callee.receiver == receiver_use::self_param, block_params);
  file_.file_id_ = saved_file;
  // The result type is not read here: reading a return type can start
  // inferring it from the callee's body, which `check_call` does only once
  // the arguments are checked, as it always has.
  if (block_params != nullptr) {
    for (const auto &param : *block_params) {
      sig.generic_params.push_back(&param);
    }
  }
  for (const auto &param : decl.type_params) {
    sig.generic_params.push_back(&param);
  }
  sig.generic =
      is_generic_template(decl) || member_needs_block_instance(callee);
  if (callee.form == callee_form::member) {
    // The block's parameters are read off the type the static is called on
    // (`wrap[int32].new(v)` takes an `int32`) or the receiver (`xs.push(x)`
    // on a `list[int32]`); a bare generic name leaves them to the arguments
    // (stage 4).
    if (block_params != nullptr && callee.self_type != k_unknown_type) {
      match_params(callee.method->impl_target_pattern, callee.self_type,
                   sig.seed);
      solve_impl_value_params(*callee.method, callee.self_type, sig.seed);
    }
    if (callee.receiver != receiver_use::first_param ||
        sig.all_params.empty()) {
      sig.params = sig.all_params;
      for (auto &param : sig.params) {
        param.type = substitute_solved(param.type, sig.seed);
      }
      return sig;
    }
    // A receiver-style method (`ma.bind(f)`): the receiver fills the first
    // parameter and answers the method's own parameters it mentions
    // (`option[A]` against an `option[int32]`). Those go in
    // `receiver_bindings`, not the seed, which holds the block alone.
    sig.receiver_param = sig.all_params.front();
    sig.receiver_param->type =
        substitute_solved(sig.receiver_param->type, sig.seed);
    match_params(strip_refs(sig.receiver_param->type),
                 strip_refs(callee.self_type), sig.receiver_bindings);
    sig.params.assign(sig.all_params.begin() + 1, sig.all_params.end());
    for (auto &param : sig.params) {
      param.type = substitute_solved(
          substitute_solved(param.type, sig.seed), sig.receiver_bindings);
    }
    return sig;
  }
  if (callee.receiver != receiver_use::first_param) {
    sig.params = sig.all_params;
    return sig;
  }
  sig.receiver_param = sig.all_params.front();
  // Solved through the references on both sides: a `&T` parameter taking
  // an auto-ref'd `T` receiver still has to bind `T`, and comparing the
  // decorated types would bind nothing.
  match_params(strip_refs(sig.receiver_param->type),
               strip_refs(callee.self_type), sig.seed);
  sig.params.assign(sig.all_params.begin() + 1, sig.all_params.end());
  for (auto &param : sig.params) {
    param.type = substitute_solved(param.type, sig.seed);
  }
  return sig;
}

auto checker::bind_explicit_args(const ast::call_expr & /*call*/,
                                 const call_callee &callee,
                                 const call_signature & /*sig*/) -> bool {
  if (callee.form == callee_form::fn_value) {
    return true;
  }
  return check_method_accepts_generic_args(callee.callee_node->span,
                                           *callee.decl, callee.explicit_args);
}

auto checker::check_call_arguments(const ast::call_expr &call,
                                   const call_callee &callee,
                                   call_signature &sig) -> value_bindings {
  if (callee.form == callee_form::fn_value) {
    return check_fn_value_arguments(call, callee, sig);
  }
  const auto &decl = *callee.decl;
  auto solved = value_bindings{};
  // A receiver-style member's receiver was found by its constructor alone
  // (`receiver_fills_first_param`), so its arguments are checked here; a
  // UFCS receiver was already fitted against the first parameter
  // (`classify_receiver_fit`), auto-ref included.
  if (callee.form == callee_form::member && sig.receiver_param.has_value() &&
      callee.receiver_expr != nullptr) {
    type_mismatch(callee.receiver_expr->span,
                  substitute_solved(strip_refs(sig.receiver_param->type),
                                    sig.receiver_bindings),
                  strip_refs(callee.self_type), "for this method's receiver");
  }
  // The lambda arguments are typed under what the receiver already said.
  auto seed = sig.seed;
  for (const auto &[id, type] : sig.receiver_bindings) {
    seed.try_emplace(id, type);
  }
  const auto generic = generic_call_context{
      .decl = &decl,
      .owner = callee.owner,
      .explicit_args = &callee.explicit_args,
      .ufcs_receiver = nullptr,
      .seed = seed.empty() ? nullptr : &seed};
  // A callee with an unannotated parameter is solved from its own body
  // (`resolve_open_param_calls`), not from the arguments: the solving a
  // generic context does would owe that parameter's leaf a shape now, and a
  // literal argument would settle to `int32` before the body says `int64`.
  // A generic free function never has one; a method of a generic block can.
  check_call_args_against(
      call, sig.params, decl.name,
      source_location{.file_id = callee.decl_file, .span = decl.span}, &solved,
      sig.generic && !has_unannotated_params(decl) ? &generic : nullptr);
  // A self method's `all_params` leave `self` out, so the receiver is not
  // the first of them.
  check_call_preconditions(call, decl, sig.all_params,
                           callee.receiver == receiver_use::first_param
                               ? callee.receiver_expr
                               : nullptr);
  return solved;
}

auto checker::check_fn_value_arguments(const ast::call_expr &call,
                                       const call_callee &callee,
                                       const call_signature &sig)
    -> value_bindings {
  // A `fn` type names no parameters, so there is nothing for a name to
  // pick out.
  const auto named = std::ranges::find_if(
      call.args, [](const auto &arg) -> bool { return arg.name.has_value(); });
  if (named != call.args.end()) {
    const auto *node = callee.callee_node;
    const auto has_name = node != nullptr &&
                          (node->kind == ast::node_kind::ident_expr ||
                           node->kind == ast::node_kind::field_expr);
    error_with_help(
        named->span,
        has_name ? std::format("`{}` is a function value, so its arguments "
                               "are positional",
                               callee.display_name)
                 : std::string("a function value's arguments are positional"),
        "named argument",
        "A function value's type lists only its parameters' types, not their "
        "names. Drop the names and pass the arguments in order.");
    infer_call_args_loosely(call);
    return {};
  }
  check_call_args_against(call, sig.params, callee.display_name,
                          source_location{});
  return {};
}

auto checker::solve_call_callee(const ast::call_expr &call,
                                const call_callee &callee, call_signature &sig,
                                const value_bindings &solved)
    -> std::optional<generic_solution> {
  const auto &decl = *callee.decl;
  if (!sig.generic) {
    return std::nullopt;
  }
  if (callee.form == callee_form::bound_member) {
    // The trait's signatures are written with `self`, so the solving that
    // reads them (the bounds, the expected type) reads it as the bound's
    // type parameter.
    return in_bound_scope(
        callee, /*trait_file=*/false, [&] -> std::optional<generic_solution> {
          return solve_member_callee(call, callee, sig, solved);
        });
  }
  if (callee.form != callee_form::member) {
    // `is_static` alongside `is_free_function`: a generic `static def`
    // reached as a free function is named like one (see
    // `check_call`).
    if (!is_free_function(decl, callee.owner) && !decl.modifiers.is_static) {
      return std::nullopt;
    }
    return solve_generic_call(call, decl, callee.owner, callee.decl_file,
                              solved, sig.all_params, callee.explicit_args,
                              callee.receiver_expr);
  }
  return solve_member_callee(call, callee, sig, solved);
}

auto checker::solve_member_callee(const ast::call_expr &call,
                                  const call_callee &callee,
                                  call_signature &sig,
                                  const value_bindings &solved)
    -> std::optional<generic_solution> {
  const auto &decl = *callee.decl;
  // A `bound_member` has no block: its trait's parameters are already in
  // the seed, answered by the fact.
  const auto *block_params =
      callee.method != nullptr ? callee.method->block_type_params : nullptr;
  auto bindings = sig.seed;
  for (const auto &[id, type] : sig.receiver_bindings) {
    bindings.try_emplace(id, type);
  }
  solve_from_argument_types(call, sig.params, bindings,
                            /*ufcs_receiver=*/nullptr,
                            /*bind_open=*/true);
  if (block_params != nullptr) {
    for (const auto &type_param : *block_params) {
      if (type_param.name.empty() || bindings.contains(param_id(type_param))) {
        continue;
      }
      // Called from inside its own block: the block's `T` is this body's
      // own, rigid here, and matching it against itself bound nothing.
      if (const auto own = session_.types_.type_param(
              type_param.name, type_param.higher_kinded_arity, &type_param);
          is_rigid_param(own)) {
        bindings.emplace(own, own);
        continue;
      }
      if (callee.receiver != receiver_use::none) {
        report_block_param_not_on_receiver(call.span, type_param, decl.name,
                                           !callee.method->trait_name.empty());
      } else if (callee.self_type == k_unknown_type) {
        error_with_help(
            call.span,
            std::format("cannot tell which `{}` this call to `{}` means",
                        type_param.name, callee.display_name),
            std::format("`{}` is not determined by the arguments",
                        type_param.name),
            std::format(
                "`{}` belongs to the `extend` block, and `{}` names the type "
                "without saying what `{}` is, so the compiler reads it off "
                "the arguments instead. None of the arguments mention `{}`. "
                "Name the type in full to say which one is meant, as in "
                "`{}[int32].{}(...)`.",
                type_param.name, callee.target_name, type_param.name,
                type_param.name, callee.target_name, decl.name));
      } else {
        error_with_help(
            call.span,
            std::format("cannot tell which `{}` this call to `{}` means",
                        type_param.name, callee.display_name),
            std::format("`{}` is not determined by the type `{}`",
                        type_param.name, callee.target_name),
            std::format(
                "`{}` is declared on the block that provides `{}`, and the "
                "compiler solves it by matching the block's target type "
                "against `{}`. Since `{}` doesn't appear in that target "
                "type, no call can pin it down. Move `{}` onto `{}` itself, "
                "so it can be solved from the arguments or given explicitly.",
                type_param.name, decl.name, callee.target_name,
                type_param.name, type_param.name, decl.name));
      }
      return std::nullopt;
    }
    for (const auto &type_param : *block_params) {
      if (const auto found = bindings.find(param_id(type_param));
          found != bindings.end()) {
        sig.seed.insert_or_assign(found->first, found->second);
      }
    }
  }

  auto solution = generic_solution{};
  auto bracketed = std::unordered_set<type_id>{};
  if (!decl.type_params.empty()) {
    auto own = solve_call(call, decl, callee.owner, solved, bindings,
                          callee.explicit_args);
    if (!own.has_value()) {
      return std::nullopt;
    }
    solution = std::move(*own);
    for (size_t i = 0;
         i < callee.explicit_args.size() && i < decl.type_params.size(); ++i) {
      bracketed.insert(param_id(decl.type_params[i]));
    }
  }
  auto subst = bindings;
  for (const auto &[id, type] : solution_subst(decl.type_params, solution)) {
    subst.insert_or_assign(id, type);
  }
  // The method's own parameters are named the way the bare form names them
  // (`ident`'s `T`); a block parameter solved from the arguments by the
  // type it belongs to (`box.make`).
  if (check_args_against_solution(
          call, sig.params, subst,
          decl.type_params.empty() ? callee.display_name : decl.name,
          /*ufcs_receiver=*/nullptr, bracketed)) {
    return std::nullopt;
  }
  if (block_params != nullptr) {
    carry_impl_value_slots(*callee.method, sig.seed, solution);
  }
  // A bound on an answer that is still an open leaf waits with the
  // instance (`name_member_solution`).
  const auto block_open =
      std::ranges::any_of(sig.seed, [&](const auto &slot) -> bool {
        return mentions_type_var(slot.second);
      });
  auto scoped = callee.method != nullptr ? callee.method->fixed_type_params
                                          : type_scope{};
  scoped.merge(bindings_by_name(sig.seed));
  solution.bounds_hold =
      block_open || solution_is_open(solution) ||
      check_call_bounds(call, decl, callee.owner, callee.decl_file, solution,
                        &scoped);
  return solution;
}

auto checker::report_block_param_not_on_receiver(
    source_span span, const ast::type_param &type_param,
    std::string_view method, bool trait_method) -> void {
  // A trait's method cannot take parameters its trait does not declare, so
  // the parameter can only leave the block.
  const auto fix =
      trait_method
          ? std::format("Remove `{}` from the `impl` block, or use it in the "
                        "target type.",
                        type_param.name)
          : std::format("Move `{}` onto `{}` itself, so it can be solved "
                        "from the arguments or given explicitly.",
                        type_param.name, method);
  error_with_help(
      span,
      std::format("cannot tell which `{}` this call to `{}` means",
                  type_param.name, method),
      std::format("`{}` is not determined by the receiver's type",
                  type_param.name),
      std::format("`{}` is declared on the `impl` block, and the compiler "
                  "solves it by matching the impl's target type against the "
                  "receiver. Since `{}` doesn't appear in that target type, "
                  "no receiver can pin it down. {}",
                  type_param.name, type_param.name, fix));
}

auto checker::member_instance_solution(const call_callee &callee,
                                       type_id target,
                                       const param_subst &block,
                                       const generic_solution &solution,
                                       std::string_view discriminator)
    -> generic_solution {
  auto named = solution;
  if (callee.method->block_type_params != nullptr) {
    carry_impl_value_slots(*callee.method, block, named);
  }
  // One generic only through its block is keyed on the target, since that
  // is what tells two impls on two instantiations of one type apart.
  if (callee.decl->type_params.empty()) {
    named.suffix =
        std::format("${}{}", mangle_type_for_instance(target), discriminator);
  }
  return named;
}

auto checker::member_instance(const ast::node &site, const call_callee &callee,
                              type_id target, const param_subst &block,
                              const generic_solution &named)
    -> const ast::func_decl * {
  const auto &method = *callee.method;
  const auto &decl = *callee.decl;
  auto scoped = method.fixed_type_params;
  scoped.merge(bindings_by_name(block));
  // A member generic over its own parameters is named by them on the
  // target's spelling (`wrap[int32]::ident$int64`), so the block's
  // arguments are in the name; one generic only through its block, by the
  // target (`wrap::new$wrap_int32_`). Owned before
  // `find_or_check_generic_instance` runs: checking the instance body
  // interns types.
  auto target_name = callee.target_name;
  if (decl.type_params.empty()) {
    target_name = std::string(session_.types_.entry(target).name);
  } else if (target != k_unknown_type) {
    target_name = session_.types_.display(target);
  }
  return find_or_check_generic_instance(
      site, decl, callee.owner, callee.decl_file, named,
      method_instance_name(target_name, decl.name, named.suffix), &scoped,
      target, method.block_type_params);
}

auto checker::name_member_instance(const ast::call_expr &call,
                                   const call_callee &callee, type_id target,
                                   const param_subst &block,
                                   const generic_solution &solution,
                                   bool check_bounds)
    -> const ast::func_decl * {
  const auto &method = *callee.method;
  const auto &decl = *callee.decl;
  const auto named = member_instance_solution(callee, target, block, solution);
  auto scoped = method.fixed_type_params;
  scoped.merge(bindings_by_name(block));
  if (check_bounds && !check_call_bounds(call, decl, callee.owner,
                                         callee.decl_file, named, &scoped)) {
    return nullptr;
  }
  const auto *instance = member_instance(call, callee, target, block, named);
  if (instance == nullptr) {
    return nullptr;
  }
  if (session_.comptime_only_functions_.contains(&decl)) {
    session_.comptime_call_type_slots_[&call] = named.type_slots;
  }
  session_.resolved_callees_[&call] =
      resolved_callee{.decl = instance,
                      .owner_module = callee.owner->module_name,
                      .impl_target_type = "",
                      .receiver = callee.receiver_expr,
                      .trait_name = method.trait_name};
  return instance;
}

auto checker::name_member_solution(const ast::call_expr &call,
                                   const call_callee &callee,
                                   const param_subst &block,
                                   const generic_solution &solution,
                                   bool check_bounds) -> void {
  const auto target = member_target(callee, block);
  auto open = std::vector<type_id>{};
  if (mentions_type_var(target)) {
    open.push_back(target);
  }
  for (const auto &[name, type] : solution.type_slots) {
    if (mentions_type_var(type)) {
      open.push_back(type);
    }
  }
  if (open.empty()) {
    const auto targetless =
        target == k_unknown_type && !member_needs_block_instance(callee);
    if (targetless || (!mentions_type_param(target) &&
                       !session_.types_.is_unknown(target))) {
      (void)name_member_instance(call, callee, target, block, solution,
                                 check_bounds);
    }
    return;
  }
  // Solved to an open leaf: `box.make(40)` knows its type as `box[?]` now,
  // and `xs.len()` on a `list[?a]` its receiver as no more than that, but
  // the instance is named once the leaf settles, and its bounds are checked
  // against what it settles to.
  defer_method_call(
      callee.decl->name, session_.types_.tuple_of(std::move(open)),
      std::vector<type_id>{},
      [this, &call, callee, block, solution](type_id) -> void {
        auto settled_callee = callee;
        auto settled_block = param_subst{};
        if (callee.receiver == receiver_use::self_param) {
          // The settled receiver's own method and block, not the open
          // one's: `list[?a]` and `list[int32]` are different entries, and
          // a type with one impl per instantiation has one method each.
          const auto self_type = leaf_ctxt_.zonk(callee.self_type);
          if (mentions_type_var(self_type)) {
            return;
          }
          const auto *method =
              resolve_method(self_type, callee.decl->name).method;
          if (method == nullptr) {
            return;
          }
          settled_callee.self_type = self_type;
          settled_callee.method = method;
          settled_callee.decl = method->decl;
          settled_callee.owner = method->owner;
          settled_callee.decl_file = method->file_id;
          settled_callee.target_name =
              std::string(session_.types_.entry(self_type).name);
          if (mentions_type_param(self_type)) {
            // Settled to the enclosing template's own parameters
            // (`list[?a]` becoming `list[T]`): each instance names its own.
            // The template's record names the template, with its
            // arguments solved here first: the block's from the settled
            // receiver, the method's own from the settled solution.
            auto template_block = param_subst{};
            if (method->block_type_params != nullptr) {
              match_params(method->impl_target_pattern, self_type,
                           template_block);
              solve_impl_value_params(*method, self_type, template_block);
            }
            auto record =
                resolved_callee{.decl = method->decl,
                                .owner_module = method->owner->module_name,
                                .impl_target_type = settled_callee.target_name,
                                .receiver = callee.receiver_expr,
                                .trait_name = method->trait_name};
            const auto own = method->decl->type_params.empty()
                                 ? std::optional<generic_solution>{solution}
                                 : settled_solution(*method->decl, solution);
            if (own.has_value()) {
              if (auto args = template_call_args(settled_callee,
                                                 template_block, *own)) {
                record.template_args = std::move(*args);
              }
            }
            session_.resolved_callees_[&call] = std::move(record);
            defer_call(call, settled_callee, std::nullopt);
            return;
          }
          if (method->block_type_params != nullptr) {
            match_params(method->impl_target_pattern, self_type,
                         settled_block);
            solve_impl_value_params(*method, self_type, settled_block);
          }
        } else {
          for (const auto &[id, type] : block) {
            const auto settled = leaf_ctxt_.zonk(type);
            if (mentions_type_var(settled)) {
              return;
            }
            settled_block.emplace(id, settled);
          }
        }
        auto settled = solution;
        if (!callee.decl->type_params.empty()) {
          const auto mapped = settled_solution(*callee.decl, solution);
          if (!mapped.has_value()) {
            return;
          }
          settled = *mapped;
        }
        name_member_solution(call, settled_callee, settled_block, settled,
                             /*check_bounds=*/true);
      });
}

auto checker::defer_call(const ast::call_expr &call, const call_callee &callee,
                         std::optional<generic_solution> solution) -> void {
  defer_to_instances(call, [this, &call, callee, solution = std::move(solution)](
                               instance_subst &subst) -> void {
    const auto &clone = *clone_of(subst, &call);
    replay_call(clone, callee, solution, subst);
    check_replayed_args(call, clone, subst);
  });
}

auto checker::template_call_args(const call_callee &callee,
                                 const param_subst &block,
                                 const generic_solution &solution)
    -> std::optional<std::vector<type_id>> {
  const auto &decl = *callee.decl;
  if (has_unannotated_params(decl)) {
    return std::nullopt;
  }
  // A receiver reached through `deref` (`guard.push(v)` on a
  // `mutex_guard[list[T]]`) can be built only when the template recorded
  // its steps, which it does for steps through a `deref` at known
  // arguments (`lookup_through_deref`). Otherwise each instance resolves
  // its own.
  if (const auto *receiver = callee.receiver_expr; receiver != nullptr) {
    if (deref_step_count(receiver) != 0 &&
        !session_.deref_adjustments_.contains(receiver)) {
      return std::nullopt;
    }
    if (const auto *path = ast::as<const ast::module_path_expr>(receiver);
        path != nullptr && !session_.path_deref_adjustments_.contains(path) &&
        std::ranges::any_of(path_deref_step_counts(*path),
                            [](std::size_t count) { return count != 0; })) {
      return std::nullopt;
    }
  }
  auto args = std::vector<type_id>{};
  const auto add = [&](std::optional<type_id> answer) -> bool {
    if (!answer.has_value()) {
      return false;
    }
    const auto settled = settle(*answer);
    if (!is_settled_type(settled)) {
      return false;
    }
    args.push_back(settled);
    return true;
  };
  const auto from_block = [&](const ast::type_param &param)
      -> std::optional<type_id> {
    const auto found = block.find(param_id(param));
    return found == block.end() ? std::nullopt
                                : std::optional<type_id>{found->second};
  };
  if (callee.form == callee_form::member && callee.method != nullptr &&
      callee.method->block_type_params != nullptr) {
    for (const auto &param : *callee.method->block_type_params) {
      if (!param.name.empty() && !add(from_block(param))) {
        return std::nullopt;
      }
    }
  }
  for (const auto &param : decl.type_params) {
    if (param.name.empty()) {
      continue;
    }
    auto answer = std::optional<type_id>{};
    if (const auto it = solution.type_slots.find(param.name);
        it != solution.type_slots.end()) {
      answer = it->second;
    } else if (const auto at = solution.const_slots.find(param.name);
               at != solution.const_slots.end()) {
      answer = at->second;
    } else {
      // A receiver-style member's own parameter its receiver answered.
      answer = from_block(param);
    }
    if (!add(answer)) {
      return std::nullopt;
    }
  }
  return args;
}

auto checker::dispatch_template_args(const method_entry &method,
                                    type_id target,
                                    const generic_solution &own)
    -> std::vector<type_id> {
  if (function_.current_template_ == nullptr ||
      !mentions_template_param(target)) {
    return {};
  }
  auto block = param_subst{};
  if (method.block_type_params != nullptr) {
    match_params(method.impl_target_pattern, target, block);
    solve_impl_value_params(method, target, block);
  }
  auto args = template_call_args(
      call_callee{.form = callee_form::member,
                  .decl = method.decl,
                  .owner = method.owner,
                  .decl_file = method.file_id,
                  .method = &method},
      block, own);
  return args.value_or(std::vector<type_id>{});
}

auto checker::check_replayed_args(const ast::call_expr &call,
                                  const ast::call_expr &clone,
                                  instance_subst &subst) -> void {
  const auto recorded = session_.resolved_callees_.find(&call);
  if (recorded == session_.resolved_callees_.end() ||
      recorded->second.template_args.empty()) {
    return;
  }
  auto expected = std::vector<type_id>{};
  for (const auto arg : recorded->second.template_args) {
    expected.push_back(settle(substitute_type(arg, subst)));
  }
  replayed_template_calls_.push_back(
      replayed_template_call{.clone = &clone,
                             .callee_template = recorded->second.decl,
                             .expected = std::move(expected)});
}

auto checker::check_replayed_template_args() -> void {
  for (const auto &item : replayed_template_calls_) {
    const auto replayed = session_.resolved_callees_.find(item.clone);
    if (replayed == session_.resolved_callees_.end()) {
      continue;
    }
    const auto *instance = replayed->second.decl;
    const auto tmpl = instance_templates_.find(instance);
    const auto args = instance_generic_args_.find(instance);
    // Another declaration than the template's (an impl per instantiation),
    // or no instance at all: nothing to compare.
    if (tmpl == instance_templates_.end() ||
        tmpl->second != item.callee_template ||
        args == instance_generic_args_.end()) {
      continue;
    }
    assert(item.expected == args->second &&
           "replay_call: an instance's call names an instance whose "
           "arguments are not the template's `template_args` under its "
           "answers");
    (void)args;
  }
  replayed_template_calls_.clear();
}

auto checker::resolve_bracket_types(call_callee &callee) -> void {
  if (callee.decl == nullptr) {
    return;
  }
  const auto &type_params = callee.decl->type_params;
  for (size_t i = 0; i < callee.explicit_args.size() && i < type_params.size();
       ++i) {
    auto &arg = callee.explicit_args[i];
    if (arg.resolved.has_value() || type_params[i].is_value_param) {
      continue;
    }
    if (arg.type != nullptr) {
      arg.resolved = resolve_type(*arg.type, quiet_ctx(current_resolve_ctx()));
    } else if (arg.value != nullptr) {
      // Only a bracket that reads as a type; anything else is left to
      // stage 4, which says why it is not one.
      if (auto named = expr_as_named_type(*arg.value)) {
        const auto &type = *named;
        session_.synthesized_types_.push_back(std::move(named));
        arg.resolved = resolve_type(type, quiet_ctx(current_resolve_ctx()));
      }
    }
    if (arg.resolved.has_value() &&
        (session_.types_.is_unknown(*arg.resolved) ||
         *arg.resolved == k_error_type)) {
      // Unresolved: stage 4 reads the expression and reports it.
      arg.resolved.reset();
    }
  }
}

auto checker::instance_callee(const call_callee &tmpl, instance_subst &subst)
    -> std::optional<call_callee> {
  auto callee = tmpl;
  callee.receiver_expr = clone_of(subst, tmpl.receiver_expr);
  callee.callee_node = clone_of(subst, tmpl.callee_node);
  for (auto &arg : callee.explicit_args) {
    arg.value = clone_of(subst, arg.value);
    if (arg.resolved.has_value()) {
      arg.resolved = substitute_type(*arg.resolved, subst);
    }
  }
  if (tmpl.self_type == k_unknown_type) {
    return callee;
  }
  callee.self_type = substitute_type(tmpl.self_type, subst);
  if (mentions_type_param(callee.self_type)) {
    return std::nullopt;
  }
  // A static (`list[T].new()`) and a receiver-style method keep the member
  // the template found; a self method and a call through a bound reach the
  // method of the instance's receiver, which may be another declaration
  // (an impl per instantiation, or the impl a bound stood for).
  const auto refind = tmpl.form == callee_form::bound_member ||
                      (tmpl.form == callee_form::member &&
                       tmpl.receiver == receiver_use::self_param);
  if (!refind) {
    if (tmpl.form == callee_form::member) {
      callee.target_name = session_.types_.display(callee.self_type);
    }
    return callee;
  }
  const auto target = strip_refs(callee.self_type);
  const auto *method = resolve_method(target, tmpl.decl->name).method;
  if (method == nullptr || callee.callee_node == nullptr ||
      callee.callee_node->kind != ast::node_kind::field_expr) {
    return std::nullopt;
  }
  const auto &field = ast::cast<const ast::field_expr>(*callee.callee_node);
  const auto &entry = session_.types_.entry(target);
  if (tmpl.receiver == receiver_use::none) {
    const auto target_name = session_.types_.display(target);
    return call_callee{.form = callee_form::member,
                       .decl = method->decl,
                       .owner = method->owner,
                       .decl_file = method->file_id,
                       .method = method,
                       .self_type = target,
                       .target_name = target_name,
                       .explicit_args = std::move(callee.explicit_args),
                       .callee_node = &field,
                       .display_name = std::format("{}.{}", target_name,
                                                   method->decl->name)};
  }
  auto found = method_call_callee(*method, field, target,
                                  std::string(entry.name),
                                  std::move(callee.explicit_args));
  if (receiver_fills_first_param(*method, entry)) {
    found.receiver = receiver_use::first_param;
    found.receiver_expr = field.object.get();
  }
  return found;
}

auto checker::replay_call(const ast::call_expr &clone, const call_callee &tmpl,
                          const std::optional<generic_solution> &solution,
                          instance_subst &subst) -> void {
  const auto rebuilt = instance_callee(tmpl, subst);
  if (!rebuilt.has_value()) {
    return;
  }
  const auto &callee = *rebuilt;
  const auto &decl = *callee.decl;

  // A free function is the same declaration in every instance, so its
  // solution is the template's with the instance's answers in it.
  if (callee.form == callee_form::free_function) {
    if (!solution.has_value()) {
      return;
    }
    if (const auto concrete = substitute_solution(decl, *solution, subst)) {
      (void)instantiate_solved_call(clone, decl, callee.owner,
                                    callee.decl_file, *concrete,
                                    callee.receiver_expr);
    }
    return;
  }

  // Stage 5's unannotated-parameter row: a generic block's self method is
  // queued as the template's own call would have been, with the types the
  // instance recorded for its arguments.
  if (callee.receiver == receiver_use::self_param &&
      decl.type_params.empty() && has_unannotated_params(decl) &&
      impl_needs_instance(*callee.method,
                          session_.types_.entry(callee.self_type))) {
    auto call_params = std::vector<type_id>{k_unknown_type};
    const auto mapping = session_.call_argument_mappings_.find(&clone);
    const auto params = signature_params(decl, callee.owner,
                                         /*skip_self=*/true);
    for (size_t i = 0; i < params.size(); ++i) {
      auto type = k_unknown_type;
      if (mapping != session_.call_argument_mappings_.end() &&
          i < mapping->second.args_by_param.size() &&
          mapping->second.args_by_param[i] != nullptr) {
        if (const auto found =
                session_.node_types_.find(mapping->second.args_by_param[i]);
            found != session_.node_types_.end()) {
          type = settle(found->second);
        }
      }
      call_params.push_back(type);
    }
    pending_open_param_calls_.push_back(pending_open_param_call{
        .call = &clone,
        .decl = &decl,
        .owner = callee.owner,
        .decl_file = callee.decl_file,
        .call_params = std::move(call_params),
        .file = file_.file_id_,
        .module = file_.module_,
        .receiver = callee.receiver_expr,
        .method = *callee.method,
        .target_type_name = callee.target_name,
        .self_type = callee.self_type,
        .block_generic = true,
        // The template already stated the call's types; this names only.
        .typed = true});
    return;
  }

  // Stage 1.
  auto sig = callee_signature(callee);
  if (!sig.generic) {
    session_.resolved_callees_[&clone] =
        resolved_callee{.decl = &decl,
                        .owner_module = callee.owner->module_name,
                        .impl_target_type = callee.target_name,
                        .receiver = callee.receiver_expr,
                        .trait_name = callee.method->trait_name};
    return;
  }
  // Stage 4, from the recorded types: a member may be another declaration
  // than the template's (see `instance_callee`), so the template's
  // solution is not its solution.
  auto values = value_bindings{};
  solve_values_from_argument_types(clone, sig.params, values);
  auto bindings = sig.seed;
  for (const auto &[id, type] : sig.receiver_bindings) {
    bindings.try_emplace(id, type);
  }
  solve_from_argument_types(clone, sig.params, bindings);
  auto block = param_subst{};
  if (callee.method->block_type_params != nullptr) {
    for (const auto &type_param : *callee.method->block_type_params) {
      if (type_param.name.empty()) {
        continue;
      }
      const auto found = bindings.find(param_id(type_param));
      if (found == bindings.end()) {
        return;
      }
      block.emplace(found->first, found->second);
    }
  }
  auto own = generic_solution{};
  if (!decl.type_params.empty()) {
    auto solved = solve_call(clone, decl, callee.owner, values, bindings,
                             callee.explicit_args);
    if (!solved.has_value()) {
      return;
    }
    own = std::move(*solved);
  }
  // Stage 5, with the bounds checked at the instance's answers.
  name_member_solution(clone, callee, block, own, /*check_bounds=*/true);
}

auto checker::method_call_callee(const method_entry &method,
                                 const ast::field_expr &field,
                                 type_id self_type, std::string target_name,
                                 explicit_generic_args explicit_args)
    -> call_callee {
  const auto takes_self =
      !method.decl->params.empty() &&
      param_name_of(method.decl->params.front()) == "self";
  return call_callee{
      .form = callee_form::member,
      .decl = method.decl,
      .owner = method.owner,
      .decl_file = method.file_id,
      .method = &method,
      .receiver = takes_self ? receiver_use::self_param : receiver_use::none,
      .receiver_expr = takes_self ? field.object.get() : nullptr,
      .self_type = self_type,
      .target_name = std::move(target_name),
      .explicit_args = std::move(explicit_args),
      .callee_node = &field,
      .display_name = method.decl->name};
}

auto checker::name_call(const ast::call_expr &call, const call_callee &callee,
                        const call_signature &sig,
                        const std::optional<generic_solution> &solution)
    -> type_id {
  if (callee.form == callee_form::bound_member) {
    return name_bound_call(call, callee, sig, solution);
  }
  const auto &decl = *callee.decl;
  const auto *owner = callee.owner;
  const auto member = callee.form == callee_form::member;
  if (solution.has_value() && !member) {
    if (const auto result =
            name_generic_call(call, callee, *solution)) {
      return *result;
    }
  }
  // A member's result is read with its block's parameters in scope and
  // restated under the solution: `-> wrap[T]` from `wrap[int32].new(v)` is a
  // `wrap[int32]`.
  auto subst = sig.seed;
  if (member && solution.has_value()) {
    for (const auto &[id, type] : solution_subst(decl.type_params, *solution)) {
      subst.insert_or_assign(id, type);
    }
  }
  // Under the *declaring* file, not the caller's — see the identical swap
  // in `instantiate_solved_call`: a return type naming something the
  // callee's module imported (`def make() -> option[foo]` over a
  // `use other.foo`) resolves to nothing when read through the caller's
  // own imports, and the call then reaches lowering with no concrete type.
  const auto saved_signature_file = file_.file_id_;
  file_.file_id_ = callee.decl_file;
  // A generic callee reaching here unsolved (and said why): its own
  // parameters in the result stand for nothing, and must not surface in
  // the caller as a second, confusing mismatch against `T`.
  const auto result = open_foreign_params(substitute_solved(
      signature_return_type(decl, owner,
                            member ? callee.method->block_type_params
                                   : nullptr),
      subst));
  file_.file_id_ = saved_signature_file;

  if (member && sig.generic) {
    if (!solution.has_value()) {
      return k_error_type;
    }
    const auto target = member_target(callee, sig.seed);
    const auto in_template_terms =
        mentions_template_param(target) ||
        solution_mentions_template_param(*solution) ||
        std::ranges::any_of(sig.seed, [&](const auto &slot) -> bool {
          return mentions_template_param(slot.second);
        });
    if (function_.current_template_ != nullptr && in_template_terms) {
      // `list[T].new()` in a template: the instance is each instance's,
      // for its own `T`.
      if (solution->bounds_hold) {
        // A bare-named target (`bag.width[int64]()` inside `extend[T]
        // bag[T]`) is replayed on the target the template solved, so each
        // instance seeds the block from its own answers. The arguments
        // need not mention a block parameter the body answered with its
        // own.
        auto deferred = callee;
        if (deferred.self_type == k_unknown_type) {
          deferred.self_type = target;
        }
        defer_call(call, deferred, std::nullopt);
      }
      auto record = resolved_callee{.decl = &decl,
                                    .owner_module = owner->module_name,
                                    .impl_target_type = callee.target_name,
                                    .receiver = callee.receiver_expr,
                                    .trait_name = callee.method->trait_name};
      if (solution->bounds_hold) {
        auto block = sig.seed;
        for (const auto &[id, type] : sig.receiver_bindings) {
          block.try_emplace(id, type);
        }
        if (auto args = template_call_args(callee, block, *solution)) {
          record.template_args = std::move(*args);
        }
      }
      session_.resolved_callees_[&call] = std::move(record);
      return record_expr_type(call, result);
    }
    if (!solution->bounds_hold) {
      return record_expr_type(call, result);
    }
    name_member_solution(call, callee, sig.seed, *solution,
                         /*check_bounds=*/false);
    // A compile-time-only instance has no compiled function for run-time
    // code to call; the call folds to its value.
    if (const auto found = session_.resolved_callees_.find(&call);
        found != session_.resolved_callees_.end() &&
        session_.comptime_only_functions_.contains(found->second.decl)) {
      (void)fold_static_call(call, decl, result, solution->type_slots);
    }
    return record_expr_type(call, result);
  }

  session_.resolved_callees_[&call] = resolved_callee{
      .decl = &decl,
      .owner_module = owner->module_name,
      .impl_target_type = member ? callee.target_name : "",
      .receiver = callee.receiver_expr,
      .trait_name = member ? callee.method->trait_name : ""};
  if (fold_static_call(call, decl, result, /*type_slots=*/{})) {
    return record_expr_type(call, result);
  }
  return open_call_result(call, result);
}

auto checker::name_bound_call(const ast::call_expr &call,
                              const call_callee &callee,
                              const call_signature &sig,
                              const std::optional<generic_solution> &solution)
    -> type_id {
  const auto &decl = *callee.decl;
  if (sig.generic && !solution.has_value()) {
    return k_error_type;
  }
  auto subst = sig.seed;
  if (solution.has_value()) {
    for (const auto &[id, type] : solution_subst(decl.type_params, *solution)) {
      subst.insert_or_assign(id, type);
    }
  }
  // `-> U` from `T.conv[int64](v)` is an `int64` here, as in every instance.
  const auto result = open_foreign_params(substitute_solved(
      in_bound_scope(callee, /*trait_file=*/true,
                     [&] -> type_id {
                       return signature_return_type(decl, callee.owner,
                                                    &callee.trait->type_params);
                     }),
      subst));
  if (callee.callee_node != nullptr) {
    auto param_types = std::vector<type_id>{};
    for (const auto &param : sig.params) {
      param_types.push_back(substitute_solved(param.type, subst));
    }
    record_expr_type(*callee.callee_node,
                     session_.types_.fn_of(std::move(param_types), result));
  }
  if (solution.has_value() && !solution->bounds_hold) {
    return result;
  }
  if (auto record = bound_call_record(callee, solution)) {
    session_.resolved_callees_[&call] = std::move(*record);
  }
  // Which impl's method this is depends on what the parameter turns out to
  // be, so each instance names its own.
  defer_call(call, callee, std::nullopt);
  return result;
}

auto checker::bound_call_record(const call_callee &callee,
                                const std::optional<generic_solution> &solution)
    -> std::optional<resolved_callee> {
  if (callee.decl == nullptr || callee.trait == nullptr) {
    return std::nullopt;
  }
  // The trait's module may be checked after this template.
  if (!trait_method_signatures_.contains(callee.decl)) {
    record_trait_method_signature_early(*callee.trait, *callee.decl,
                                        callee.owner, callee.decl_file);
  }
  if (!trait_method_signatures_.contains(callee.decl)) {
    return std::nullopt;
  }
  auto own = template_call_args(callee, param_subst{},
                                solution.value_or(generic_solution{}));
  if (!own.has_value()) {
    return std::nullopt;
  }
  const auto self_type = settle(callee.self_type);
  if (!is_settled_type(self_type)) {
    return std::nullopt;
  }
  auto trait_args = std::vector<type_id>{};
  for (const auto arg : callee.trait_args) {
    const auto settled = settle(arg);
    if (!is_settled_type(settled)) {
      return std::nullopt;
    }
    trait_args.push_back(settled);
  }
  return resolved_callee{
      .decl = callee.decl,
      .owner_module = callee.owner != nullptr ? callee.owner->module_name : "",
      .receiver = callee.receiver_expr,
      .trait_name = callee.trait->name,
      .template_args = std::move(*own),
      .bound = bound_call{.trait = callee.trait,
                          .self_type = self_type,
                          .trait_args = std::move(trait_args)}};
}

auto checker::check_call(const ast::call_expr &call, const call_callee &found)
    -> type_id {
  auto callee = found;
  // A template's call may be replayed by its instances, which have no
  // scope to read a bracket's `T` in.
  if (function_.current_template_ != nullptr &&
      callee.form != callee_form::fn_value) {
    resolve_bracket_types(callee);
  }
  assert((callee.form != callee_form::free_function ||
          callee.receiver != receiver_use::self_param) &&
         "check_call: a free function takes no `self`");
  assert((callee.form != callee_form::bound_member ||
          (callee.decl != nullptr && callee.trait != nullptr)) &&
         "check_call: a bound member names its trait and method");
  if (callee.form == callee_form::fn_value) {
    // No declaration: nothing to solve or name, and no callee record (the
    // adapter calls the callee node's value).
    call_error_floors_.push_back(errors_emitted_);
    struct pop_floor {
      std::vector<size_t> &floors;
      ~pop_floor() { floors.pop_back(); }
    } const floor_guard{call_error_floors_};
    auto sig = callee_signature(callee);
    (void)check_call_arguments(call, callee, sig);
    return session_.types_.entry(strip_refs(callee.fn_type)).result;
  }
  const auto &decl = *callee.decl;
  const auto *owner = callee.owner;
  const auto first_param = callee.receiver == receiver_use::first_param;
  const auto self_param = callee.receiver == receiver_use::self_param;
  // A bracketed call names its instance's type once it is solved; the
  // template's own `fn` type would be wrong for it. A call the compiler
  // made itself (a `deref` step) has no callee node. A bound member's is
  // recorded by stage 5, under the bound's `self` and the solution.
  if (callee.callee_node != nullptr &&
      callee.form != callee_form::bound_member) {
    record_expr_type(*callee.callee_node,
                     first_param || callee.form == callee_form::member ||
                             callee.explicit_args.empty()
                         ? fn_type_of(decl, owner)
                         : k_unknown_type);
  }
  // Recorded before anything can fail or wait, and replaced by the instance
  // once one is named: a task's carrier rule reads the receiver's mode off
  // this record (`receiver_is_shared_borrow`), and an `async` method call
  // with no record would hide what its task carries.
  if (callee.form == callee_form::member && callee.receiver_expr != nullptr) {
    session_.resolved_callees_[&call] =
        resolved_callee{.decl = &decl,
                        .owner_module = owner->module_name,
                        .impl_target_type = callee.target_name,
                        .receiver = callee.receiver_expr,
                        .trait_name = callee.method->trait_name};
  }

  call_error_floors_.push_back(errors_emitted_);
  struct pop_floor {
    std::vector<size_t> &floors;
    ~pop_floor() { floors.pop_back(); }
  } const floor_guard{call_error_floors_};
  if (function_.in_contract_ && !decl.modifiers.is_pure) {
    error_with_help(
        call.span,
        std::format("contract conditions may only call pure functions, and "
                    "`{}` is not declared `pure`",
                    decl.name),
        "impure call in a contract condition",
        std::format("Mark `{}` as `pure def` if it has no side effects.",
                    decl.name));
  }
  if (decl.modifiers.is_async) {
    session_.async_calls_.insert(&call);
  }

  auto sig = callee_signature(callee);
  if (!bind_explicit_args(call, callee, sig)) {
    infer_call_args_loosely(call);
    return k_error_type;
  }
  const auto solved = check_call_arguments(call, callee, sig);
  // The arguments may have pinned a receiver that was open: `out.push(i)`
  // on a `list[?a]` solves `?a` from `i`, and the block is solved as what
  // the receiver now is.
  for (auto &slot : sig.seed) {
    slot.second = settle(slot.second);
  }

  // A self method with an unannotated parameter is elaborated the same way
  // once its body has said what the parameter is, for this receiver; a
  // receiver written in the enclosing template's parameters is named by
  // each instance instead.
  if (callee.form == callee_form::member && self_param &&
      has_unannotated_params(decl) && !is_generic_template(decl) &&
      !mentions_template_param(settle(callee.self_type))) {
    // `params` leaves `self` out; the record indexes by declaration. They
    // are read under the receiver's block bindings, as the call saw them.
    auto param_types = std::vector<type_id>{k_unknown_type};
    for (const auto &param : sig.params) {
      param_types.push_back(param.type);
    }
    const auto block_generic = member_needs_block_instance(callee);
    queue_open_param_call(pending_open_param_call{
        .call = &call,
        .decl = &decl,
        .owner = owner,
        .decl_file = callee.decl_file,
        .call_params = std::move(param_types),
        .file = file_.file_id_,
        .module = file_.module_,
        .receiver = callee.receiver_expr,
        .method = *callee.method,
        .target_type_name = callee.target_name,
        .self_type = strip_refs(settle(callee.self_type)),
        .block_generic = block_generic,
        .probe = probing_decl_});
    mint_open_result(decl, call.span);
    if (block_generic) {
      // The instance is named for the receiver and the body's answer
      // together (`resolve_open_param_calls`), not here.
      const auto saved_file = std::exchange(file_.file_id_, callee.decl_file);
      const auto result = substitute_solved(
          signature_return_type(decl, owner, callee.method->block_type_params),
          sig.seed);
      file_.file_id_ = saved_file;
      return open_call_result(call, result);
    }
  }

  // A call to a function with an unannotated parameter is elaborated only
  // once that function's own body has said what the parameter is — see
  // `resolve_open_param_calls`. `all_params` carries this call's private
  // copy of each still-open parameter leaf, already tied to the arguments;
  // a receiver is the callee's first argument, so it is tied to that
  // parameter's leaf here.
  if (callee.form == callee_form::free_function &&
      has_unannotated_params(decl) && is_free_function(decl, owner)) {
    if (first_param) {
      solve_leaves(sig.receiver_param->type, callee.self_type);
    }
    auto param_types = std::vector<type_id>{};
    param_types.reserve(sig.all_params.size());
    for (const auto &param : sig.all_params) {
      param_types.push_back(param.type);
    }
    queue_open_param_call(
        pending_open_param_call{.call = &call,
                                .decl = &decl,
                                .owner = owner,
                                .decl_file = callee.decl_file,
                                .call_params = std::move(param_types),
                                .file = file_.file_id_,
                                .module = file_.module_,
                                .receiver = callee.receiver_expr,
                                .probe = probing_decl_});
    mint_open_result(decl, call.span);
  }

  // A call whose solution is written in the caller's own parameters
  // (`get(v, i)` inside a function generic over `n` passes an
  // `array[int32, n]`; `wrap(x)` inside one generic over `T` passes a `T`)
  // names no instance; stage 5 defers it to the caller's instances. One
  // whose solution is concrete names its instance there, template or not;
  // one whose receiver or arguments are still open (`xs.iter()` on a
  // `list[?a]` a later statement will pin) names it once they settle.
  const auto solution = solve_call_callee(call, callee, sig, solved);
  return name_call(call, callee, sig, solution);
}

// ==========================================================================
//  Calls
// ==========================================================================

/// Hard-codes the result types of the handful of prelude methods the
/// checker knows about for `list[T]`, `str`, `option[T]`, and `result[T, E]`
/// — a stopgap until the standard library itself is represented as
/// checkable declarations. Returns `unknown` for any other object kind or
/// unrecognized method name, which is silently accepted rather than
/// reported as an error.
/// The type `object.name(...)` evaluates to for a builtin inherent
/// method, or `k_unknown_type` when `name` is not one — which is what lets
/// the caller fall through to `extend`/`impl` methods and UFCS.
auto checker::builtin_method_result(const type_entry &object,
                                    std::string_view name) -> type_id {
  const auto owner = builtin_method_owner(object);
  if (owner.empty()) {
    return k_unknown_type;
  }
  // `array[T, N]` keeps its element type in `result`, not `args`, and a
  // `str` is a block of `byte` with no element type recorded at all.
  const auto element =
      object.kind == type_kind::array_kind ? object.result
      : (object.kind == type_kind::builtin_kind && object.name == "str")
          ? session_.types_.builtin("byte")
      : object.args.empty() ? k_unknown_type
                            : object.args[0];
  for (const auto &method : k_builtin_methods) {
    if (method.owner != owner || method.name != name) {
      continue;
    }
    switch (method.result) {
    case builtin_result_shape::usize_result:
      return session_.types_.builtin("usize");
    case builtin_result_shape::bool_result:
      return session_.types_.builtin("bool");
    case builtin_result_shape::unit_result:
      return session_.types_.builtin("unit");
    case builtin_result_shape::element:
      return element;
    case builtin_result_shape::option_of_element:
      return session_.types_.builtin_generic("option", {element});
    case builtin_result_shape::byte_slice:
      return session_.types_.builtin_generic("slice",
                                             {session_.types_.builtin("byte")});
    case builtin_result_shape::ptr_to_element:
      return session_.types_.ptr_to(element, false);
    case builtin_result_shape::mut_ptr_to_element:
      return session_.types_.ptr_to(element, true);
    }
  }
  return k_unknown_type;
}

/// Reports `receiver.name(...)` naming no method that exists on a *builtin*
/// receiver, once every lookup — builtin inherent, `impl`, `extend`, UFCS —
/// has failed.
///
/// This case used to fall through silently, returning `k_unknown_type` with
/// no diagnostic at all: `nums.fliter(...)` on a `list[int32]` compiled, and
/// whatever it "produced" unified with everything downstream because
/// `k_unknown_type` deliberately does. A user-defined receiver has always
/// reported "no method `x` on type `y`" here; a builtin one said nothing,
/// which is exactly backwards — `list`, `str` and `option` are the types a
/// beginner meets first.
///
/// Returns `nullopt` rather than reporting when the receiver is a type
/// whose method set is not actually known. `k_unknown_type` is the whole
/// mechanism by which one gap in inference avoids cascading into unrelated
/// errors, and a type parameter's methods depend on bounds resolved
/// elsewhere — reporting on either would turn a silent gap into a wrong
/// error, which is worse than the gap.
auto checker::report_unknown_builtin_method(const ast::call_expr &call,
                                            const ast::field_expr &field,
                                            const type_entry &entry,
                                            type_id object)
    -> std::optional<type_id> {
  // Restricting to these two kinds is what keeps an unresolved receiver
  // silent: `k_unknown_type` is `unknown_kind`, and a type parameter is
  // `type_param_kind`, so neither reaches the report below.
  if (entry.kind != type_kind::builtin_kind &&
      entry.kind != type_kind::builtin_generic_kind) {
    return std::nullopt;
  }

  build_method_table();
  auto candidates = builtin_method_names(entry);
  for (auto &declared : method_lookup().declared_names(entry)) {
    candidates.push_back(std::move(declared));
  }
  std::ranges::sort(candidates);
  candidates.erase(std::ranges::unique(candidates).begin(), candidates.end());

  const auto display = session_.types_.display(object);
  auto diag = diagnostic(
      diagnostic_level::error,
      std::format("no method `{}` on type `{}`", field.field_name, display),
      file_.file_id_);
  diag.with_label(field.span, "method not found");
  const auto is_wrapper = entry.name == "option" || entry.name == "result";
  if (is_wrapper && field.field_name == "unwrap") {
    // The operator replaced the method (`x or else y`).
    diag.with_help(std::format(
        "`{}` has no `unwrap`. Write `x or else panic(\"...\")` to stop when "
        "there is no value, or `x or else <fallback>` to use another value.",
        entry.name));
  } else if (is_wrapper && field.field_name == "unwrap_or") {
    diag.with_help(std::format(
        "`{}` has no `unwrap_or`. Write `x or else <fallback>` to use another "
        "value when there is none.",
        entry.name));
  } else if (const auto suggestion =
                 best_suggestion(field.field_name, candidates)) {
    diag.with_help(std::format("did you mean `{}`?", *suggestion));
  } else if (!candidates.empty()) {
    auto listed = std::string{};
    for (const auto &name : candidates) {
      if (!listed.empty()) {
        listed += ", ";
      }
      listed += std::format("`{}`", name);
    }
    diag.with_note(std::format("`{}` provides {}", display, listed));
    diag.with_help(std::format(
        "A builtin type's methods come from the compiler itself and from "
        "`extend {}:` blocks in the standard library. To add one of your "
        "own, write a free function whose first parameter is `{}` — it is "
        "then callable as a method.",
        entry.name, display));
  } else {
    diag.with_help(std::format(
        "`{}` has no methods. A free function whose first parameter is `{}` "
        "is callable as a method on one.",
        display, display));
  }
  emit_diag(diag);
  mark_error();
  infer_call_args_loosely(call);
  return k_error_type;
}

/// Every inherent method name a builtin receiver answers to, for the "did
/// you mean" suggestion on an unknown method.
auto checker::builtin_method_names(const type_entry &object)
    -> std::vector<std::string> {
  const auto owner = builtin_method_owner(object);
  auto names = std::vector<std::string>{};
  if (owner.empty()) {
    return names;
  }
  for (const auto &method : k_builtin_methods) {
    if (method.owner == owner) {
      names.emplace_back(method.name);
    }
  }
  return names;
}

/// Every method name a user type declares, sorted and deduplicated — the
/// pool both the "provides the methods ..." note and the "did you mean"
/// suggestion on an unknown-method error are drawn from.
auto checker::available_method_name_list(const type_entry &instance)
    -> std::vector<std::string> {
  build_method_table();
  auto names = method_lookup().declared_names(instance);
  std::ranges::sort(names);
  names.erase(std::ranges::unique(names).begin(), names.end());
  return names;
}

/// Builds the comma-separated, sorted, deduplicated method-name list
/// used in the "provides the methods ..." note on an unknown-method error.
auto checker::available_method_names(const type_entry &instance)
    -> std::string {
  const auto names = available_method_name_list(instance);
  auto out = std::string{};
  for (const auto &name : names) {
    if (!out.empty()) {
      out += ", ";
    }
    out += std::format("`{}`", name);
  }
  return out;
}

/// A method call's instance, reachable from a site that is not a call at
/// all.
///
/// A method reached through an `impl` over a generic target has no compiled
/// form until some receiver pins the target down — that is what the
/// per-receiver instance discipline exists for. String interpolation
/// reaches such a method without an `ast::call_expr` anywhere: `"{v}"`
/// dispatches to `v`'s `show` through `interp_dispatch`. Recording the
/// *template* there would let `hir::lower` name it `wrap::show`, a function
/// nothing compiles.
///
/// Everything the call pipeline does with the call itself (argument
/// checking, preconditions, recording a `resolved_callee`) is absent here,
/// because an interpolated segment supplies no arguments: `show`/`debug`/
/// `hex`/`octal`/`binary` all take `self` alone. `site` is used only as the
/// node instantiation diagnostics point at.
///
/// `nullptr` means "no instance is needed or possible", and the caller
/// should keep the declaration it already had.
/// `discriminator` distinguishes two instances that would otherwise
/// mangle alike: `index[usize]` and `index[range[usize]]` both contribute
/// an `at` for the same receiver, so the receiver type alone does not name
/// the instance. Empty for every other caller.
auto checker::instantiate_impl_method_for(const ast::node &site,
                                          const method_entry &method,
                                          const type_entry &receiver_entry,
                                          type_id receiver_type,
                                          std::string_view discriminator)
    -> const ast::func_decl * {
  if (!impl_needs_instance(method, receiver_entry) ||
      mentions_template_param(receiver_type)) {
    return nullptr;
  }
  auto block = param_subst{};
  match_params(method.impl_target_pattern, receiver_type, block);
  solve_impl_value_params(method, receiver_type, block);
  for (const auto &type_param : *method.block_type_params) {
    if (!type_param.name.empty() && !block.contains(param_id(type_param))) {
      // An impl parameter the receiver doesn't pin. The call-shaped sibling
      // reports here; this path stays silent and falls back, since an
      // interpolation is not where a user would act on that diagnostic.
      return nullptr;
    }
  }
  // The naming stage a method call reaches, so an operator, interpolation
  // or loop names the same instance a written call would.
  const auto callee = call_callee{.form = callee_form::member,
                                  .decl = method.decl,
                                  .owner = method.owner,
                                  .decl_file = method.file_id,
                                  .method = &method,
                                  .receiver = receiver_use::self_param,
                                  .self_type = receiver_type,
                                  .target_name = receiver_entry.name,
                                  .display_name = method.decl->name};
  return member_instance(
      site, callee, receiver_type, block,
      member_instance_solution(callee, receiver_type, block,
                               generic_solution{}, discriminator));
}

/// Whether calling `method` on a receiver of type `receiver` needs a
/// monomorphized instance of its own, rather than resolving to the single
/// compiled `Target::method` an ordinary impl member gets.
///
/// Two shapes need one, and both are invisible on the method's own
/// declaration:
///
///   - `impl[T] get_it[T] for holder[T]` — the impl is generic, so the
///     method's signature and body are written in terms of `T` and mean
///     something different for every receiver.
///   - `impl get_it[int32] for holder[int32]` — the impl is *not* generic,
///     but its target is an instantiation, so `holder[int32]` and
///     `holder[str]` may each have an impl and both would claim the name
///     `holder::get`.
///
/// In both cases there is no one function to compile, which is why
/// `hir::lower_impl_associated_functions` historically emitted nothing at
/// all for either (design §2.1's G0).
auto checker::impl_needs_instance(const method_entry &method,
                                  const type_entry &receiver) -> bool {
  return method.block_type_params != nullptr &&
         (!method.block_type_params->empty() || !receiver.args.empty());
}

/// Fills in `bindings` for an impl-block *value* parameter. `bindings` is
/// keyed by parameter id, but a value parameter such as `n: usize` embedded
/// in a target like `buf[n]` is a polynomial variable (see
/// `resolve_value_arg`/`name_poly`), so its answer arrives in the match's
/// value solutions rather than its type bindings. This reads it from there
/// — the same `match_value_params` free functions use, against the impl's
/// target pattern instead of a parameter list — and binds the constant.
auto checker::solve_impl_value_params(const method_entry &method,
                                      type_id concrete, param_subst &bindings)
    -> void {
  auto solved = value_bindings{};
  match_value_params(method.impl_target_pattern, concrete, solved);
  for (const auto &type_param : *method.block_type_params) {
    if (type_param.name.empty() || !type_param.is_value_param ||
        bindings.contains(param_id(type_param))) {
      continue;
    }
    const auto found = solved.find(value_var(param_id(type_param)));
    if (found == solved.end() || !found->second.is_constant()) {
      continue;
    }
    if (const auto underlying =
            value_param_underlying(type_param, method.owner)) {
      bindings.emplace(
          param_id(type_param),
          session_.types_.const_value(
              *underlying, static_cast<uint64_t>(found->second.constant)));
    }
  }
}

/// Copies each value-kind impl-block parameter's binding from `bindings`
/// into `solution`'s const-slots, so `find_or_check_generic_instance`
/// also lands it in `session_.const_param_values_` — what
/// `record_const_param_reference` reads to embed `n`'s concrete value as
/// a literal wherever the body reads it as a value — rather than only
/// substituting it into the checked signature's types.
auto checker::carry_impl_value_slots(const method_entry &method,
                                     const param_subst &bindings,
                                     generic_solution &solution) -> void {
  for (const auto &type_param : *method.block_type_params) {
    if (!type_param.is_value_param) {
      continue;
    }
    const auto found = bindings.find(param_id(type_param));
    if (found == bindings.end()) {
      continue;
    }
    const auto &entry = session_.types_.entry(found->second);
    if (entry.kind != type_kind::const_value_kind) {
      continue;
    }
    solution.const_slots.emplace(type_param.name, found->second);
    solution.values.emplace(type_param.name, entry.value.constant);
  }
}

/// Whether `method` is *receiver-style* for an `instance` of some
/// constructor: declared without `self`, but with a first parameter that
/// is an application of the very constructor the receiver instantiates —
/// the shape a higher-kinded trait's methods take (`def bind[A, B](ma:
/// M[A], ...)` under `impl monad for option` has first parameter
/// `option[A]`). Method-call syntax passes the receiver as that first
/// argument. A non-`self` method whose first parameter is anything else
/// (an associated function like `from(errno)`) keeps its historical
/// call-shape, where the written arguments fill every parameter.
auto checker::receiver_fills_first_param(const method_entry &method,
                                         const type_entry &instance) -> bool {
  if (method.decl->params.empty() ||
      param_name_of(method.decl->params.front()) == "self") {
    return false;
  }
  const auto params = signature_params(*method.decl, method.owner, false);
  if (params.empty()) {
    return false;
  }
  const auto &first = session_.types_.entry(strip_refs(params.front().type));
  if (first.decl != nullptr || instance.decl != nullptr) {
    return first.decl != nullptr && first.decl == instance.decl;
  }
  return first.kind == type_kind::builtin_generic_kind &&
         instance.kind == type_kind::builtin_generic_kind &&
         first.name == instance.name;
}

/// Types a method-call expression `object.method(args...)`: resolves
/// `object`'s type, then tries (in order) a user-declared method, a
/// `deriving`/prelude-trait-derived method, a callable struct field, or a
/// builtin-type method — reporting an unknown-method error only for
/// struct/sum/opaque objects, since builtin methods are best-effort.
/// The compile-time arguments a method call wrote after the method name
/// (`b.take[int64](5)`). Unlike the expression-position brackets of
/// `zeros[8]()`, these are parsed as types outright — the grammar spells
/// the suffix as a `type_arg_list` — so they need no reinterpretation.
auto checker::method_explicit_generic_args(const ast::field_expr &field)
    -> explicit_generic_args {
  auto args = explicit_generic_args{};
  args.reserve(field.generic_args.size());
  for (const auto &arg : field.generic_args) {
    if (arg != nullptr) {
      args.push_back(
          explicit_generic_arg{.type = arg.get(), .span = arg->span});
    }
  }
  return args;
}

/// Rejects brackets on a method that has no compile-time parameters to
/// fill (§3.3). The shape can't mean indexing — that would index the
/// call's *result*, which is written `m(...)[i]` — so there is nothing
/// sensible to fall back to, and silently ignoring it is the bug this
/// whole section exists to fix.
auto checker::check_method_accepts_generic_args(
    source_span span, const ast::func_decl &decl,
    const explicit_generic_args &explicit_args) -> bool {
  if (explicit_args.empty() || !decl.type_params.empty()) {
    return true;
  }
  error_with_help(
      span,
      std::format("`{}` takes no compile-time parameters, and this call "
                  "gives it {} in brackets",
                  decl.name, explicit_args.size()),
      "no compile-time parameters to fill",
      std::format(
          "`{}` is declared without a `[...]` parameter list, so there is "
          "nothing for these brackets to name. Drop them. (To index what "
          "the call returns, put the brackets after it: `.{}( ... )[i]`.)",
          decl.name, decl.name));
  return false;
}

} // namespace cinder::semantic::detail
