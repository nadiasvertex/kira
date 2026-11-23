#include "checker.h"
#include "src/parser/ast_traversal.h"

namespace cinder::semantic::detail {

/// Whether `decl` may be reached by method syntax at all (§3.3): it takes
/// at least one parameter — there must be something for the receiver to
/// fill — and that parameter is not `self`, which would make it a method
/// already reached by the arms above.
///
/// The design also requires `pub`. That is enforced only for functions
/// from *other* modules, which is where the concern it answers lives
/// (§R6, candidate-pool pollution from imports). Inside the declaring
/// module a private `def helper(x: int32)` is already callable as
/// `helper(x)`, and refusing `x.helper()` there would be an asymmetry with
/// no safety story behind it.
auto checker::is_ufcs_eligible(const ast::func_decl &decl, bool same_module)
    -> bool {
  if (decl.params.empty() || param_name_of(decl.params.front()) == "self") {
    return false;
  }
  return same_module || decl.visibility == ast::visibility::pub;
}

/// Every UFCS-eligible free function named `name` visible here, in the
/// same order `infer_call` resolves a bare `name(...)`: this module, then
/// explicit imports, then wildcard imports, then the prelude. Duplicates
/// are collapsed by declaration identity, so a function reachable through
/// two different imports is one candidate, not an ambiguity.
auto checker::collect_ufcs_candidates(const std::string &name)
    -> std::vector<ufcs_candidate> {
  auto found = std::vector<ufcs_candidate>{};
  const auto add = [&](const func_decl_ref &fn, const module_members *owner,
                       ufcs_origin origin) -> void {
    if (fn.decl == nullptr || owner == nullptr ||
        !is_ufcs_eligible(*fn.decl, origin == ufcs_origin::this_module)) {
      return;
    }
    if (std::ranges::any_of(found, [&](const ufcs_candidate &seen) -> bool {
          return seen.decl == fn.decl;
        })) {
      return;
    }
    found.push_back(ufcs_candidate{.decl = fn.decl,
                                   .owner = owner,
                                   .file_id = fn.file_id,
                                   .origin = origin});
  };

  if (file_.module_ != nullptr) {
    if (const auto it = file_.module_->functions.find(name);
        it != file_.module_->functions.end()) {
      add(it->second, file_.module_, ufcs_origin::this_module);
    }
  }
  if (const auto *import = find_import(name)) {
    if (const auto *source = import_source_module(*import)) {
      if (const auto it = source->functions.find(imported_member_name(*import));
          it != source->functions.end()) {
        add(it->second, source, ufcs_origin::explicit_import);
      }
    }
  }
  for (const auto *source : wildcard_import_sources()) {
    if (const auto it = source->functions.find(name);
        it != source->functions.end()) {
      add(it->second, source, ufcs_origin::wildcard_import);
    }
  }
  if (const auto prelude = find_prelude_function(name)) {
    add(prelude->first, prelude->second, ufcs_origin::prelude);
  }
  return found;
}

/// Whether the receiver expression names something that may be mutated —
/// a `var`/`mut` binding, a by-value parameter, or a value already held
/// behind a `&mut`. Used only to decide between `fits` and `needs_mut`,
/// never to *permit* a mutation: the ordinary mutability checks still run
/// on the call.
auto checker::receiver_is_mutable_lvalue(const ast::expr &receiver,
                                         type_id receiver_type) -> bool {
  const auto &entry = session_.types_.entry(receiver_type);
  if (entry.kind == type_kind::ref_kind) {
    return entry.is_mut;
  }
  const auto *root = assignment_root_ident(receiver);
  if (root == nullptr) {
    return false;
  }
  const auto *binding = lookup_value(root->name);
  if (binding == nullptr) {
    return false;
  }
  // A reference binding decides by what it lets through.
  const auto &binding_entry = session_.types_.entry(binding->type);
  if (binding_entry.kind == type_kind::ref_kind) {
    return binding_entry.is_mut;
  }
  // A parameter taken by value is the function's own (`self` never is: it
  // is a reference, or a `copy` scalar a non-`mut` method may not change).
  if (binding->origin == binding_origin::parameter) {
    return root->name != "self";
  }
  return binding->origin == binding_origin::var_binding ||
         binding->origin == binding_origin::mut_binding;
}

/// The §3.2 adaptation table, first match wins. `param` is the callee's
/// declared first parameter type; `receiver` is the receiver's own type,
/// references and all.
auto checker::classify_receiver_fit(type_id param, type_id receiver,
                                    bool receiver_is_mutable) -> receiver_fit {
  const auto &param_entry = session_.types_.entry(param);
  const auto bare_param = strip_refs(param);
  const auto bare_receiver = strip_refs(receiver);

  // An unconstrained generic first parameter (`def sum[I, T](it: I)`)
  // accepts anything — unification binds it to whatever arrived.
  if (session_.types_.entry(bare_param).kind == type_kind::type_param_kind) {
    return receiver_fit::fits;
  }
  // The candidate's own parameters (`&list[T]`'s `T`) are not solved yet.
  if (!session_.types_.compatible(open_foreign_params(bare_param),
                                  bare_receiver)) {
    return receiver_fit::does_not_fit;
  }
  if (param_entry.kind == type_kind::ref_kind && param_entry.is_mut) {
    // `&mut T` against a non-`mut` binding is a hard error, never a
    // silently skipped candidate — otherwise `v.iter_mut()` on a `let`
    // collapses into a baffling "no method `iter_mut`".
    return receiver_is_mutable ? receiver_fit::fits : receiver_fit::needs_mut;
  }
  return receiver_fit::fits;
}

/// Renders a candidate the way the ambiguity and no-fit diagnostics name
/// it: fully qualified, with the signature the user has to satisfy.
auto checker::describe_ufcs_candidate(const ufcs_candidate &candidate)
    -> std::string {
  auto rendered = std::string{};
  for (const auto &param : signature_params(*candidate.decl, candidate.owner,
                                            /*skip_self=*/false)) {
    if (!rendered.empty()) {
      rendered += ", ";
    }
    rendered +=
        std::format("{}: {}", param.name, session_.types_.display(param.type));
  }
  return std::format("{}.{}({})", candidate.owner->module_name,
                     candidate.decl->name, rendered);
}

/// The candidate's qualified name alone, for the "call it this way
/// instead" half of the ambiguity help.
auto checker::ufcs_candidate_path(const ufcs_candidate &candidate)
    -> std::string {
  return std::format("{}.{}", candidate.owner->module_name,
                     candidate.decl->name);
}

/// Checks `recv.f(a...)` resolved to the free function `candidate`, as the
/// call `f(recv, a...)` it desugars to: the receiver fills the first
/// declared parameter, the written arguments fill the rest, and the
/// callee's type parameters are solved from the receiver first and the
/// arguments after — the same discipline a receiver-style method gets, on a
/// declaration that simply lives outside any `impl`.
auto checker::check_ufcs_call(const ast::call_expr &call,
                              const ast::field_expr &field,
                              const ufcs_candidate &candidate,
                              type_id receiver_type) -> type_id {
  return check_call(
      call, call_callee{.form = callee_form::free_function,
                        .decl = candidate.decl,
                        .owner = candidate.owner,
                        .decl_file = candidate.file_id,
                        .receiver = receiver_use::first_param,
                        .receiver_expr = field.object.get(),
                        .self_type = receiver_type,
                        .explicit_args = method_explicit_generic_args(field),
                        .callee_node = &field,
                        .display_name = candidate.decl->name});
}

/// `4.id64()` for `def id64(x: int64)`: an integer literal receiver is
/// solved by the one free function the call can reach, as `id64(4)` is by
/// the parameter it is passed to. Waiting would default the literal to
/// `int32` first and then report that `id64` cannot be called on it.
///
/// Only an integer literal, because its candidates are the numeric types
/// alone, so "no method by this name" can be checked against the type the
/// call would pin it to — UFCS never shadows a method. Only when exactly
/// one candidate fits at the most-visible level and its first parameter
/// is a concrete number; anything else waits as before, and an ambiguity
/// or a mismatch is still reported by `try_ufcs_call`.
auto checker::pin_literal_receiver_by_ufcs(const ast::field_expr &field,
                                           type_id receiver) -> bool {
  if (session_.types_.entry(receiver).kind != type_kind::type_var_kind ||
      !integer_literal_leaves_.contains(leaf_ctxt_.find(receiver))) {
    return false;
  }
  auto viable = std::vector<std::pair<ufcs_candidate, type_id>>{};
  for (const auto &candidate : collect_ufcs_candidates(field.field_name)) {
    const auto params = signature_params(*candidate.decl, candidate.owner,
                                         /*skip_self=*/false);
    if (params.empty()) {
      continue;
    }
    const auto first = strip_refs(params.front().type);
    if (session_.types_.is_numeric(first) ||
        session_.types_.entry(first).kind == type_kind::type_param_kind) {
      viable.emplace_back(candidate, params.front().type);
    }
  }
  if (viable.empty()) {
    return false;
  }
  const auto best = std::ranges::min(
      viable, {}, [](const auto &c) -> ufcs_origin { return c.first.origin; });
  std::erase_if(viable, [&](const auto &c) -> bool {
    return c.first.origin != best.first.origin;
  });
  if (viable.size() != 1) {
    return false;
  }
  const auto param = viable.front().second;
  const auto number = strip_refs(param);
  if (!session_.types_.is_numeric(number) ||
      resolve_method(number, field.field_name).found()) {
    return false;
  }
  solve_leaves(number, receiver);
  return true;
}

/// The fourth and last arm of the method-call ladder. Returns `nullopt`
/// when no free function of this name is visible at all, leaving the
/// caller to report its own not-found error; otherwise this call is UFCS's
/// to answer, successfully or with a diagnostic.
auto checker::try_ufcs_call(const ast::call_expr &call,
                            const ast::field_expr &field, type_id receiver_type)
    -> std::optional<type_id> {
  if (field.object == nullptr) {
    return std::nullopt;
  }
  // A receiver that is still a type parameter has no method set yet, so it
  // cannot be matched against a free function's first parameter: a type
  // param unifies with anything, which means *every* visible free function
  // of the right name "fits". That is how `self.peek()` inside a trait
  // default (`self` is `Self` there, a type param) was captured by
  // `std.algo.peek(it: &mut peekable_iter[I, T])` — a free function
  // shadowing a method of the very trait being defined, which the UFCS
  // rule ("reached only after every method lookup above has failed, so a
  // free function can never shadow a method") exists to forbid.
  //
  // Nothing is lost by declining: a method on a `T` is what its bounds
  // promise (`find_bound_callee`), and each instance names the
  // concrete method its template's call reaches.
  if (session_.types_.entry(receiver_type).kind == type_kind::type_param_kind) {
    return std::nullopt;
  }
  const auto candidates = collect_ufcs_candidates(field.field_name);
  if (candidates.empty()) {
    return std::nullopt;
  }

  const auto mutable_receiver =
      receiver_is_mutable_lvalue(*field.object, receiver_type);
  auto viable = std::vector<ufcs_candidate>{};
  auto wants_mut = std::vector<ufcs_candidate>{};
  for (const auto &candidate : candidates) {
    const auto params = signature_params(*candidate.decl, candidate.owner,
                                         /*skip_self=*/false);
    if (params.empty()) {
      continue;
    }
    switch (classify_receiver_fit(params.front().type, receiver_type,
                                  mutable_receiver)) {
    case receiver_fit::fits:
      viable.push_back(candidate);
      break;
    case receiver_fit::needs_mut:
      wants_mut.push_back(candidate);
      break;
    case receiver_fit::does_not_fit:
      break;
    }
  }

  // Several candidates fit, but they are not all equally visible: a `map`
  // declared in this module beats `std.algo.map` from the prelude, exactly
  // as it does for a bare `map(...)` call — `collect_ufcs_candidates`
  // gathers them in that precedence order for this reason. Without the
  // narrowing, defining a function whose name the standard library also
  // uses made `x.map(...)` ambiguous while `map(x, ...)` kept working,
  // which is not a distinction the reader can be expected to predict.
  // Two candidates at the *same* level genuinely are ambiguous and still
  // report below.
  if (viable.size() > 1) {
    const auto best = std::ranges::min(
        viable, {},
        [](const ufcs_candidate &c) -> ufcs_origin { return c.origin; });
    std::erase_if(viable, [&](const ufcs_candidate &c) -> bool {
      return c.origin != best.origin;
    });
  }

  if (viable.size() == 1) {
    return check_ufcs_call(call, field, viable.front(), receiver_type);
  }

  if (viable.size() > 1) {
    auto diag = diagnostic(
        diagnostic_level::error,
        std::format("`{}` is ambiguous here — {} visible functions accept "
                    "`{}` as their first argument",
                    field.field_name, viable.size(),
                    session_.types_.display(receiver_type)),
        file_.file_id_);
    diag.with_label(field.span, "ambiguous call");
    for (const auto &candidate : viable) {
      diag.with_note(describe_ufcs_candidate(candidate));
    }
    diag.with_help(std::format(
        "There is no most-specific winner to pick between these; say which "
        "one you mean by calling it qualified, with the receiver as its "
        "first argument: `{}({}{})`.",
        ufcs_candidate_path(viable.front()),
        describe(*field.object, predicate_subst{}),
        call.args.empty() ? "" : ", ..."));
    emit_diag(diag);
    mark_error();
    infer_call_args_loosely(call);
    return k_error_type;
  }

  if (!wants_mut.empty()) {
    const auto &candidate = wants_mut.front();
    error_with_help(
        field.span,
        std::format("`{}` needs to mutate its receiver, and `{}` is not a "
                    "mutable binding",
                    field.field_name,
                    describe(*field.object, predicate_subst{})),
        "receiver is immutable",
        std::format("`{}` takes its first argument by `&mut`. Bind the "
                    "receiver with `var` (or `let mut`) so it can be "
                    "mutated in place.",
                    describe_ufcs_candidate(candidate)));
    infer_call_args_loosely(call);
    return k_error_type;
  }

  // The name exists but nothing accepts this receiver — the highest-value
  // message of the three, because this is what a forgotten `.iter()`
  // looks like.
  auto diag =
      diagnostic(diagnostic_level::error,
                 std::format("`{}` cannot be called on `{}`", field.field_name,
                             session_.types_.display(receiver_type)),
                 file_.file_id_);
  diag.with_label(field.span,
                  std::format("no `{}` accepts `{}` as its first argument",
                              field.field_name,
                              session_.types_.display(receiver_type)));
  // Naming each candidate's *first parameter type* is the whole value of
  // this message: the mismatch is always between that and the receiver,
  // and spelling both out is what turns "no method" into "you forgot an
  // `.iter()`" without having to guess which fix applies.
  auto wanted = std::string{};
  for (const auto &candidate : candidates) {
    const auto params = signature_params(*candidate.decl, candidate.owner,
                                         /*skip_self=*/false);
    diag.with_note(std::format(
        "`{}` is visible here, and wants `{}` first",
        describe_ufcs_candidate(candidate),
        params.empty() ? "nothing"
                       : session_.types_.display(params.front().type)));
    if (wanted.empty() && !params.empty()) {
      wanted = session_.types_.display(params.front().type);
    }
  }
  diag.with_help(std::format(
      "A free function is callable as a method only when its first "
      "parameter accepts the receiver. Here that parameter is `{}` and the "
      "receiver is `{}` — either produce a `{}` to call this on (a "
      "`list[T]` becomes an iterator with `.iter()`), or call `{}` "
      "qualified with an argument that fits.",
      wanted, session_.types_.display(receiver_type), wanted,
      field.field_name));
  emit_diag(diag);
  mark_error();
  infer_call_args_loosely(call);
  return k_error_type;
}

auto checker::infer_method_call(const ast::call_expr &call,
                                const ast::field_expr &field) -> type_id {
  if (field.object == nullptr) {
    infer_call_args_loosely(call);
    return k_unknown_type;
  }

  // `expr.lit(...)`/`expr.ident(...)` — the AST-builder intrinsics that
  // construct a new `expr` quote value programmatically. `expr` here is
  // a contextual pseudo-namespace (see `is_prelude_value_name`), not a
  // real bound value, so it must be intercepted before ordinary object
  // typing/method lookup ever tries to resolve it. Actual construction
  // happens at evaluation time (`evaluator::try_eval_expr_builder_call`);
  // this just gives the call site a concrete type to check against.
  if (field.object->kind == ast::node_kind::ident_expr &&
      ast::cast<const ast::ident_expr>(*field.object).name == "expr") {
    infer_call_args_loosely(call);
    static constexpr std::array<std::string_view, 10> k_expr_builder_names = {
        "lit",    "ident", "field",        "interp_concat", "debug",
        "binary", "call",  "ctor_pattern", "match_on",      "arm"};
    if (std::ranges::find(k_expr_builder_names, field.field_name) ==
        k_expr_builder_names.end()) {
      error_with_help(field.span,
                      std::format("`expr.{}` is not a recognized AST-builder "
                                  "intrinsic",
                                  field.field_name),
                      "unknown AST-builder call",
                      "Only `expr.lit(value)`, `expr.ident(name)`, "
                      "`expr.field(object, name)`, `expr.interp_concat(a, b)`, "
                      "`expr.debug(value)`, `expr.binary(op, lhs, rhs)`, "
                      "`expr.call(callee, args...)`, "
                      "`expr.ctor_pattern(name, arity, prefix)`, "
                      "`expr.match_on(subject)`, "
                      "and `expr.arm(match, pattern, body)` construct a new "
                      "`expr`/`pattern` quote value programmatically.");
      return k_error_type;
    }
    return session_.types_.builtin("expr");
  }

  // `T.fields()`/`T.field_count()`/`T.name()` — compile-time reflection
  // over a struct type's own declaration (design plan section 4). Only
  // intercepted when both the method name matches one of the three
  // recognized reflection calls *and* the object actually names a real
  // type or an in-scope generic type parameter (the latter for a
  // `static def derive_show[T]()`-shaped body calling `T.fields()` — M7)
  // reachable here — a bare `ident_expr` whose name happens to collide
  // with an unrelated value or qualified-call target must still fall
  // through to the ordinary handling below untouched.
  if (field.object->kind == ast::node_kind::ident_expr &&
      (field.field_name == "fields" || field.field_name == "field_count" ||
       field.field_name == "name" || field.field_name == "variants" ||
       field.field_name == "variant_count" || field.field_name == "kind")) {
    const auto &type_name =
        ast::cast<const ast::ident_expr>(*field.object).name;
    if (const auto bound = lookup_type_param(type_name); bound.has_value()) {
      infer_call_args_loosely(call);
      if (field.field_name == "name") {
        // Inside the *template* (not yet instantiated), `T` is bound to
        // an abstract placeholder rather than a concrete type — nothing
        // to fold yet, and the template itself is never lowered; each
        // instance folds its own. Only a genuinely concrete
        // instance's binding is foldable; that's what `hir::lower` needs
        // in `session_.type_param_reflections_` to give this call a runtime
        // answer (`static` contexts aside, `T.name()` has no other way
        // to produce one — see `type_param_reflection`'s doc comment).
        const auto &bound_entry = session_.types_.entry(*bound);
        if (bound_entry.kind != type_kind::type_param_kind) {
          session_.type_param_reflections_[&call] =
              type_param_reflection{.type_name = bound_entry.name};
        } else {
          defer_to_instances(
              call,
              [this, &call, bound = *bound](instance_subst &subst) -> void {
                const auto settled =
                    session_.types_.entry(substitute_type(bound, subst));
                if (settled.kind != type_kind::type_param_kind) {
                  session_.type_param_reflections_[clone_of(subst, &call)] =
                      type_param_reflection{.type_name = settled.name};
                }
              });
        }
        return session_.types_.builtin("str");
      }
      return reflect_type_call(call, field.field_name, *bound);
    }
    if (const auto found = find_type_decl_by_name(type_name)) {
      infer_call_args_loosely(call);
      // The type the name means, with any type parameters it declares left
      // as they are: reflecting on `pair` describes the declaration.
      const auto reflected =
          session_.types_.user_type(*found->first, found->second, {});
      if (field.field_name == "name") {
        fold_type_reflection(call, field.field_name, reflected);
        return session_.types_.builtin("str");
      }
      return reflect_type_call(call, field.field_name, reflected);
    }
  }

  // `M.name()`/`M.functions()`/`M.types()`/`.function_count()`/
  // `.type_count()` — compile-time reflection over a module's surface
  // (design §5). Intercepted only when the object names a module in scope
  // and is not a type (type reflection above already claimed the type case),
  // so `comptime::try_eval_module_reflection_call` sees a well-typed call.
  if (field.object->kind == ast::node_kind::ident_expr &&
      (field.field_name == "name" || field.field_name == "functions" ||
       field.field_name == "types" || field.field_name == "function_count" ||
       field.field_name == "type_count")) {
    const auto &mod_name = ast::cast<const ast::ident_expr>(*field.object).name;
    if (names_reflectable_module(mod_name)) {
      infer_call_args_loosely(call);
      return reflect_module_call(call, field.field_name, mod_name);
    }
  }

  // `int32.max`-style access on a type name never reaches method lookup.
  const auto type_constant = infer_type_name_member(field);
  if (type_constant.has_value() && *type_constant != k_unknown_type) {
    infer_call_args_loosely(call);
    return *type_constant;
  }

  // A module- or type-qualified call (`std.io.open(...)`,
  // `console.println(...)`, `io_error.from(...)`, `int64.zero()`) — see
  // `infer_qualified_call`'s doc comment for why this is checked here
  // rather than dispatched on `call.callee`'s own kind.
  if (const auto qualified = infer_qualified_call(call, field)) {
    return *qualified;
  }
  if (type_constant.has_value()) {
    infer_call_args_loosely(call);
    return *type_constant;
  }

  // Deliberately *not* a point of demand. A receiver still open here is
  // waited on rather than defaulted: the dispatch registers a `method_call`
  // obligation over the receiver's leaves and runs once a later statement
  // (`xs.push(big)`) has said what they are.
  // Settled, so a receiver whose leaf an earlier statement solved (the
  // result of a call that waited, read back through its binding) is looked
  // up as what it now is.
  auto object = strip_refs(settle(infer_expr(*field.object, k_unknown_type)));
  if (pin_literal_receiver_by_ufcs(field, object)) {
    object = strip_refs(settle(object));
  }
  // The exception is a call whose method cannot be found on the receiver
  // as it stands: no method by this name and no free function that could
  // be reached by UFCS — the impl is chosen by the receiver's type, as
  // with `b32.get()` on a `boxed[?a]` with one `get` per element type — or
  // a receiver that is a bare leaf (`a.cmp(&b)` for `let a = 5`), which
  // has no shape to match anything against. The call waits for a later
  // statement to say what the receiver is (`defer_open_receiver_call`);
  // where it cannot, the receiver is owed the last resort now, and the
  // lookup below runs against the answer.
  if (leaf_ctxt_.meta_count() != 0 && mentions_type_var(settle(object)) &&
      !resolve_method(object, field.field_name).found() &&
      (session_.types_.entry(settle(object)).kind == type_kind::type_var_kind ||
       field.object->kind != ast::node_kind::ident_expr ||
       collect_ufcs_candidates(field.field_name).empty())) {
    if (const auto waiting = defer_open_receiver_call(call, field)) {
      return *waiting;
    }
    object = strip_refs(demand(object));
  }
  // A member the receiver lacks is looked up on its `deref` target, and
  // the call proceeds against that target; lowering inserts the
  // `deref()` calls in front of the receiver (`deref_adjustments`).
  const auto handle = object;
  const auto via_deref =
      lookup_through_deref(deref_site{.receiver = field.object.get()}, object,
                           field.field_name, field.span);
  if (via_deref.has_value()) {
    object = *via_deref;
  }
  const auto &entry = session_.types_.entry(object);
  if (entry.kind == type_kind::type_param_kind) {
    const auto bound = find_bound_callee(call, field, object,
                                         /*static_call=*/false);
    return bound.has_value() ? check_call(call, *bound) : k_error_type;
  }
  if (entry.kind == type_kind::builtin_generic_kind &&
      builtin_method_result(entry, field.field_name) != k_unknown_type) {
    if (entry.name == "crew" && field.field_name == "spawn" &&
        call.args.size() == 1) {
      // `c.spawn(t)` gives back the task it was handed.
      const auto task = infer_expr(*call.args[0].value, k_unknown_type);
      if (reject_non_task(*call.args[0].value, task, "c.spawn(...)")) {
        return k_error_type;
      }
      record_crew_spawn(*field.object, task, call.args[0].value->span);
      require_task_args_sendable(*call.args[0].value);
      return task;
    }
    if (entry.name == "crew" && field.field_name == "errors" &&
        call.args.empty()) {
      return infer_crew_errors(call, *field.object, field.span);
    }
    if (entry.name == "task" && field.field_name == "get" &&
        call.args.empty()) {
      // `t.get()` is `await t`, for a task a `crew` has already finished.
      return awaited_type(object, field.span);
    }
  }
  auto found = find_method_callee(call, field, object, handle,
                                  via_deref.has_value());
  if (const auto *callee = std::get_if<call_callee>(&found)) {
    const auto result = check_call(call, *callee);
    // A method that hands back the receiver's own concrete type (e.g. a
    // builder-style trait method returning `Self`) stays opaque to the
    // caller, exactly like the value that produced it.
    if (callee->opaque_result &&
        session_.types_.compatible(callee->self_type, result)) {
      return object;
    }
    return result;
  }
  if (const auto *answered = std::get_if<type_id>(&found)) {
    return *answered;
  }
  // A builtin method the compiler answers itself.
  infer_call_args_loosely(call);
  return builtin_method_result(entry, field.field_name);
}

auto checker::find_method_callee(const ast::call_expr &call,
                                 const ast::field_expr &field, type_id object,
                                 type_id handle, bool via_deref)
    -> method_callee {
  const auto &entry = session_.types_.entry(object);
  const auto explicit_args = method_explicit_generic_args(field);
  // A receiver-style method (`ma.bind(f)`) takes its receiver as its first
  // parameter rather than as `self`.
  const auto callee_of = [&](const method_entry &method) -> call_callee {
    auto callee = method_call_callee(method, field, object,
                                     std::string(entry.name), explicit_args);
    if (receiver_fills_first_param(method, entry)) {
      callee.receiver = receiver_use::first_param;
      callee.receiver_expr = field.object.get();
    }
    return callee;
  };

  switch (entry.kind) {
  case type_kind::struct_kind:
  case type_kind::sum_kind:
  case type_kind::opaque_kind: {
    if (const auto *method = find_method(entry, field.field_name, object)) {
      // `x.drop()` is refused outright (spec/specification/02-intermediate/
      // 17-shared-ownership-and-drop.md, "no direct `x.drop()` call") —
      // calling it explicitly and then letting scope-exit call it again
      // would double-drop. Gated on the type actually implementing the
      // `drop` trait, not merely on the method being named "drop" — an
      // unrelated user method that happens to share the name is fine.
      // `method->from_trait` is *not* the right test here: it is set only
      // for a trait-*default* method reached with no override, and every
      // real `impl drop for T: def drop(mut self) -> unit: ...` writes its
      // own body, so `from_trait` is null for the exact case this must
      // catch.
      if (field.field_name == "drop" && type_has_trait(entry, "drop")) {
        error_with_help(
            call.span,
            std::format("`{}.drop()` may not be called directly", entry.name),
            "explicit `drop()` here",
            "a value's `drop` runs on its own when its scope ends; calling "
            "it directly and then letting scope exit call it again would "
            "run it twice. If you need to release it early, end its "
            "scope early instead (e.g. a `scope:` block once that lands, "
            "or restructure so this is its last use).");
        infer_call_args_loosely(call);
        return k_error_type;
      }
      if (via_deref &&
          !method_callable_through_deref(call, field, *method, handle)) {
        infer_call_args_loosely(call);
        return k_error_type;
      }
      return callee_of(*method);
    }
    if (const auto derived = derived_method_result(entry, field.field_name)) {
      infer_call_args_loosely(call);
      return *derived;
    }
    if (const auto field_type = struct_field_type(entry, field.field_name)) {
      const auto &field_entry = session_.types_.entry(*field_type);
      if (field_entry.kind == type_kind::fn_kind) {
        record_expr_type(field, *field_type);
        return check_call(call,
                          call_callee{.form = callee_form::fn_value,
                                      .callee_node = &field,
                                      .display_name = field.field_name,
                                      .fn_type = *field_type});
      }
      infer_call_args_loosely(call);
      return k_unknown_type;
    }
    // Last arm of the ladder: a free function called with method syntax
    // (§3). Reached only after every method lookup above has failed, so a
    // free function can never shadow a method.
    if (const auto ufcs = try_ufcs_call(call, field, object)) {
      return *ufcs;
    }
    // The header names the receiver's *applied* type (`list[int32]`), not
    // the bare constructor `entry.name` holds: "no method `pusk` on type
    // `list`" leaves the reader to guess which `list`. The note and help
    // stay on `entry.name` — an `impl` block is written against the
    // constructor, so that is the right spelling there.
    const auto display = session_.types_.display(object);
    auto diag = diagnostic(
        diagnostic_level::error,
        std::format("no method `{}` on type `{}`", field.field_name, display),
        file_.file_id_);
    diag.with_label(field.span, "method not found");
    // A near-miss gets named outright rather than leaving the reader to
    // spot it in the list: `nums.pusk(4)` should say "did you mean
    // `push`?".
    const auto candidates = available_method_name_list(entry);
    if (const auto suggestion = best_suggestion(field.field_name, candidates)) {
      diag.with_help(std::format("did you mean `{}`?", *suggestion));
    }
    if (const auto searched = deref_search_note(object)) {
      diag.with_note(*searched);
    }
    if (const auto methods = available_method_names(entry); !methods.empty()) {
      diag.with_note(
          std::format("`{}` provides the methods {}", entry.name, methods));
    } else {
      diag.with_help(std::format(
          "Add the method in an `impl {}:` block, or implement a trait "
          "that provides it.",
          entry.name));
    }
    emit_diag(diag);
    mark_error();
    infer_call_args_loosely(call);
    return k_error_type;
  }
  case type_kind::existential_kind: {
    // Only the bound traits' own methods are callable on an existential
    // receiver — the concrete underlying type (`entry.result`) is a
    // checker-internal detail, not something the caller may otherwise
    // touch. Dispatch itself is ordinary static `find_method` against the
    // underlying type once the name is confirmed to belong to the bound.
    const auto in_bound = std::ranges::any_of(
        entry.existential_bound, [&](const bound_trait_ref &required) -> bool {
          const auto trait = find_trait_anywhere(required.trait_name);
          return trait.has_value() &&
                 trait_declares_method(*trait->first, field.field_name);
        });
    if (!in_bound) {
      error_with_help(field.span,
                      std::format("no method `{}` on the existential type `{}`",
                                  field.field_name, entry.name),
                      "method not in bound",
                      std::format("`{}` only exposes {}.", entry.name,
                                  existential_bound_names(entry)));
      infer_call_args_loosely(call);
      return k_error_type;
    }
    const auto &underlying = session_.types_.entry(entry.result);
    if (const auto *method = find_method(underlying, field.field_name)) {
      auto callee = method_call_callee(*method, field, entry.result,
                                       std::string(underlying.name),
                                       explicit_args);
      callee.opaque_result = true;
      return callee;
    }
    error_with_help(
        field.span,
        std::format("`{}` provides `{}` but no concrete implementation "
                    "was found",
                    entry.name, field.field_name),
        "method not implemented",
        "This is likely a trait default that hasn't been given a body — "
        "check the `impl` providing this trait for the underlying type.");
    infer_call_args_loosely(call);
    return k_error_type;
  }
  default: {
    // Builtin inherent methods take priority; an impl on the builtin's
    // constructor (`impl monad for option`) or an `extend` block fills in
    // only when the name isn't one of the hardcoded builtin methods.
    const auto resolved = resolve_method(entry, object, field.field_name);
    if (resolved.origin == method_origin::builtin_impl ||
        resolved.origin == method_origin::builtin_extend) {
      // A generic impl or `extend` on a prelude generic or a structural
      // type (`impl[T] eq for option[T]`, `extend[T] option[T]`) is
      // compiled once per receiver type, by the call pipeline.
      return callee_of(*resolved.method);
    }
    if (resolved.origin == method_origin::none) {
      // Same last arm as the struct/sum/opaque case above, and the one
      // that matters most for `std.algo`: the whole catalog is free
      // functions over a builtin `list`/`slice` receiver. Only when no
      // method of the receiver's own matched, so a real builtin method
      // still wins.
      if (const auto ufcs = try_ufcs_call(call, field, object)) {
        return *ufcs;
      }
      if (const auto reported =
              report_unknown_builtin_method(call, field, entry, object)) {
        return *reported;
      }
    }
    return std::monostate{};
  }
  }
}

/// Whether `expr` is built only from numeric literals, unary `-`/`~`, and
/// arithmetic/bitwise operators, such that every literal in it can take
/// `target` as its type. A float literal never can under an integer
/// target (`int32(2.5)` is a real conversion), and bitwise operators need
/// an integer target.
auto checker::is_literal_tree(const ast::expr &expr, type_id target) -> bool {
  const auto integer_target = session_.types_.is_integer(target);
  switch (expr.kind) {
  case ast::node_kind::literal_expr: {
    const auto kind = ast::cast<const ast::literal_expr>(expr).lit_kind;
    return kind == token_kind::int_lit ||
           (kind == token_kind::float_lit && !integer_target);
  }
  case ast::node_kind::group_expr: {
    const auto &group = ast::cast<const ast::group_expr>(expr);
    return group.inner != nullptr && is_literal_tree(*group.inner, target);
  }
  case ast::node_kind::unary_expr: {
    const auto &unary = ast::cast<const ast::unary_expr>(expr);
    const auto op_fits = unary.op == ast::unary_op::neg ||
                         (unary.op == ast::unary_op::bit_not && integer_target);
    return op_fits && unary.operand != nullptr &&
           is_literal_tree(*unary.operand, target);
  }
  case ast::node_kind::binary_expr: {
    const auto &binary = ast::cast<const ast::binary_expr>(expr);
    const auto op_fits = ast::is_arithmetic(binary.op) ||
                         (ast::is_integer_bitwise(binary.op) && integer_target);
    return op_fits && binary.lhs != nullptr && binary.rhs != nullptr &&
           is_literal_tree(*binary.lhs, target) &&
           is_literal_tree(*binary.rhs, target);
  }
  default:
    return false;
  }
}

/// Types a constructor-style conversion call `target_name(value)` (e.g.
/// `float64(n)`), Cinder's replacement for a cast operator. Reports a
/// no-conversion-exists error when the source is a known non-numeric,
/// non-boolean type and the target is numeric.
auto checker::check_conversion_call(const ast::call_expr &call,
                                    std::string_view target_name) -> type_id {
  const auto target = session_.types_.builtin(target_name);
  if (call.args.size() != 1) {
    error(call.span,
          std::format("conversion to `{}` takes exactly one value, found {}",
                      target_name, call.args.size()),
          "wrong number of arguments");
    infer_call_args_loosely(call);
    return target;
  }
  const auto &arg = call.args.front();
  if (arg.value == nullptr) {
    return target;
  }
  // A value built only from literals is typed at the conversion's target,
  // exactly as under `let v: T = ...`, not at the `int32` default and then
  // converted: `int64(4294967296 * 2)` is a wide constant, `float64(7 / 2)`
  // is `3.5`, and `uint8(300 - 100)` is rejected because `300` does not
  // fit in `uint8`.
  const auto arg_expected =
      session_.types_.is_numeric(target) && is_literal_tree(*arg.value, target)
          ? target
          : k_unknown_type;
  const auto found = strip_refs(infer_expr(*arg.value, arg_expected));
  if (session_.types_.is_numeric(target) &&
      !session_.types_.is_unknown(found) &&
      !session_.types_.is_numeric(found) &&
      !session_.types_.is_boolean(found)) {
    error_with_help(
        arg.value->span,
        std::format("no conversion from `{}` to `{}`",
                    session_.types_.display(found), target_name),
        "cannot convert this value",
        std::format("A conversion exists when `{}` provides a `from` for "
                    "the source type; the numeric types convert between "
                    "one another.",
                    target_name));
  }
  return target;
}

/// Resolves a call whose callee is `object.fn_name(...)` against a real
/// declaration when `object` names a module or a type rather than a
/// value — a fully module-qualified free function (`std.io.open(...)`,
/// `object` a multi-segment `module_path_expr`), a free function reached
/// through a whole-module `use` import (`console.println(...)`, `object`
/// a single-segment `ident_expr` naming an imported module), or a
/// type-qualified associated function (`io_error.from(...)`, `object` a
/// single-segment `ident_expr` naming a type) — dispatched like
/// `find_method` but with no receiver, so a match whose first parameter is
/// `self` is rejected (that's a genuine instance-method call, handled the
/// existing way by the rest of `infer_method_call`). Returns `nullopt`
/// when `object` is value-rooted or doesn't resolve to any of these,
/// leaving the caller to fall back to today's method-call/unknown
/// handling. On a match, records a `resolved_callee` so `hir::lower_call`
/// can rebuild this exact call site without redoing this resolution.
///
/// Called from `infer_method_call` rather than dispatched on
/// `call.callee`'s own kind in `infer_call`, because the parser never
/// produces a bare `module_path_expr` callee for a call — a dotted name
/// immediately followed by `(` always parses as `field_expr` (see
/// `parser::parse_ident_or_path_expr`), with `object` holding whatever
/// dotted prefix, if any, preceded the call-adjacent final segment.
/// The concrete type a static call's receiver names, when that receiver is
/// written as a generic application (`list[int32].from_iter(...)`).
///
/// The receiver reaches the checker as an *expression*: `gen[int32]`
/// parses as an index into something named `gen`, and `map[str, int32]` as
/// a call of `map`, because in expression position that is all the parser
/// could know. `expr_as_named_type` already rebuilds exactly these shapes,
/// so the receiver is reinterpreted by the same route an explicit type
/// argument takes and reaches the same builtins, user types, and imports.
///
/// Resolution is deliberately `quiet`, and a head naming a value declines
/// outright: this runs speculatively on every `<expr>.method(...)` whose
/// object is an index or a call, and the overwhelming majority of those
/// are real indexing and real calls. Reporting an undefined *type* for
/// `values[0].method()` would be nonsense, so anything unresolved simply
/// leaves the caller on its existing path, diagnostics included.
auto checker::resolve_applied_type_receiver(const ast::expr &object)
    -> std::optional<type_id> {
  auto named = expr_as_named_type(object);
  if (named == nullptr || named->type_args.empty()) {
    return std::nullopt;
  }
  // A head that names a value is indexing or invocation, whatever it might
  // also spell as a type. Same test the `ident_expr` case at the top of
  // `infer_qualified_call` applies.
  if (named->path.size() == 1 &&
      (lookup_value(named->path.front()) != nullptr ||
       named->path.front() == "self")) {
    return std::nullopt;
  }

  const auto &type = *named;
  session_.synthesized_types_.push_back(std::move(named));
  auto ctx = current_resolve_ctx();
  ctx.quiet = true;
  const auto resolved = resolve_type(type, ctx);
  return session_.types_.is_unknown(resolved) || resolved == k_error_type
             ? std::nullopt
             : std::optional{resolved};
}

/// The finder for every static-member spelling. Each one resolves to a
/// `method_entry` and the type the static is called on, and `check_call`
/// does the rest the same way for all of them, so `list[int32].from_iter`
/// and a `C.from_iter` that solved `C` to `list[int32]` name the same
/// instance. A bare generic type name (`box.make(40)`) leaves `self_type`
/// unknown: the block's parameters solve from the arguments.
auto checker::find_static_callee(const ast::call_expr &call,
                                 const ast::field_expr &field)
    -> static_callee {
  const auto &object = *field.object;
  const auto &fn_name = field.field_name;
  const auto takes_self = [&](const method_entry &method) -> bool {
    return !method.decl->params.empty() &&
           param_name_of(method.decl->params.front()) == "self";
  };
  const auto callee_for = [&](const method_entry &method, type_id self_type,
                              std::string target_name) -> call_callee {
    auto display_name = std::format("{}.{}", target_name, fn_name);
    return call_callee{.form = callee_form::member,
                       .decl = method.decl,
                       .owner = method.owner,
                       .decl_file = method.file_id,
                       .method = &method,
                       .self_type = self_type,
                       .target_name = std::move(target_name),
                       .explicit_args = method_explicit_generic_args(field),
                       .callee_node = &field,
                       .display_name = std::move(display_name)};
  };
  // A static member of `target`, found the way a method call finds one:
  // a builtin target (`list[int32]`) has no `type_decl` to key on, and its
  // impl and `extend` members live in the constructor-name-keyed tables.
  const auto static_of = [&](type_id target) -> const method_entry * {
    const auto *method =
        resolve_method(session_.types_.entry(target), target, fn_name).method;
    return method != nullptr && !takes_self(*method) ? method : nullptr;
  };

  if (object.kind == ast::node_kind::index_expr ||
      object.kind == ast::node_kind::call_expr) {
    // A static call on a generic *application*: `list[int32].from_iter(it)`,
    // `wrap[int32].make(9)`.
    const auto target = resolve_applied_type_receiver(object);
    if (!target.has_value()) {
      return std::monostate{};
    }
    const auto target_name = session_.types_.display(*target);
    if (const auto *method = static_of(*target)) {
      return callee_for(*method, *target, target_name);
    }
    // A `self` method or a free function named on the type, not on a value,
    // has no receiver; `report_missing_type_static` says which. A value the
    // parser also read as an application never gets here:
    // `resolve_applied_type_receiver` declines a head that names a value.
    report_missing_type_static(field, *target, target_name);
    infer_call_args_loosely(call);
    return k_error_type;
  }

  const auto root_path = qualified_call_root(object);
  if (!root_path.has_value() || root_path->size() != 1) {
    return std::monostate{};
  }
  const auto &root = root_path->front();

  // A prelude constructor (`option.pure(...)`): an associated function an
  // impl of a higher-kinded trait provides for that constructor. Its type
  // parameters solve from the arguments.
  if (builtin_generic_arity(root).has_value()) {
    if (const auto *method = method_lookup().constructor_method(root, fn_name);
        method != nullptr && !takes_self(*method)) {
      return callee_for(*method, k_unknown_type, root);
    }
  }

  // A type parameter (`C.from_iter(it)` inside `def collect[I, C](self: I)
  // -> C`), substituted for whatever this instance solved it to; dispatch is
  // static and monomorphic, so nothing here needs the trait to be
  // object-safe (§5.2).
  if (const auto bound = lookup_type_param(root)) {
    // Still a template: `C` is the body's own parameter, and its static
    // members are what its bounds promise (phase 9, rule 2).
    if (is_rigid_param(*bound)) {
      auto callee = find_bound_callee(call, field, *bound,
                                      /*static_call=*/true);
      if (!callee.has_value()) {
        return k_error_type;
      }
      return *std::move(callee);
    }
    if (mentions_abstract_type(*bound) || session_.types_.is_unknown(*bound)) {
      infer_call_args_loosely(call);
      return k_unknown_type;
    }
    if (const auto *method = static_of(*bound)) {
      return callee_for(*method, *bound, session_.types_.display(*bound));
    }
    // `C` is bound, so this is a type-parameter static that does not
    // resolve. Reported here: falling through would call the parameter
    // itself an undefined name and say nothing about the type it stands for.
    report_missing_type_param_static(field, root, *bound, fn_name);
    infer_call_args_loosely(call);
    return k_error_type;
  }

  // A builtin scalar (`int64.zero()`): its static members come from
  // `extend` blocks and trait impls (todo 9).
  if (is_builtin_scalar_name(root)) {
    const auto type = session_.types_.builtin(root);
    if (const auto *method = static_of(type)) {
      return callee_for(*method, type, root);
    }
    report_missing_type_static(field, type, root);
    infer_call_args_loosely(call);
    return k_error_type;
  }

  // A type name (`io_error.from(...)`, `box.make(40)`).
  if (const auto found_type = find_type_decl_by_name(root)) {
    const auto type =
        make_user_type(*found_type->first, found_type->second, {});
    if (const auto *method = find_method(session_.types_.entry(type), fn_name);
        method != nullptr && !takes_self(*method)) {
      // The bare name of a generic type says nothing about its arguments.
      const auto generic = !found_type->first->type_params.empty();
      return callee_for(*method, generic ? k_unknown_type : type,
                        found_type->first->name);
    }
    // A type with no static function by this name. Answering nothing here
    // let the call through with no type, and lowering then called the
    // program valid but unbuildable.
    report_missing_type_static(field, type, root);
    infer_call_args_loosely(call);
    return k_error_type;
  }
  return std::monostate{};
}

/// Explains a `C.member(...)` whose solved type has no such static member.
///
/// This is the *common* failure of an inferred, unbounded generic like
/// `collect` (§6): `C` is decoration-bounded at most, so an unsuitable one
/// isn't caught at the call site and surfaces here instead. The message
/// therefore has to carry its own context — which parameter, what it was
/// solved to — because the instantiation notes supply the rest.
auto checker::report_missing_type_param_static(const ast::field_expr &field,
                                               const std::string &param_name,
                                               type_id target,
                                               const std::string &fn_name)
    -> void {
  error_with_help(
      field.span,
      std::format("no static `{}` on type `{}`", fn_name,
                  session_.types_.display(target)),
      std::format("`{}` was solved to `{}`, which provides no `{}`", param_name,
                  session_.types_.display(target), fn_name),
      std::format("`{}` here stands for whatever type this instance solved "
                  "it to, and the call needs that type to provide a static "
                  "`{}`. Either add one in an `extend {}:` block, or use a "
                  "type for `{}` that already has one.",
                  param_name, fn_name, session_.types_.display(target),
                  param_name));
}

auto checker::infer_qualified_call(const ast::call_expr &call,
                                   const ast::field_expr &field)
    -> std::optional<type_id> {
  const auto &object = *field.object;
  if (object.kind != ast::node_kind::index_expr &&
      object.kind != ast::node_kind::call_expr) {
    const auto root = qualified_call_root(object);
    if (!root.has_value()) {
      return std::nullopt;
    }
    if (const auto found = find_path_function(*root, field.field_name)) {
      // `limits.max[int32]()` carries its brackets on the field (todo 8).
      return check_function_call(call, field, *found,
                                 method_explicit_generic_args(field));
    }
  }
  auto found = find_static_callee(call, field);
  if (const auto *callee = std::get_if<call_callee>(&found)) {
    return check_call(call, *callee);
  }
  if (const auto *answered = std::get_if<type_id>(&found)) {
    return *answered;
  }
  return std::nullopt;
}

auto checker::report_missing_type_static(const ast::field_expr &field,
                                         type_id type,
                                         const std::string &type_name)
    -> void {
  const auto &entry = session_.types_.entry(type);
  const auto *method = resolve_method(entry, type, field.field_name).method;
  if (method != nullptr) {
    error_with_help(
        field.span,
        std::format("`{}` takes `self`, so it is called on a value of `{}`",
                    field.field_name, type_name),
        std::format("called on the type `{}`", type_name),
        std::format("Call it on a value instead: `value.{}(...)`.",
                    field.field_name));
    return;
  }
  error_with_help(
      field.span,
      std::format("`{}` has no static function `{}`", type_name,
                  field.field_name),
      "no such static function",
      std::format("A function called on a type name is a `static def` "
                  "declared in an `extend {}` block or in an impl of a trait "
                  "for `{}`.",
                  type_name, type_name));
}

auto checker::find_callable_decl(std::string_view name)
    -> std::optional<callable_lookup> {
  if (file_.module_ != nullptr) {
    if (const auto it = file_.module_->functions.find(std::string(name));
        it != file_.module_->functions.end()) {
      return callable_lookup{.decl = it->second.decl,
                             .owner = file_.module_,
                             .file_id = it->second.file_id};
    }
  }
  if (const auto *import = find_import(name)) {
    // An import that binds a *module* (`use std.iter`) has no source member;
    // a module is not callable, so the search goes on to the prelude.
    if (const auto *source = import_source_module(*import)) {
      const auto member = imported_member_name(*import);
      if (const auto it = source->functions.find(member);
          it != source->functions.end()) {
        return callable_lookup{.decl = it->second.decl,
                               .owner = source,
                               .file_id = it->second.file_id};
      }
      return std::nullopt;
    }
  }
  for (const auto *source : wildcard_import_sources()) {
    if (const auto it = source->functions.find(std::string(name));
        it != source->functions.end()) {
      return callable_lookup{.decl = it->second.decl,
                             .owner = source,
                             .file_id = it->second.file_id};
    }
  }
  if (const auto found = find_prelude_function(name)) {
    const auto &[fn, owner] = *found;
    return callable_lookup{
        .decl = fn.decl, .owner = owner, .file_id = fn.file_id};
  }
  return std::nullopt;
}

auto checker::qualified_call_root(const ast::expr &object)
    -> std::optional<std::vector<std::string>> {
  if (object.kind == ast::node_kind::ident_expr) {
    const auto &ident = ast::cast<const ast::ident_expr>(object);
    if (dotted_root_is_value(ident.name)) {
      return std::nullopt; // a real value, not a module/type reference
    }
    return std::vector<std::string>{ident.name};
  }
  if (object.kind == ast::node_kind::module_path_expr) {
    const auto &path = ast::cast<const ast::module_path_expr>(object);
    if (path.segments.empty() || dotted_root_is_value(path.segments.front())) {
      return std::nullopt; // a value-rooted field chain, not a module path
    }
    return path.segments;
  }
  return std::nullopt;
}

auto checker::find_path_function(const std::vector<std::string> &root,
                                 const std::string &fn_name)
    -> std::optional<callable_lookup> {
  const auto member_of =
      [&](const module_members *owner) -> std::optional<callable_lookup> {
    if (const auto it = owner->functions.find(fn_name);
        it != owner->functions.end()) {
      return callable_lookup{.decl = it->second.decl,
                             .owner = owner,
                             .file_id = it->second.file_id};
    }
    return std::nullopt;
  };

  // Fully module-qualified: `std.io.open(...)`.
  if (const auto *owner = find_visible_module_of_path(root)) {
    if (const auto found = member_of(owner)) {
      return found;
    }
  }

  // Relative to the module being checked: `inner.val()` inside `module
  // main` is `main.inner`'s `val`. An import and a declared child can't
  // share a name (`validate_module_name_conflicts`), so skipping an
  // imported root here only keeps the two lookups from overlapping. This
  // needs the whole path plus `fn_name` as the module member, which is
  // what appending the name to `root` asks for.
  if (find_import(root.front()) == nullptr) {
    auto member_path = root;
    member_path.push_back(fn_name);
    if (const auto *owner = find_submodule_of_current_module(member_path)) {
      if (const auto found = member_of(owner)) {
        return found;
      }
    }
  }

  // A single-segment root naming an imported module (`use std.console`,
  // then `console.println(...)`).
  if (root.size() == 1) {
    if (const auto *binding = find_import(root.front());
        binding != nullptr && binding->leaf_name.empty() &&
        session_owns_path_root(binding->path)) {
      if (const auto *owner =
              index_.find_module(join_strings(binding->path, "."))) {
        if (const auto found = member_of(owner)) {
          return found;
        }
      }
    }
  }
  return std::nullopt;
}

auto checker::find_function_callee(const ast::expr &callee)
    -> std::optional<callable_lookup> {
  if (callee.kind == ast::node_kind::ident_expr) {
    const auto &name = ast::cast<const ast::ident_expr>(callee).name;
    // A local value shadowing the name is what the call calls.
    if (lookup_value(name) != nullptr) {
      return std::nullopt;
    }
    return find_callable_decl(name);
  }
  if (callee.kind == ast::node_kind::field_expr) {
    const auto &field = ast::cast<const ast::field_expr>(callee);
    if (field.object == nullptr) {
      return std::nullopt;
    }
    if (const auto root = qualified_call_root(*field.object)) {
      return find_path_function(*root, field.field_name);
    }
  }
  return std::nullopt;
}

auto checker::check_function_call(const ast::call_expr &call,
                                  const ast::expr &callee_node,
                                  const callable_lookup &found,
                                  const explicit_generic_args &explicit_args)
    -> type_id {
  // The template's record, which a concrete generic call's instance
  // overwrites (`instantiate_solved_call`).
  session_.resolved_callees_[&call] =
      resolved_callee{.decl = found.decl,
                      .owner_module = found.owner->module_name,
                      .impl_target_type = ""};
  return check_call(call, call_callee{.form = callee_form::free_function,
                                      .decl = found.decl,
                                      .owner = found.owner,
                                      .decl_file = found.file_id,
                                      .explicit_args = explicit_args,
                                      .callee_node = &callee_node,
                                      .display_name = found.decl->name});
}

/// Recognizes an explicitly instantiated call — `zeros[8]()`,
/// `pair[int32, str](a, b)` — where the brackets give the callee's
/// compile-time arguments outright instead of leaving them to be read off
/// the call's arguments.
///
/// This is the answer for a parameter no argument *could* determine: a
/// `def zeros[n: usize]() -> array[int32, n]` mentions `n` only in its
/// return type, so nothing about `zeros()` says which `n` is meant, and
/// only the call site can say. Falls through (`nullopt`) whenever the shape
/// isn't this — the same brackets spell ordinary indexing, and the base
/// naming a generic function is the only thing that tells the two apart.
auto checker::infer_explicit_generic_call(const ast::call_expr &call)
    -> std::optional<type_id> {
  auto explicit_args = explicit_generic_args{};
  const auto *base = explicit_generic_callee(*call.callee, explicit_args);
  if (base == nullptr || explicit_args.empty()) {
    return std::nullopt;
  }
  // A base that names no generic function means the brackets are indexing a
  // value, not instantiating anything.
  const auto found = find_function_callee(*base);
  if (!found.has_value() || !is_generic_template(*found->decl)) {
    return std::nullopt;
  }
  return check_function_call(call, *base, *found, explicit_args);
}

/// Recognizes `size_of[T]()`, `align_of[T]()` and `needs_drop[T]()`, the
/// type queries answered at lowering time, and records which type each asks
/// about
/// (`checked_types::layout_queries`) for the storage form to answer
/// against `runtime::value_layouts`. Both are prelude names with no
/// declaration anywhere — `size_of` used to type-check to `usize` here and
/// then reach lowering with nothing behind it, which is why
/// `size_of[int32]()` reported "no concrete checked type is available for
/// this node" for a program that was entirely correct.
///
/// The value form `size_of(expr)` is accepted too, asking about the
/// expression's own type. `expr` is still type-checked (so a mistake
/// inside it is still reported) but is never evaluated at runtime — the
/// whole call folds to a constant — so it must not be spelled with a
/// side-effecting expression. Returns `nullopt` for every other shape, and
/// for a `size_of` the user has shadowed with a real declaration or local.
auto checker::infer_layout_query_call(const ast::call_expr &call)
    -> std::optional<type_id> {
  auto bracket_args = explicit_generic_args{};
  const auto *base = call.callee->kind == ast::node_kind::ident_expr
                         ? call.callee.get()
                         : explicit_generic_callee(*call.callee, bracket_args);
  if (base == nullptr || base->kind != ast::node_kind::ident_expr) {
    return std::nullopt;
  }
  const auto &name = ast::cast<const ast::ident_expr>(*base).name;
  const auto kind = name == "size_of"      ? layout_query_kind::size_of
                    : name == "align_of"   ? layout_query_kind::align_of
                    : name == "needs_drop" ? layout_query_kind::needs_drop
                                           : layout_query_kind::size_of;
  if (name != "size_of" && name != "align_of" && name != "needs_drop") {
    return std::nullopt;
  }
  // A user is free to define their own `size_of`; the prelude never wins
  // over a real declaration or a local binding of the same name.
  if (lookup_value(name) != nullptr || find_callable_decl(name).has_value()) {
    return std::nullopt;
  }

  // `needs_drop` answers a `bool`; the two layout queries answer a `usize`.
  const auto result_type = kind == layout_query_kind::needs_drop
                               ? session_.types_.builtin("bool")
                               : session_.types_.builtin("usize");
  auto operand = k_unknown_type;
  if (bracket_args.size() == 1) {
    // `size_of[T]()` — the type is in the brackets, and the parentheses
    // must be empty.
    if (!call.args.empty()) {
      error(call.span,
            std::format("`{}[T]()` takes no value arguments, found {}", name,
                        call.args.size()),
            "remove these arguments");
    }
    // An unresolved bracket argument is *not* an error here. Inside a
    // `concept` body or an un-instantiated generic template, `T` is not a
    // resolvable type yet — `size_of[T]() <= 64` as a concept constraint
    // is the motivating case — and the instantiated copy will reach this
    // same code later with a concrete type. The query is still recorded,
    // with an unknown operand, so that lowering can say something useful
    // if one of these ever does reach it.
    if (const auto resolved = explicit_type_argument(bracket_args.front())) {
      operand = *resolved;
    }
  } else if (bracket_args.empty() && call.args.size() == 1 &&
             call.args.front().value != nullptr) {
    // `size_of(expr)` — ask about the expression's own type.
    operand = strip_refs(infer_expr(*call.args.front().value, k_unknown_type));
  } else {
    error(call.span,
          std::format("`{}` takes one type argument, as `{}[T]()`", name, name),
          "expected exactly one type argument");
    infer_call_args_loosely(call);
    return k_error_type;
  }

  session_.layout_queries_[&call] =
      layout_query{.operand = operand, .kind = kind};
  record_expr_type(*base, k_unknown_type);
  return record_expr_type(call, result_type);
}

/// Reports `operation` as needing a `machine` function, unless one is
/// already in force. Returns whether the operation is permitted, so a
/// caller can still produce a sensible type afterwards rather than
/// cascading — a rejected `machine` operation is a permission error, not
/// an "I don't know what this is" error.
auto checker::require_machine_context(source_span span,
                                      std::string_view operation) -> bool {
  if (function_.in_machine_function_) {
    return true;
  }
  error_with_help(
      span,
      std::format("{} is only allowed inside a `machine` function", operation),
      "raw memory access outside the machine layer",
      "Mark the enclosing function `machine` to take responsibility for "
      "the memory it touches, and keep it small: `machine` suspends the "
      "compiler's usual guarantees for the whole body, so the safe API "
      "belongs on the outside of it.");
  return false;
}

/// Recognizes `uninit[T, N]()` — a fixed-capacity buffer of `N` slots,
/// each sized and aligned for `T`, with no guarantee that any slot holds a
/// valid `T` (spec/specification/03-advanced/38-machine-layer.md).
///
/// Unlike every other aggregate in the language, this one is *not*
/// heap-allocated: it becomes a genuine stack slot in the enclosing frame
/// (an `alloca` on the LLVM tier, a statically-sized frame-local byte
/// range on the bytecode tier). That is the entire point of the type — a
/// `small_list[T, N]` that spills to the heap only past `N` elements has
/// no reason to exist if its inline storage is itself a heap block.
///
/// `N` must fold to a definite non-negative constant here. Inside an
/// un-instantiated generic template it will not, and the buffer type is
/// left un-sized; the monomorphized copy reaches this code again with a
/// real count.
auto checker::infer_uninit_call(const ast::call_expr &call)
    -> std::optional<type_id> {
  auto bracket_args = explicit_generic_args{};
  const auto *base = explicit_generic_callee(*call.callee, bracket_args);
  if (base == nullptr || base->kind != ast::node_kind::ident_expr ||
      bracket_args.size() != 2) {
    return std::nullopt;
  }
  const auto &name = ast::cast<const ast::ident_expr>(*base).name;
  if (name != "uninit") {
    return std::nullopt;
  }
  if (lookup_value(name) != nullptr || find_callable_decl(name).has_value()) {
    return std::nullopt;
  }
  record_expr_type(*base, k_unknown_type);
  require_machine_context(call.span, "`uninit[T, N]`");
  if (!call.args.empty()) {
    error(call.span,
          std::format("`uninit[T, N]()` takes no value arguments, found {}",
                      call.args.size()),
          "remove these arguments");
  }

  const auto element =
      explicit_type_argument(bracket_args.front()).value_or(k_unknown_type);
  auto count = std::optional<uint64_t>{};
  if (bracket_args[1].value != nullptr) {
    if (const auto folded = explicit_value_argument(*bracket_args[1].value);
        folded.has_value() && *folded >= 0) {
      count = static_cast<uint64_t>(*folded);
    }
  }
  if (!count.has_value()) {
    // No definite slot count: leave the type un-sized rather than report.
    // A template body legitimately reaches here; so does a genuinely bad
    // count, and lowering reports that one with the span in hand.
    return record_expr_type(call, session_.types_.builtin_generic(
                                      "uninit", {element, k_unknown_type}));
  }
  session_.stack_buffers_[&call] =
      stack_buffer_request{.element = element, .count = *count};
  return record_expr_type(
      call,
      session_.types_.builtin_generic(
          "uninit", {element, session_.types_.const_value(
                                  session_.types_.usize_type(), *count)}));
}

/// Recognizes `ptr_cast[U](p)` — reinterprets a raw pointer as pointing
/// at a different type. Every raw pointer has the same runtime
/// representation (an address), so this is a type-level operation with no
/// code behind it; see `checked_types::ptr_casts`.
///
/// The result's mutability follows the operand's: a `*mut byte` casts to a
/// `*mut U` and a `*byte` to a `*U`. There is deliberately no way to gain
/// mutability through a cast, because that would let `machine` code write
/// through a pointer obtained from an immutable `as_ptr()` without the
/// operation ever being spelled.
///
/// This is what makes `rt_alloc` usable: it hands back an untyped
/// `*mut byte`, and a collection storing `T`s has to say, once, that the
/// block holds `T`s.
auto checker::infer_ptr_cast_call(const ast::call_expr &call)
    -> std::optional<type_id> {
  auto bracket_args = explicit_generic_args{};
  const auto *base = explicit_generic_callee(*call.callee, bracket_args);
  if (base == nullptr || base->kind != ast::node_kind::ident_expr ||
      bracket_args.size() != 1) {
    return std::nullopt;
  }
  const auto &name = ast::cast<const ast::ident_expr>(*base).name;
  if (name != "ptr_cast") {
    return std::nullopt;
  }
  if (lookup_value(name) != nullptr || find_callable_decl(name).has_value()) {
    return std::nullopt; // a real `ptr_cast` the user declared wins
  }
  record_expr_type(*base, k_unknown_type);

  if (call.args.size() != 1 || call.args.front().value == nullptr) {
    error(call.span,
          std::format("`ptr_cast[U]` takes exactly one pointer argument, "
                      "found {}",
                      call.args.size()),
          "expected one argument");
    infer_call_args_loosely(call);
    return k_error_type;
  }
  const auto written = infer_expr(*call.args.front().value, k_unknown_type);
  const auto operand = strip_refs(written);
  const auto &operand_entry = session_.types_.entry(operand);
  // A reference is the address of the place it borrows, so `ptr_cast[T]`
  // of a `&T`/`&mut T` is a raw pointer to that place: how `machine` code
  // moves a value into or out of a borrowed place (`swap`, `replace`).
  // Mutability follows the reference, as it follows a pointer operand.
  if (operand_entry.kind != type_kind::ptr_kind &&
      !session_.types_.is_unknown(written) && written != k_error_type &&
      session_.types_.entry(written).kind == type_kind::ref_kind) {
    const auto is_mut = session_.types_.entry(written).is_mut;
    require_machine_context(call.span, "`ptr_cast`");
    const auto target =
        explicit_type_argument(bracket_args.front()).value_or(k_unknown_type);
    const auto result = session_.types_.ptr_to(target, is_mut);
    if (!session_.types_.is_unknown(target)) {
      session_.ptr_casts_[&call] = result;
    }
    return record_expr_type(call, result);
  }
  if (operand_entry.kind != type_kind::ptr_kind) {
    if (session_.types_.is_unknown(operand) || operand == k_error_type) {
      return record_expr_type(call, k_unknown_type);
    }
    error(call.args.front().value->span,
          std::format("`ptr_cast` requires a raw pointer or a reference, "
                      "found `{}`",
                      session_.types_.display(operand)),
          "not a `*T`, `*mut T`, `&T` or `&mut T`");
    return k_error_type;
  }
  require_machine_context(call.span, "`ptr_cast`");
  // An unresolved target type is not an error: inside an un-instantiated
  // generic template (`ptr_cast[T](raw)` in `std.mem`'s `alloc[T]`) there
  // is no concrete `T` yet, and the monomorphized copy reaches this code
  // again with one. The template itself is never lowered. Same reasoning
  // as `infer_layout_query_call`'s unresolved-argument case.
  const auto target =
      explicit_type_argument(bracket_args.front()).value_or(k_unknown_type);
  const auto result = session_.types_.ptr_to(target, operand_entry.is_mut);
  if (!session_.types_.is_unknown(target)) {
    session_.ptr_casts_[&call] = result;
  }
  return record_expr_type(call, result);
}

/// Recognizes `slice_from_raw_parts(p, len)` / `slice_mut_from_raw_parts(p,
/// len)` — builds a `slice[T]`/`slice_mut[T]` view over memory the caller
/// already has a raw pointer to, with `T` read straight off `p`'s own
/// pointee type (no explicit `[T]` needed, unlike `ptr_cast`, since a
/// pointer's pointee already says what `T` is unambiguously).
///
/// This is the one primitive a library collection needs and had no way to
/// get: `array`/`slice`/`str` are the only receivers the compiler still
/// range-indexes directly, so a
/// type like `list[T]` that wants to hand back a view of *part* of its own
/// storage — as `std.algo`'s sort family does via `&mut xs[a..b]` — has no
/// way to build one from its own `as_ptr`/`as_mut_ptr` otherwise.
auto checker::infer_slice_from_raw_parts_call(const ast::call_expr &call)
    -> std::optional<type_id> {
  if (call.callee == nullptr ||
      call.callee->kind != ast::node_kind::ident_expr) {
    return std::nullopt;
  }
  const auto &name = ast::cast<const ast::ident_expr>(*call.callee).name;
  const auto is_mut = name == "slice_mut_from_raw_parts";
  if (!is_mut && name != "slice_from_raw_parts") {
    return std::nullopt;
  }
  if (lookup_value(name) != nullptr || find_callable_decl(name).has_value()) {
    return std::nullopt; // a real declaration of this name wins
  }
  record_expr_type(*call.callee, k_unknown_type);
  if (call.args.size() != 2 || call.args[0].value == nullptr ||
      call.args[1].value == nullptr) {
    error(call.span,
          std::format("`{}` takes exactly two arguments: a pointer and a "
                      "length",
                      name),
          "expected two arguments");
    infer_call_args_loosely(call);
    return record_expr_type(call, k_error_type);
  }
  require_machine_context(call.span, std::format("`{}`", name));
  const auto ptr_type =
      strip_refs(infer_expr(*call.args[0].value, k_unknown_type));
  const auto &ptr_entry = session_.types_.entry(ptr_type);
  infer_expr(*call.args[1].value, session_.types_.builtin("usize"));
  if (ptr_entry.kind != type_kind::ptr_kind) {
    if (session_.types_.is_unknown(ptr_type) || ptr_type == k_error_type) {
      return record_expr_type(call, k_unknown_type);
    }
    error(call.args[0].value->span,
          std::format("`{}` requires a raw pointer, found `{}`", name,
                      session_.types_.display(ptr_type)),
          "not a `*T` or `*mut T`");
    return record_expr_type(call, k_error_type);
  }
  if (is_mut && !ptr_entry.is_mut) {
    error(call.args[0].value->span,
          std::format("`{}` requires a `*mut T` pointer, found `{}`", name,
                      session_.types_.display(ptr_type)),
          "this pointer is not mutable");
    return record_expr_type(call, k_error_type);
  }
  const auto element = ptr_entry.result;
  const auto result = session_.types_.builtin_generic(
      is_mut ? "slice_mut" : "slice", {element});
  session_.slice_from_raw_parts_calls_.insert(&call);
  return record_expr_type(call, result);
}

/// Recognizes `implements[T, Trait]()` — a compile-time coherence-table
/// query, not an ordinary generic call. `Trait` names a *trait*, not a
/// type, so it cannot go through `solve_generic_params`/`explicit_type_
/// argument` the way every other bracketed type argument does (that path
/// rejects a trait name outright — "is not a type this call can name").
/// `implements` has no declared `func_decl` anywhere in the stdlib; the
/// whole call is recognized here purely by shape (base name `implements`,
/// exactly two bracketed arguments, no parens arguments), the same way
/// `T.kind()` is recognized purely by field-call shape rather than
/// resolving to a declared method. Always types as `bool`; the checker
/// answers it from the coherence table (`fold_implements`) and lowering
/// emits that constant.
auto checker::infer_implements_call(const ast::call_expr &call)
    -> std::optional<type_id> {
  auto explicit_args = explicit_generic_args{};
  const auto *base = explicit_generic_callee(*call.callee, explicit_args);
  if (base == nullptr || base->kind != ast::node_kind::ident_expr ||
      explicit_args.size() != 2 || !call.args.empty()) {
    return std::nullopt;
  }
  if (ast::cast<const ast::ident_expr>(*base).name != "implements") {
    return std::nullopt;
  }
  const auto &t_arg = explicit_args[0];
  const auto &trait_arg = explicit_args[1];
  if (t_arg.value == nullptr ||
      t_arg.value->kind != ast::node_kind::ident_expr ||
      trait_arg.value == nullptr ||
      trait_arg.value->kind != ast::node_kind::ident_expr) {
    error_with_help(
        call.span,
        "`implements[T, Trait]`'s arguments must be plain type/trait names",
        "expected two plain names here",
        "`implements` takes a type and a trait name, each written as a "
        "bare identifier in brackets — not an expression, a qualified "
        "path, or a parameterized type.");
    return k_error_type;
  }
  const auto &t_name = ast::cast<const ast::ident_expr>(*t_arg.value).name;
  if (!lookup_type_param(t_name).has_value() &&
      !find_type_decl_by_name(t_name).has_value() &&
      !is_builtin_scalar_name(t_name)) {
    error_with_help(
        t_arg.span, std::format("`{}` does not name a known type here", t_name),
        "expected a type here",
        "`implements[T, Trait]`'s `T` has to name a type that is in "
        "scope — a builtin, a user type, or an in-scope generic type "
        "parameter.");
    return k_error_type;
  }
  const auto &trait_name =
      ast::cast<const ast::ident_expr>(*trait_arg.value).name;
  auto named = ast::named_type{};
  named.span = trait_arg.value->span;
  named.path = {trait_name};
  const auto term = resolve_satisfaction_term(named, current_resolve_ctx());
  if (!term.has_value()) {
    error_with_help(
        trait_arg.span,
        std::format("`{}` does not name a trait or concept here", trait_name),
        "expected a trait or concept here",
        "`implements[T, C]`'s `C` has to name a trait or a concept (such "
        "as `copy`) that is in scope.");
    return k_error_type;
  }
  record_expr_type(*base, k_unknown_type);
  // The answer is a constant of the concrete type: lowering emits it in
  // place of the call (`fold_implements`). A template's answer waits for
  // each instance's type. The term is resolved here, where it is written,
  // so each instance asks about the same trait or concept.
  if (const auto type = explicit_type_argument(t_arg)) {
    if (mentions_template_param(*type)) {
      defer_to_instances(call,
                         [this, &call, term = *term,
                          type = *type](instance_subst &subst) -> void {
                           fold_implements(*clone_of(subst, &call),
                                           substitute_type(type, subst), term);
                         });
    } else {
      fold_implements(call, *type, *term);
    }
  }
  return session_.types_.builtin("bool");
}

/// Checks a call, then gates the result if it hands a raw pointer back.
///
/// A `*T`/`*mut T` escaping into an ordinary function is a raw-memory
/// operation no matter which call produced it, so the gate lives here
/// rather than on a list of blessed method names. It has to: since `list`
/// became an ordinary stdlib type (`src/std/list.cn`), its `as_ptr`/
/// `as_mut_ptr` are `pub machine def`s like any other user method, and a
/// name-based gate over the *builtin* method table no longer sees them.
/// Calling a `machine def` from safe code stays legal — that is how the
/// whole stdlib is written, `list.push` included — so it is the pointer in
/// the result type, not the callee's modifier, that draws the line.
auto checker::infer_call(const ast::call_expr &call, type_id expected)
    -> type_id {
  const auto result = infer_call_unguarded(call, expected);
  if (!function_.in_machine_function_ &&
      session_.types_.entry(result).kind == type_kind::ptr_kind) {
    require_machine_context(call.span, called_name_for_diagnostic(call));
  }
  return result;
}

/// Names the callee of `call` for a diagnostic — ``` `as_mut_ptr` ``` for a
/// method or free call, and a plain description when the callee is some
/// other expression (a call through a stored function value, say) and has
/// no name worth quoting.
auto checker::called_name_for_diagnostic(const ast::call_expr &call)
    -> std::string {
  if (call.callee != nullptr) {
    if (call.callee->kind == ast::node_kind::field_expr) {
      return std::format(
          "`{}`", ast::cast<const ast::field_expr>(*call.callee).field_name);
    }
    if (call.callee->kind == ast::node_kind::ident_expr) {
      return std::format("`{}`",
                         ast::cast<const ast::ident_expr>(*call.callee).name);
    }
  }
  return "handing out a raw pointer";
}

auto checker::infer_call_unguarded(const ast::call_expr &call, type_id expected)
    -> type_id {
  // Before anything else, including argument checking: every
  // instantiation path below reaches back for this by call node.
  if (!session_.types_.is_unknown(expected) && expected != k_error_type) {
    session_.call_expected_types_[&call] =
        session_.types_.strip_refinement(expected);
  }
  if (call.callee == nullptr) {
    infer_call_args_loosely(call);
    return k_unknown_type;
  }

  if (const auto proof = infer_refinement_try_from(call)) {
    return *proof;
  }

  if (call.callee->kind == ast::node_kind::field_expr) {
    return infer_method_call(call,
                             ast::cast<const ast::field_expr>(*call.callee));
  }

  // `size_of[T]()` / `align_of[T]()`. Tried before every other bracketed-
  // callee form: neither names a real declaration, so each of the shapes
  // below would fail to resolve it and the call would reach lowering with
  // no concrete type at all.
  if (const auto result = infer_layout_query_call(call)) {
    return *result;
  }
  if (const auto result = infer_ptr_cast_call(call)) {
    return *result;
  }
  if (const auto result = infer_slice_from_raw_parts_call(call)) {
    return *result;
  }
  if (const auto result = infer_uninit_call(call)) {
    return *result;
  }

  // `implements[T, Trait]()`: tried before the general explicit-generic-
  // call form below, which would otherwise try to resolve `implements` as
  // a declared function (it isn't one — see `infer_implements_call`'s
  // doc comment) and fail with "cannot find `implements`".
  if (call.callee->kind == ast::node_kind::index_expr ||
      call.callee->kind == ast::node_kind::call_expr) {
    if (const auto result = infer_implements_call(call)) {
      return *result;
    }
  }

  // `zeros[8]()` / `pair[int32, str](a, b)` / `derive_show[point]()`: the
  // brackets carry the callee's compile-time arguments.
  if (call.callee->kind == ast::node_kind::index_expr ||
      call.callee->kind == ast::node_kind::call_expr) {
    if (const auto result = infer_explicit_generic_call(call)) {
      return *result;
    }
  }

  if (call.callee->kind != ast::node_kind::ident_expr) {
    const auto callee =
        infer_expr(*call.callee, invoked_lambda_expectation(*call.callee));
    const auto &entry = session_.types_.entry(strip_refs(callee));
    if (entry.kind == type_kind::fn_kind) {
      return check_call(call,
                        call_callee{.form = callee_form::fn_value,
                                    .callee_node = call.callee.get(),
                                    .display_name = "this function value",
                                    .fn_type = callee});
    }
    infer_call_args_loosely(call);
    return k_unknown_type;
  }

  const auto &ident = ast::cast<const ast::ident_expr>(*call.callee);
  const auto &name = ident.name;

  if (is_variant_ident(ident)) {
    if (const auto variant =
            resolve_variant_value(name, &call, call.span, expected)) {
      return *variant;
    }
    emit_undefined_variant(ident.span, name, expected);
    infer_call_args_loosely(call);
    return k_error_type;
  }

  if (const auto *binding = lookup_value(name)) {
    if (binding->generic_lambda != nullptr) {
      const auto copy = record_expr_type(
          ident, instantiate_generic_lambda(
                     *binding->generic_lambda,
                     {.node = &ident, .name = ident.name, .span = ident.span},
                     /*params=*/nullptr));
      if (session_.types_.entry(copy).kind != type_kind::fn_kind) {
        infer_call_args_loosely(call);
        return k_error_type;
      }
      return check_call(call, call_callee{.form = callee_form::fn_value,
                                          .callee_node = &ident,
                                          .display_name = name,
                                          .fn_type = copy});
    }
    const auto &entry = session_.types_.entry(strip_refs(binding->type));
    if (entry.kind == type_kind::fn_kind) {
      // The callee ident is resolved straight from the scope binding
      // here, not through `infer_expr`, so (like the other direct-
      // resolution bypasses this chokepoint doc comment lists) it needs
      // its own persistence hook.
      record_expr_type(ident, binding->type);
      return check_call(call, call_callee{.form = callee_form::fn_value,
                                          .callee_node = &ident,
                                          .display_name = name,
                                          .fn_type = binding->type});
    }
    if (session_.types_.is_unknown(binding->type)) {
      infer_call_args_loosely(call);
      return k_unknown_type;
    }
    error_with_help(
        call.span, std::format("`{}` is not callable", name),
        std::format("this has type `{}`",
                    session_.types_.display(binding->type)),
        "Only functions, lambdas, and callable values can be called.");
    infer_call_args_loosely(call);
    return k_error_type;
  }

  // Same precedence as in `infer_ident`: a callee an enclosing capture
  // list left out is reported as that rather than resolving to a
  // same-named module-level function.
  if (const auto *barrier = capture_barrier_blocking(name)) {
    emit_capture_not_listed(ident.span, name, *barrier);
    infer_call_args_loosely(call);
    return k_error_type;
  }

  if (const auto found = find_callable_decl(name)) {
    return check_function_call(call, ident, *found, /*explicit_args=*/{});
  }
  // An import of a module member that is not a function (a type, say): the
  // call is not this checker's to type.
  if (const auto *import = find_import(name);
      import != nullptr && import_source_module(*import) != nullptr) {
    infer_call_args_loosely(call);
    return k_unknown_type;
  }

  if (name == "panic") {
    infer_call_args_loosely(call);
    return session_.types_.builtin("never");
  }
  if (name == "assert") {
    if (!call.args.empty() && call.args.front().value != nullptr) {
      require_bool(*call.args.front().value, "the `assert` condition");
    }
    for (size_t i = 1; i < call.args.size(); ++i) {
      if (call.args[i].value != nullptr) {
        infer_expr(*call.args[i].value, k_unknown_type);
      }
    }
    return session_.types_.builtin("unit");
  }
  if (name == "size_of") {
    infer_call_args_loosely(call);
    return session_.types_.builtin("usize");
  }
  if (name == "args") {
    infer_call_args_loosely(call);
    return resolve_list_type(session_.types_.builtin("str"));
  }
  if (name == "env") {
    infer_call_args_loosely(call);
    return session_.types_.builtin_generic("option",
                                           {session_.types_.builtin("str")});
  }
  if (name == "min" || name == "max") {
    auto first = k_unknown_type;
    for (const auto &arg : call.args) {
      if (arg.value != nullptr) {
        const auto found = infer_expr(*arg.value, first);
        if (first == k_unknown_type) {
          first = found;
        }
      }
    }
    return first;
  }

  if (is_builtin_scalar_name(name)) {
    return check_conversion_call(call, name);
  }

  if (const auto variant =
          resolve_variant_value(name, &call, call.span, expected)) {
    emit_bare_variant(ident.span, name, /*is_call=*/true);
    return *variant;
  }

  // `drop` names the prelude trait, not a function. Without this it would
  // count as a type-like name, type-check as unknown, and fail in lowering.
  if (name == "drop" && find_prelude_trait(name).has_value()) {
    error_with_help(
        call.span, "`drop` is a trait, not a function",
        "no function `drop` exists",
        "a value's `drop` runs on its own when its scope ends, so there is "
        "no function to call it early. To release a value sooner, end its "
        "scope sooner, or restructure so this is its last use.");
    infer_call_args_loosely(call);
    return k_error_type;
  }

  if (is_prelude_value_name(name) || is_type_like_name(name) ||
      builtin_generic_arity(name).has_value()) {
    infer_call_args_loosely(call);
    return k_unknown_type;
  }

  if (file_.file_has_external_wildcard_) {
    infer_call_args_loosely(call);
    return k_unknown_type;
  }

  emit_undefined_name(ident.span, name);
  infer_call_args_loosely(call);
  return k_error_type;
}

} // namespace cinder::semantic::detail
