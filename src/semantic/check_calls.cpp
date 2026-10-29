#include "checker.h"
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
/// `check_call_against_decl` can monomorphize the callee with them.
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
  const auto check_argument = [&](const pending_argument &item,
                                  const param_subst &bindings) -> void {
    const auto declared = item.target != nullptr
                              ? substitute_solved(item.target->type, bindings)
                              : k_unknown_type;
    const auto expected =
        open_foreign_params(substitute_solved(declared, opened));
    const auto found = infer_expr(*item.value, expected);
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
    if (!session_.types_.compatible(settle(expected), settle(found))) {
      session_.mismatched_argument_calls_.insert(&call);
    }
    type_mismatch(item.value->span, shown, found, "for this argument");
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
  auto no_bindings = param_subst{};
  for (const auto &item : pending) {
    if (item.value->kind != ast::node_kind::lambda_expr) {
      check_argument(item, no_bindings);
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
      check_argument(item, bindings);
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

/// Checks a call against a known `func_decl`: enforces the
/// contract-purity rule when inside a contract condition, checks its
/// arguments via `check_call_args_against`, and returns its return type.
///
/// `explicit_args` are the compile-time arguments the call gave in brackets
/// (`zeros[8]()`), empty for the ordinary bare-callee call.
///
/// `method_site` is set for a call to a `self`-taking method of a
/// non-generic `impl`/`extend` block, so a method with an unannotated
/// parameter is instantiated per call like a free function is.
auto checker::check_call_against_decl(
    const ast::call_expr &call, const ast::func_decl &decl,
    const module_members *owner, file_id_type decl_file, bool skip_self,
    const explicit_generic_args &explicit_args,
    const method_call_site *method_site) -> type_id {
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
  const auto params = signature_params(decl, owner, skip_self);
  auto solved = value_bindings{};
  const auto generic = generic_call_context{.decl = &decl,
                                            .owner = owner,
                                            .explicit_args = &explicit_args,
                                            .ufcs_receiver = nullptr};
  check_call_args_against(
      call, params, decl.name,
      source_location{.file_id = decl_file, .span = decl.span}, &solved,
      is_generic_template(decl) ? &generic : nullptr);
  check_call_preconditions(call, decl, params);
  // A call to a function with an unannotated parameter is elaborated only
  // once that function's own body has said what the parameter is — see
  // `resolve_open_param_calls`. `params` carries this call's private copy
  // of each still-open parameter leaf, already tied to the arguments.
  const auto is_free_call = !skip_self && is_free_function(decl, owner);
  const auto is_method_call =
      skip_self && method_site != nullptr &&
      !mentions_template_param(settle(method_site->receiver_type));
  if (has_unannotated_params(decl) && (is_free_call || is_method_call)) {
    auto param_types = std::vector<type_id>{};
    param_types.reserve(params.size() + 1);
    if (is_method_call) {
      // `params` leaves `self` out; the record indexes by declaration.
      param_types.push_back(k_unknown_type);
    }
    for (const auto &param : params) {
      param_types.push_back(param.type);
    }
    auto pending = pending_open_param_call{
        .call = &call,
        .decl = &decl,
        .owner = owner,
        .decl_file = is_method_call ? method_site->method->file_id : decl_file,
        .call_params = std::move(param_types),
        .file = file_.file_id_,
        .module = file_.module_,
        .probe = probing_decl_};
    if (is_method_call) {
      pending.receiver = method_site->receiver;
      pending.method = *method_site->method;
      pending.target_type_name = std::string(method_site->target_type_name);
      pending.self_type = strip_refs(settle(method_site->receiver_type));
    }
    queue_open_param_call(std::move(pending));
    mint_open_result(decl, call.span);
  }
  // A call whose solution is written in the caller's own parameters
  // (`get(v, i)` inside a function generic over `n` passes an
  // `array[int32, n]`; `wrap(x)` inside one generic over `T` passes a `T`)
  // names no instance; `instantiate_generic_function` defers it to the
  // caller's instances. One whose solution is concrete names its instance
  // here, template or not.
  //
  // Value parameters and type parameters take the same road, together: a
  // template mixing them (`[n: usize, T]`) is one solution with both kinds
  // of binding in it, not a third case.
  // `is_static` alongside `is_free_function`: a `static def` in an
  // `extend`/`impl` block is not in `owner->functions`, so it failed this
  // test and was never monomorphized. A generic one then reached lowering
  // as its uncompiled *template*, and both backends reported "call to
  // `trio::from_array` could not be resolved to a function in this
  // compiled module" — a codegen failure for correct code. A static method
  // takes no receiver, so its instance is named and dispatched exactly
  // like a free function's.
  if (is_generic_template(decl) &&
      (is_free_function(decl, owner) || decl.modifiers.is_static)) {
    const auto result = instantiate_generic_function(
        call, decl, owner, decl_file, solved, params, explicit_args);
    if (result.has_value()) {
      return *result;
    }
  }
  // Under the *declaring* file, not the caller's — see the identical swap
  // in `instantiate_generic_function`: a return type naming something the
  // callee's module imported (`def make() -> option[foo]` over a
  // `use other.foo`) resolves to nothing when read through the caller's
  // own imports, and the call then reaches lowering with no concrete type.
  const auto saved_signature_file = file_.file_id_;
  file_.file_id_ = decl_file;
  // A generic callee reaching here was not solved (and said why): its own
  // parameters in the result stand for nothing, and must not surface in
  // the caller as a second, confusing mismatch against `T`.
  const auto result = open_foreign_params(signature_return_type(decl, owner));
  file_.file_id_ = saved_signature_file;
  return open_call_result(call, result);
}

// ==========================================================================
//  Calls
// ==========================================================================

/// Checks a call against a `fn(...)`-typed callable value: arity must
/// match exactly (no named arguments or defaults for a bare fn type), and
/// each argument's type is checked positionally.
auto checker::check_call_against_fn_type(const ast::call_expr &call,
                                         const type_entry &fn_entry,
                                         std::string_view callee_name)
    -> type_id {
  if (call.args.size() != fn_entry.args.size()) {
    error(call.span,
          std::format("wrong number of arguments to `{}`: expected {}, "
                      "found {}",
                      callee_name, fn_entry.args.size(), call.args.size()),
          "argument count does not match the function type");
    infer_call_args_loosely(call);
    return fn_entry.result;
  }
  for (size_t i = 0; i < call.args.size(); ++i) {
    if (call.args[i].value == nullptr) {
      continue;
    }
    const auto expected = fn_entry.args[i];
    const auto found = infer_expr(*call.args[i].value, expected);
    type_mismatch(call.args[i].value->span, expected, found,
                  "for this argument", call.args[i].value.get());
  }
  return fn_entry.result;
}

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
    case builtin_result_shape::cell_of_element:
      return session_.types_.builtin_generic("cell", {element});
    case builtin_result_shape::option_of_mut_cell_of_element:
      return session_.types_.builtin_generic(
          "option", {session_.types_.builtin_generic("cell_mut", {element})});
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
  if (const auto suggestion = best_suggestion(field.field_name, candidates)) {
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

/// Records a `session_.resolved_callees_` entry for a genuine instance-method
/// call
/// (`object.method(args...)`, `method` a real `self`-taking declaration
/// found via `resolve_method`) so
/// `hir::lower_call` can rebuild the call site directly instead of
/// mishandling `field` as plain field access — see `resolved_callee`'s
/// doc comment (`semantic/types.h`) for why `receiver` matters. A method
/// with no `self` parameter (an associated function reached through
/// `find_method`, e.g. a type-constant lookup) has no receiver to record
/// and is left alone; `infer_qualified_call` is what actually resolves
/// those call shapes.
/// `check_impl_generic_method_call`'s instantiation step, reachable from a
/// site that is not a call at all.
///
/// A method reached through an `impl` over a generic target has no compiled
/// form until some receiver pins the target down — that is what the
/// per-receiver instance discipline exists for. String interpolation
/// reaches such a method without an `ast::call_expr` anywhere: `"{v}"`
/// dispatches to `v`'s `show` through `interp_dispatch`, which recorded the
/// *template* and let `hir::lower` name it `wrap::show`. No function ever
/// bore that name, so the call type-checked and then failed to compile —
/// the same split `check_impl_generic_method_call` exists to prevent, just
/// arrived at from the other direction.
///
/// Everything the call-shaped sibling does with the call itself (argument
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
/// the instance. Empty for every other caller, which keeps their names
/// byte-identical to before.
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
  auto bindings = param_subst{};
  match_params(method.impl_target_pattern, receiver_type, bindings);
  solve_impl_value_params(method, receiver_type, bindings);
  for (const auto &type_param : *method.block_type_params) {
    if (!type_param.name.empty() && !bindings.contains(param_id(type_param))) {
      // An impl parameter the receiver doesn't pin. The call-shaped sibling
      // reports here; this path stays silent and falls back, since an
      // interpolation is not where a user would act on that diagnostic.
      return nullptr;
    }
  }
  auto scoped_params = method.fixed_type_params;
  scoped_params.merge(bindings_by_name(bindings));
  auto solution = generic_solution{};
  carry_impl_value_slots(method, bindings, solution);
  solution.suffix = std::format(
      "${}{}", mangle_type_for_instance(receiver_type), discriminator);
  const auto name = method_instance_name(receiver_entry.name, method.decl->name,
                                         solution.suffix);
  return find_or_check_generic_instance(
      site, *method.decl, method.owner, method.file_id, solution, name,
      &scoped_params, receiver_type, method.block_type_params);
}

auto checker::record_instance_method_callee(const ast::call_expr &call,
                                            const method_entry &method,
                                            std::string_view target_type_name,
                                            const ast::expr &receiver) -> void {
  if (method.decl->params.empty() ||
      param_name_of(method.decl->params.front()) != "self") {
    return;
  }
  session_.resolved_callees_[&call] =
      resolved_callee{.decl = method.decl,
                      .owner_module = method.owner->module_name,
                      .impl_target_type = std::string(target_type_name),
                      .receiver = &receiver,
                      .trait_name = method.trait_name};
}

/// Types a call to a `self`-taking method that is a *generic template*
/// (`def at[n: usize](self, v: array[int32, n], i: index[n])`), resolving
/// it to a monomorphized instance the way `check_receiver_call` does for a
/// receiver-style one.
///
/// Without this the call would resolve to the template — which has no
/// compiled form, so the backends would look for a `holder::at` that was
/// never lowered. The parameters are solved from the argument types alone:
/// `self` is the receiver and carries no compile-time arguments of the
/// method's own, so it is skipped here exactly as it is in the ordinary
/// non-generic path.
///
/// Returns `nullopt` when the method isn't a template, or when the
/// enclosing declaration is itself one — inside a template the arguments
/// are still symbols and there is nothing concrete to instantiate against.
auto checker::check_generic_instance_method_call(
    const ast::call_expr &call, const method_entry &method,
    std::string_view target_type_name, const ast::expr &receiver,
    type_id receiver_type, const explicit_generic_args &explicit_args)
    -> std::optional<type_id> {
  if (!is_generic_template(*method.decl)) {
    return std::nullopt;
  }
  const auto params = signature_params(*method.decl, method.owner,
                                       /*skip_self=*/true);
  // Both readings run, because they answer different things: the type
  // bindings below read a type parameter straight off an argument's type,
  // while `match_value_params` (via `solved`) is what reaches *inside* a type
  // to pin a value parameter — an `array[int32, n]` parameter given an
  // `array[int32, 3]` argument solves `n := 3` there and nowhere else.
  auto solved = value_bindings{};
  check_call_args_against(
      call, params, method.decl->name,
      source_location{.file_id = file_.file_id_, .span = method.decl->span},
      &solved);
  check_call_preconditions(call, *method.decl, params);

  auto bindings = param_subst{};
  solve_from_argument_types(call, params, bindings,
                            /*ufcs_receiver=*/nullptr,
                            /*bind_open=*/true);
  // A generic method of a generic block (`def map[U]` in `extend[T]
  // box[T]`) inherits `T` from the receiver: it rides into the instance as
  // a fixed binding, exactly as the static path delivers it.
  auto impl_bindings = param_subst{};
  if (method.block_type_params != nullptr) {
    match_params(method.impl_target_pattern, strip_refs(receiver_type),
                 impl_bindings);
    solve_impl_value_params(method, strip_refs(receiver_type), impl_bindings);
  }

  // Solved in a template's terms — the receiver, or an answer for the
  // method's own parameters or the block's, is written in the caller's
  // parameters: each of the caller's instances names its own copy.
  const auto open_slot = [&](const auto &slot) -> bool {
    return mentions_template_param(slot.second);
  };
  const auto in_template =
      mentions_template_param(receiver_type) ||
      std::ranges::any_of(bindings, open_slot) ||
      std::ranges::any_of(impl_bindings, open_slot) ||
      std::ranges::any_of(solved, [](const auto &slot) -> bool {
        return !slot.second.is_constant();
      });
  if (in_template) {
    defer_impl_method_call(call, receiver_type);
    record_instance_method_callee(call, method, target_type_name, receiver);
    auto all = bindings;
    all.insert(impl_bindings.begin(), impl_bindings.end());
    return substitute_solved(signature_return_type(*method.decl, method.owner,
                                                   method.block_type_params),
                             all);
  }
  const auto impl_scope = bindings_by_name(impl_bindings);
  if (!instantiate_hk_method(
          call, method, target_type_name, bindings,
          resolved_callee{.owner_module = method.owner->module_name,
                          .impl_target_type = "",
                          .receiver = &receiver,
                          .trait_name = method.trait_name},
          explicit_args, solved, receiver_type, &impl_scope)) {
    return std::nullopt;
  }
  return substitute_solved(signature_return_type(*method.decl, method.owner),
                           bindings);
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

/// Types a call to a method whose *impl block* — not the method itself — is
/// what carries generic content (`impl_needs_instance`), resolving it to an
/// instance compiled for this one receiver type.
///
/// The impl's parameters are solved by rigid-unifying its target pattern
/// (`holder[T]`) against the concrete receiver (`holder[int32]`), giving
/// `T := int32`; the signature is then restated under that solution, so
/// `def get(self) -> T` correctly reports `int32` instead of accepting
/// anything at all (design §2's G1 — the silent-acceptance bug).
///
/// The instance is keyed on the *receiver's* type arguments rather than the
/// solved impl parameters, because those are what distinguish two impls for
/// two instantiations of the same target: a non-generic
/// `impl get_it[int32] for holder[int32]` solves nothing and would otherwise
/// collide with its `holder[str]` sibling under one shared name.
///
/// Returns `nullopt` when no instance is needed, or when the enclosing
/// declaration is itself a template — inside one the receiver is still
/// written in type parameters and there is nothing concrete to compile for.
/// A template's call to a method of a generic `impl`/`extend` on a
/// receiver written in the template's parameters (`self.reserve(n)` on a
/// `list[T]`), made for each instance on the receiver's concrete type.
auto checker::defer_impl_method_call(const ast::call_expr &call,
                                     type_id receiver_type) -> void {
  defer_to_instances(
      call, [this, &call, receiver_type](instance_subst &subst) -> void {
        const auto &clone = *clone_of(subst, &call);
        auto explicit_args = explicit_generic_args{};
        if (const auto *field = method_field_of(clone, explicit_args)) {
          replay_method_call(clone, *field,
                             substitute_type(receiver_type, subst),
                             /*static_call=*/false, explicit_args);
        }
      });
}

/// The `receiver.method` a method call names, and its explicit
/// compile-time arguments if it was written `receiver.method[A](...)`.
auto checker::method_field_of(const ast::call_expr &call,
                              explicit_generic_args &explicit_args)
    -> const ast::field_expr * {
  if (call.callee == nullptr) {
    return nullptr;
  }
  const auto *callee = call.callee.get();
  if (callee->kind != ast::node_kind::field_expr) {
    callee = explicit_generic_callee(*callee, explicit_args);
  }
  if (callee == nullptr || callee->kind != ast::node_kind::field_expr) {
    return nullptr;
  }
  const auto &field = ast::cast<const ast::field_expr>(*callee);
  if (explicit_args.empty()) {
    explicit_args = method_explicit_generic_args(field);
  }
  return &field;
}

/// A template's call to a method of a generic `impl`/`extend`: checked
/// against the method's signature read through the receiver (`xs.push(x)`
/// on a `list[T]` takes a `T`), and instantiated by each instance.
auto checker::template_impl_method_call(const ast::call_expr &call,
                                        const method_entry &method,
                                        const type_entry &receiver_entry,
                                        const ast::expr &receiver,
                                        type_id receiver_type)
    -> std::optional<type_id> {
  auto bindings = param_subst{};
  match_params(method.impl_target_pattern, receiver_type, bindings);
  solve_impl_value_params(method, receiver_type, bindings);
  for (const auto &type_param : *method.block_type_params) {
    if (!type_param.name.empty() && !bindings.contains(param_id(type_param))) {
      defer_impl_method_call(call, receiver_type);
      return std::nullopt;
    }
  }
  // The method's own `where` is checked against the template's parameters
  // the way a free function's bounds are (phase 9, rule 3): `p.ordered()`
  // on a `pair[T]` needs `T: ord` written on this template. A failed bound
  // is reported once, here, and its instances do not repeat it.
  auto block_scope = method.fixed_type_params;
  block_scope.merge(bindings_by_name(bindings));
  auto block_solution = generic_solution{};
  carry_impl_value_slots(method, bindings, block_solution);
  if (check_call_bounds(call, *method.decl, method.owner, method.file_id,
                        block_solution, &block_scope)) {
    defer_impl_method_call(call, receiver_type);
  }
  auto params = signature_params(*method.decl, method.owner,
                                 /*skip_self=*/true, method.block_type_params);
  for (auto &param : params) {
    param.type = substitute_solved(param.type, bindings);
  }
  check_call_args_against(
      call, params, method.decl->name,
      source_location{.file_id = file_.file_id_, .span = method.decl->span});
  check_call_preconditions(call, *method.decl, params);
  record_instance_method_callee(call, method, receiver_entry.name, receiver);
  return substitute_solved(signature_return_type(*method.decl, method.owner,
                                                 method.block_type_params),
                           bindings);
}

auto checker::check_impl_generic_method_call(const ast::call_expr &call,
                                             const method_entry &method,
                                             const type_entry &receiver_entry,
                                             const ast::expr &receiver,
                                             type_id receiver_type)
    -> std::optional<type_id> {
  receiver_type = settle(receiver_type);
  if (!impl_needs_instance(method, receiver_entry)) {
    return std::nullopt;
  }
  // A receiver still written in type parameters — `self.cur.is_none()`
  // where `cur: option[J]` inside `extend[J] holder[J]`. There is nothing
  // concrete to instantiate yet; each of the enclosing template's instances
  // names its own. Without this the loop below would report "cannot tell
  // which `T` this call to `is_none` means" -- blaming the user's call for
  // the compiler looking at it a phase too early, and pointing them at a
  // fix (move `T` onto the method) for a declaration in the prelude that
  // they cannot edit and that is not wrong.
  if (mentions_template_param(receiver_type)) {
    return template_impl_method_call(call, method, receiver_entry, receiver,
                                     receiver_type);
  }

  auto bindings = param_subst{};
  match_params(method.impl_target_pattern, receiver_type, bindings);
  solve_impl_value_params(method, receiver_type, bindings);
  for (const auto &type_param : *method.block_type_params) {
    if (type_param.name.empty() || bindings.contains(param_id(type_param))) {
      continue;
    }
    // The receiver didn't pin one of the impl's parameters — an impl whose
    // parameter appears nowhere in its own target type. Nothing about the
    // call site can determine it, so there is no instance to compile.
    error_with_help(
        call.span,
        std::format("cannot tell which `{}` this call to `{}` means",
                    type_param.name, method.decl->name),
        std::format("`{}` is not determined by the receiver's type",
                    type_param.name),
        std::format(
            "`{}` is declared on the `impl` block, and the compiler solves "
            "it by matching the impl's target type against the receiver. "
            "Since `{}` doesn't appear in that target type, no receiver can "
            "pin it down. Move `{}` onto `{}` itself, so it can be solved "
            "from the arguments or given explicitly.",
            type_param.name, type_param.name, type_param.name,
            method.decl->name));
    return k_error_type;
  }

  // The impl's parameters go in as *fixed* bindings rather than as the
  // solution's `type_slots`, because `push_type_params` — which is what
  // consults `type_slots` — only ever walks the declaration's own
  // `type_params`, and `T` is not one of them. `fixed_type_params` is
  // pushed as a scope outright, which is how a monomorphized trait default
  // already carries its `F := option`, and is the mechanism that makes the
  // *body* of `def get(self) -> T` resolve `T` while it is being checked.
  auto scoped_params = method.fixed_type_params;
  scoped_params.merge(bindings_by_name(bindings));

  auto solution = generic_solution{};
  carry_impl_value_slots(method, bindings, solution);
  solution.suffix = std::format("${}", mangle_type_for_instance(receiver_type));

  auto params = signature_params(*method.decl, method.owner,
                                 /*skip_self=*/true, method.block_type_params);
  for (auto &param : params) {
    param.type = substitute_solved(param.type, bindings);
  }
  check_call_args_against(
      call, params, method.decl->name,
      source_location{.file_id = file_.file_id_, .span = method.decl->span});
  check_call_preconditions(call, *method.decl, params);

  // A method with an unannotated parameter is elaborated once its body has
  // said what the parameter is, as any such call is: its instance needs
  // both the receiver's bindings and, if the body left the parameter
  // open, this call's arguments (`resolve_open_param_calls`).
  if (has_unannotated_params(*method.decl)) {
    auto call_params = std::vector<type_id>{k_unknown_type};
    for (const auto &param : params) {
      call_params.push_back(param.type);
    }
    queue_open_param_call(pending_open_param_call{
        .call = &call,
        .decl = method.decl,
        .owner = method.owner,
        .decl_file = method.file_id,
        .call_params = std::move(call_params),
        .file = file_.file_id_,
        .module = file_.module_,
        .receiver = &receiver,
        .method = method,
        .target_type_name = std::string(receiver_entry.name),
        .self_type = strip_refs(receiver_type),
        .block_generic = true,
        .probe = probing_decl_});
    mint_open_result(*method.decl, call.span);
    return open_call_result(
        call,
        substitute_solved(signature_return_type(*method.decl, method.owner,
                                                method.block_type_params),
                          bindings));
  }

  // Phase 8: the arguments above are what pin a receiver that was still
  // open — `out.push(i)` on a `list[?a]` solves `?a` from `i`. Checking the
  // arguments is what ran the solver, so everything named from the receiver
  // has to be recomputed now: an instance named `list___` is a function
  // nothing ever compiles, which is precisely the split phase 6 exists to
  // prevent, arriving from the other direction.
  if (const auto settled = settle(receiver_type); settled != receiver_type) {
    receiver_type = settled;
    // Solving may have pinned the receiver to something still abstract —
    // `list[?a]` becoming `list[T]` inside a generic body. That is the
    // template case the guard above exists for, arriving late; there is
    // still nothing concrete to compile for.
    if (mentions_type_param(receiver_type)) {
      defer_impl_method_call(call, receiver_type);
      return std::nullopt;
    }
    bindings.clear();
    match_params(method.impl_target_pattern, receiver_type, bindings);
    solve_impl_value_params(method, receiver_type, bindings);
    scoped_params = method.fixed_type_params;
    scoped_params.merge(bindings_by_name(bindings));
    solution = generic_solution{};
    carry_impl_value_slots(method, bindings, solution);
    solution.suffix =
        std::format("${}", mangle_type_for_instance(receiver_type));
  }

  // Still open after the arguments have had their say: `xs.len()` on a
  // `list[?a]` that nothing has pinned *yet*. There is no instance to name
  // — `len$list___` is named for a variable, and is a function nothing
  // ever compiles — but there is no error either, because a `push` later
  // in the body may still answer. The decision waits (phase 4's queue),
  // and `flush_deferred` runs it once `?a` is solved.
  if (mentions_type_var(receiver_type)) {
    defer_method_call(
        method.decl->name, receiver_type, std::vector<type_id>{},
        [this, &call, &method, &receiver](type_id settled) -> void {
          name_impl_method_instance(call, method.decl->name, receiver, settled);
        });
    return substitute_solved(signature_return_type(*method.decl, method.owner,
                                                   method.block_type_params),
                             bindings);
  }

  return finish_impl_generic_method_call(call, method, receiver_entry.name,
                                         receiver, receiver_type, bindings,
                                         scoped_params, solution);
}

/// Names the instance and records the callee — the *elaboration* half of
/// `check_impl_generic_method_call`, split out because it is exactly the
/// half that has to wait when the receiver is still open, and is therefore
/// what the `method_call` obligation's resolver runs.
auto checker::finish_impl_generic_method_call(
    const ast::call_expr &call, const method_entry &method,
    std::string_view receiver_name, const ast::expr &receiver,
    type_id receiver_type, const param_subst &bindings,
    const type_scope &scoped_params, const generic_solution &solution)
    -> std::optional<type_id> {
  const auto result =
      substitute_solved(signature_return_type(*method.decl, method.owner,
                                              method.block_type_params),
                        bindings);
  // The method's own `where` (`def ordered(self) where K: ord`) is checked
  // here, at the line that chose `K`, rather than surfacing later from
  // inside the instance body. A call whose bounds fail names no instance.
  if (!check_call_bounds(call, *method.decl, method.owner, method.file_id,
                         solution, &scoped_params)) {
    return result;
  }
  const auto name =
      method_instance_name(receiver_name, method.decl->name, solution.suffix);
  const auto *instance = find_or_check_generic_instance(
      call, *method.decl, method.owner, method.file_id, solution, name,
      &scoped_params, receiver_type, method.block_type_params);
  if (instance == nullptr) {
    return std::nullopt;
  }
  session_.resolved_callees_[&call] =
      resolved_callee{.decl = instance,
                      .owner_module = method.owner->module_name,
                      .impl_target_type = "",
                      .receiver = &receiver,
                      .trait_name = method.trait_name};
  return result;
}

/// The static-dispatch sibling of `check_impl_generic_method_call`: a
/// `static def` reached through a *generic* impl block
/// (`impl[T] maker[T] for example_type[T]`), where the parameter to be solved
/// belongs to the impl and not to the method.
///
/// Such a method has no `type_params` of its own, so it used to take
/// `resolve_type_param_static`'s non-generic path, which names the callee
/// `example_type[int32]::make` and stops. Nothing had instantiated the impl
/// for `int32`, so no function ever bore that name: the call type-checked and
/// then failed in lowering with "could not be resolved to a function in
/// this compiled module". Type-checking a call and compiling nothing for
/// it is exactly the split the receiver-taking sibling exists to prevent.
///
/// The instance discipline is that sibling's, unchanged. The only
/// difference is where the type solving the impl comes from: a qualified
/// path (`example_type[int32].make(...)`) or a solved type parameter
/// (`C.make(...)`), rather than a receiver expression.
///
/// `bindings` receives the impl's solution so the caller can restate the
/// return type under it — `-> example_type[T]` has to report
/// `example_type[int32]`. Returns `nullptr` when no instance could be built,
/// leaving the caller on its existing path.
auto checker::check_impl_generic_static_call(const ast::call_expr &call,
                                             const method_entry &method,
                                             type_id target,
                                             param_subst &bindings)
    -> const ast::func_decl * {
  match_params(method.impl_target_pattern, target, bindings);
  solve_impl_value_params(method, target, bindings);
  for (const auto &type_param : *method.block_type_params) {
    if (type_param.name.empty() || bindings.contains(param_id(type_param))) {
      continue;
    }
    // An impl parameter its own target type never mentions. No call site
    // can pin it, so there is nothing concrete to compile.
    return nullptr;
  }

  auto scoped_params = method.fixed_type_params;
  scoped_params.merge(bindings_by_name(bindings));

  auto solution = generic_solution{};
  carry_impl_value_slots(method, bindings, solution);
  solution.suffix = std::format("${}", mangle_type_for_instance(target));
  // Owned before `find_or_check_generic_instance` runs: checking the
  // instance body interns types, and this name is read after that.
  const auto target_name = std::string(session_.types_.entry(target).name);
  const auto name =
      method_instance_name(target_name, method.decl->name, solution.suffix);
  if (!check_call_bounds(call, *method.decl, method.owner, method.file_id,
                         solution, &scoped_params)) {
    return nullptr;
  }
  return find_or_check_generic_instance(
      call, *method.decl, method.owner, method.file_id, solution, name,
      &scoped_params, target, method.block_type_params);
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

/// Checks a method-syntax call resolved to a receiver-style method (see
/// `receiver_fills_first_param`): the receiver fills the first declared
/// parameter, the written arguments fill the rest. The callee's type
/// parameters are solved by rigid pattern unification — the receiver
/// first (`option[int32]` against `M[A]` gives `M = option, A = int32`),
/// then each checked argument — and the declared return type is restated
/// under that solution, so `ma.bind(f)` has a concrete type instead of an
/// opaque generic one.
auto checker::check_receiver_call(const ast::call_expr &call,
                                  const method_entry &method,
                                  std::string_view target_type_name,
                                  const ast::expr &receiver,
                                  type_id receiver_type,
                                  const explicit_generic_args &explicit_args)
    -> type_id {
  const auto params = signature_params(*method.decl, method.owner, false);
  if (params.empty()) {
    infer_call_args_loosely(call);
    return signature_return_type(*method.decl, method.owner);
  }

  auto bindings = param_subst{};
  match_params(params.front().type, receiver_type, bindings);
  type_mismatch(receiver.span, substitute_solved(params.front().type, bindings),
                receiver_type, "for this method's receiver");

  auto rest = std::vector<fn_param_info>(params.begin() + 1, params.end());
  for (auto &param : rest) {
    param.type = substitute_solved(param.type, bindings);
  }
  check_call_args_against(
      call, rest, method.decl->name,
      source_location{.file_id = file_.file_id_, .span = method.decl->span});

  // Later arguments pin parameters the receiver didn't (`bind`'s `B`
  // arrives with the function argument): read each argument's recorded
  // type back and unify it too, then state the return type under the
  // full solution.
  solve_from_argument_types(call, rest, bindings, /*ufcs_receiver=*/nullptr,
                            /*bind_open=*/true);

  // A generic method has no compiled form of its own — resolve the call
  // against a monomorphized instance (`option::bind$int32$str`) so both
  // backends see only concrete types. A non-generic method resolves
  // directly, exactly like a `self`-taking method does.
  if (!method.decl->type_params.empty()) {
    if (!instantiate_hk_method(
            call, method, target_type_name, bindings,
            resolved_callee{.owner_module = method.owner->module_name,
                            .impl_target_type = "",
                            .receiver = &receiver},
            explicit_args) &&
        function_.current_template_ != nullptr) {
      // `o.map(f)` on an `option[T]`: the instance is each instance's.
      defer_to_instances(
          call,
          [this, &call, &method, target = std::string(target_type_name),
           &receiver, receiver_type](instance_subst &subst) -> void {
            const auto &clone = *clone_of(subst, &call);
            const auto concrete = substitute_type(receiver_type, subst);
            const auto params =
                signature_params(*method.decl, method.owner, false);
            auto solved = param_subst{};
            match_params(params.front().type, concrete, solved);
            auto rest =
                std::vector<fn_param_info>(params.begin() + 1, params.end());
            for (auto &param : rest) {
              param.type = substitute_solved(param.type, solved);
            }
            solve_from_argument_types(clone, rest, solved);
            auto clone_args = explicit_generic_args{};
            if (const auto *field = method_field_of(clone, clone_args)) {
              (void)field;
            }
            (void)instantiate_hk_method(
                clone, method, target, solved,
                resolved_callee{.owner_module = method.owner->module_name,
                                .impl_target_type = "",
                                .receiver = clone_of(subst, &receiver)},
                clone_args);
          });
    }
  } else {
    session_.resolved_callees_[&call] =
        resolved_callee{.decl = method.decl,
                        .owner_module = method.owner->module_name,
                        .impl_target_type = std::string(target_type_name),
                        .receiver = &receiver};
  }
  return substitute_solved(signature_return_type(*method.decl, method.owner),
                           bindings);
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
    const ast::field_expr &field, const ast::func_decl &decl,
    const explicit_generic_args &explicit_args) -> bool {
  if (explicit_args.empty() || !decl.type_params.empty()) {
    return true;
  }
  error_with_help(
      field.span,
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
