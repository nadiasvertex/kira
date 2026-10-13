#include "checker.h"

namespace cinder::semantic::detail {

// ==========================================================================
//  Const-generic monomorphization
//  (`spec/dependent-types-design.md` §3.1, `spec/dependent-types-
//   implementation-plan.md` phase 1)
//
//  A value parameter is a compile-time argument with no runtime existence:
//  `def get[n: usize](v: array[int32, n], i: index[n])` is passed a `v` and
//  an `i`, never an `n`. Everything in the body that *mentions* `n` — the
//  array's length, the refinement's bound, the check an `index[n].try_from`
//  compiles into — therefore has nothing to compile against while `n` is
//  still a symbol.
//
//  So the template is never lowered. Instead, each call site whose arguments
//  pin `n` down to a constant (`match_value_params`, already run to check
//  the arguments) gets an *instance*: the declaration cloned, re-checked
//  with `n` bound to that constant, and named `get$3`. Instances are shared
//  per constant tuple, so a call in a loop compiles one of them.
// ==========================================================================

/// Whether `decl` is generic over compile-time *values* only — the case
/// that monomorphizes here — generic over compile-time *values*
/// (`[n: usize]`), over *types* (`[T]`), or over both at once
/// (`[n: usize, T]`). All three are one case: a template with no compiled
/// form of its own, whose every parameter one call pins down to a constant
/// or a concrete type, yielding an instance that is what actually gets
/// lowered.
auto checker::is_generic_template(const ast::func_decl &decl) -> bool {
  return !decl.type_params.empty() &&
         std::ranges::all_of(decl.type_params, [](const auto &param) -> bool {
           return !param.name.empty();
         });
}

/// Whether `decl` is a module-level free function of `owner` — the shape
/// instances are lowered as by their bare name (`hir::lower_module`). A
/// *method* is instantiated too, but through the receiver-call path, which
/// prefixes its impl target into the instance name (`vec::at$3`).
auto checker::is_free_function(const ast::func_decl &decl,
                               const module_members *owner) const -> bool {
  if (owner == nullptr) {
    return false;
  }
  const auto it = owner->functions.find(decl.name);
  return it != owner->functions.end() && it->second.decl == &decl;
}

/// One parameter's answer, appended to `solution` in declaration order.
auto checker::bind_generic_constant(generic_solution &solution,
                                    const ast::type_param &param,
                                    type_id underlying, int64_t constant)
    -> void {
  solution.const_slots.emplace(
      param.name,
      session_.types_.const_value(underlying, static_cast<uint64_t>(constant)));
  solution.values.emplace(param.name, constant);
  // A negative constant spells its minus as `m` (`shift$m1`), keeping the
  // name to characters every backend's symbol table accepts.
  solution.suffix += constant < 0 ? std::format("$m{}", -constant)
                                  : std::format("${}", constant);
}

/// A solution's answers keyed by the parameters of `params` they answer —
/// the substitution form of the scope `generic_solution` carries by name.
auto checker::solution_subst(const std::vector<ast::type_param> &params,
                             const generic_solution &solution) -> param_subst {
  auto subst = param_subst{};
  for (const auto &param : params) {
    if (param.name.empty()) {
      continue;
    }
    const auto &slots =
        param.is_value_param ? solution.const_slots : solution.type_slots;
    if (const auto found = slots.find(param.name); found != slots.end()) {
      subst.emplace(param_id(param), found->second);
    } else if (const auto other = solution.type_slots.find(param.name);
               other != solution.type_slots.end()) {
      subst.emplace(param_id(param), other->second);
    }
  }
  return subst;
}

auto checker::bind_generic_type(generic_solution &solution,
                                const ast::type_param &param, type_id solved)
    -> void {
  solution.type_slots.emplace(param.name, solved);
  solution.suffix += std::format("${}", mangle_type_for_instance(solved));
}

/// How to describe, in a diagnostic, what a copy of `decl` is made *per* —
/// read off the template's own parameters so a mixed template says both.
auto checker::instance_cardinality(const ast::func_decl &decl)
    -> std::string_view {
  const auto has_value =
      std::ranges::any_of(decl.type_params, [](const auto &param) -> bool {
        return param.is_value_param;
      });
  const auto has_type =
      std::ranges::any_of(decl.type_params, [](const auto &param) -> bool {
        return !param.is_value_param;
      });
  if (has_value && has_type) {
    return "one per combination of constant and concrete type";
  }
  return has_value ? "one per constant" : "one per concrete type";
}

/// Clones and names a generic instance, and queues its body to be checked.
///
/// The elaboration split cuts this function in half. The
/// clone is still made here and the pointer still returned here, so every
/// caller keeps getting a real declaration to record in its dispatch map
/// exactly as before. What moved is the *checking* of the cloned body,
/// which now happens in `flush_pending_instances` after the walk.
///
/// That split is the enabling move for everything after it. Checking an
/// instance body in the middle of the expression that asked for it is what
/// forces every type to be final the instant it is first observed, which is
/// the structural reason the checker grew 28 `unify_rigid` sites and four
/// solvers. Nothing is deferred *type*-wise yet — the instance's signature
/// is the template's, substituted, and no caller reads anything else off
/// the returned pointer — so this step is behavior-preserving, and the
/// phase 0 snapshot is what says so.
auto checker::find_or_check_generic_instance(
    const ast::node &call, const ast::func_decl &decl,
    const module_members *owner, std::optional<file_id_type> decl_file,
    const generic_solution &solution, const std::string &name,
    const type_scope *fixed_type_params, type_id self_type,
    const std::vector<ast::type_param> *block_type_params,
    const std::vector<type_id> *param_types) -> const ast::func_decl * {
  // A template names no instance of anything written in its own
  // parameters: `list::push$list_T_` is a function no backend can compile.
  // Each of the template's instances asks again with its own answers
  // (`defer_to_instances`).
  if (function_.current_template_ != nullptr &&
      (solution_mentions_template_param(solution) ||
       (fixed_type_params != nullptr &&
        std::ranges::any_of(*fixed_type_params,
                            [&](const auto &slot) -> auto {
                              return mentions_template_param(slot.second);
                            })) ||
       mentions_template_param(self_type))) {
    return nullptr;
  }
  const auto key = std::format("{}#{}", static_cast<const void *>(&decl), name);
  if (const auto found = session_.hk_instance_cache_.find(key);
      found != session_.hk_instance_cache_.end()) {
    return found->second;
  }

  static constexpr size_t k_max_instantiation_depth = 32;
  if (session_.instantiation_depth_ >= k_max_instantiation_depth) {
    error_with_help(
        call.span, std::format("`{}` instantiates itself endlessly", decl.name),
        "generic instantiation is too deep",
        std::format("Each call with a new compile-time argument compiles a "
                    "new copy of `{}`, so a body that calls itself at a "
                    "*different* argument every time (`{}[n]` calling "
                    "`{}[n + 1]`, say) never bottoms out. Recurse at an "
                    "argument that eventually repeats, or make the "
                    "recursion an ordinary runtime one over a parameter you "
                    "don't change.",
                    decl.name, decl.name, decl.name));
    return nullptr;
  }

  auto pairs = std::make_shared<ast::clone_map>();
  auto cloned = ast::clone_func_decl(decl, pairs.get());
  if (!cloned.has_value()) {
    auto diag =
        diagnostic(diagnostic_level::error,
                   std::format("cannot compile `{}` for this call: its body "
                               "uses a construct generic instantiation "
                               "doesn't support yet",
                               decl.name),
                   file_.file_id_);
    diag.with_label(call.span,
                    std::format("this call needs a compiled copy of `{}`, {}",
                                decl.name, instance_cardinality(decl)));
    diag.children.push_back(
        diagnostic(diagnostic_level::note, cloned.error().message,
                   decl_file.value_or(file_.file_id_))
            .with_label(cloned.error().span, "unsupported here"));
    emit_diag(diag);
    mark_error();
    return nullptr;
  }

  (*cloned)->name = name;
  const auto *instance = cloned->get();
  session_.synthesized_decls_.push_back(std::move(*cloned));
  session_.hk_instance_cache_.emplace(key, instance);
  // A compile-time call into the instance runs where its template was
  // declared (`function_owner`), not where it was instantiated.
  if (owner != nullptr) {
    cache_.function_owners_.emplace(
        instance, std::pair{owner, decl_file.value_or(file_.file_id_)});
  }
  // An instance of a function with unannotated parameters is checked
  // against the concrete types its call chose, not against leaves: seeding
  // `param_types_for`'s cache is all that takes.
  if (param_types != nullptr) {
    cache_.inferred_param_types_[instance] = *param_types;
  }

  // Recorded against the *caller's* file — the whole point of the note is
  // to name a line the user actually wrote, and by flush time `file_.file_id_`
  // is somewhere else entirely.
  auto frame = instantiation_frame{.call_span = call.span,
                                   .call_file = file_.file_id_,
                                   .instance_name = name};
  if (const auto solved = session_.expected_solved_params_.find(&call);
      solved != session_.expected_solved_params_.end()) {
    frame.context_solutions = solved->second;
  }
  auto sites = session_.instantiation_sites_;
  sites.push_back(std::move(frame));

  auto queued = pending_instance{
      .instance = instance,
      .tmpl = &decl,
      .owner = owner,
      .decl_file = decl_file,
      .solution = solution,
      .fixed_type_params =
          fixed_type_params != nullptr && !fixed_type_params->empty()
              ? std::optional(*fixed_type_params)
              : std::nullopt,
      .self_type = self_type,
      .block_type_params = block_type_params,
      .sites = std::move(sites),
      .depth = session_.instantiation_depth_ + 1,
      .enclosing_type_params = function_.type_params_,
      .clone_pairs = std::move(pairs),
      .open_params = param_types != nullptr};

  // A compile-time-only function is the one thing that cannot wait. Its
  // body is not code to compile later; it is the answer to a call being
  // checked right now, and the fold that consumes it happens in this very
  // expression. Deferring it leaves the call unfolded, and it reaches the
  // backends as a call to a function no module compiles — which is how
  // `is_signed_integer$int32` turned up missing the first time this split
  // was tried.
  if (session_.comptime_only_functions_.contains(&decl)) {
    check_instance(queued);
    return instance;
  }

  instances_.request(std::move(queued));
  return instance;
}

/// Checks every queued instance body, including the ones those bodies ask
/// for.
///
/// A worklist rather than a loop over a snapshot of the queue: checking an
/// instance can request further instances (a generic calling a generic),
/// and those must be checked too. Draining until empty is what makes this
/// equivalent to the recursion it replaced.
///
/// Each item restores the checker context captured at its request — module,
/// file, parameter slots, `self`, the enclosing type-parameter scopes, and
/// the instantiation chain its diagnostics are reported under. Restoring
/// the chain is not cosmetic: "instantiated from here, as `biggest$point`"
/// is often the only line in the message that points at code the user
/// wrote.
/// Wires every empty `[]` whose element type is now known, and reports the
/// ones still open.
///
/// Deferred rather than done at the literal, because wiring it there would
/// name `list[?]::from_array` — an instance of a type no value has. Run
/// before `flush_pending_instances`, since wiring requests instances of its
/// own.
///
/// A leaf that is still open is reported *here*, against the literal's own
/// line. That placement is the point: before phase 8 the alternative was a
/// `list[?]` reaching the standard library and being reported from inside
/// `src/std/list.cn`, which is a diagnostic about the compiler's
/// internals for a mistake in the user's own line.
auto checker::flush_leaf_literals() -> void {
  // Everything else first, then the defaults, one at a time. A literal
  // that gets its type from a real constraint must never have been
  // answered by `int32` before that constraint arrived.
  //
  // The failure case is deliberately ignored: `defaulting` is the only
  // kind wired, and its candidate is refused only when the leaf already
  // holds an incompatible solution — in which case the mismatch was
  // reported where the value met the declared type, and this would be a
  // second diagnostic for one mistake.
  (void)leaf_queue_.flush();
  flush_leaf_values();

  auto pending = std::vector<pending_leaf_literal>{};
  pending.swap(pending_leaf_literals_);
  const auto saved_file = file_.file_id_;
  const auto *saved_module = file_.module_;
  const auto *saved_current_template = function_.current_template_;
  for (const auto &leaf : pending) {
    if (leaf.skip) {
      continue;
    }
    const auto element = leaf_ctxt_.zonk(leaf.element);
    file_.file_id_ = leaf.file;
    file_.module_ = leaf.module;
    function_.current_template_ = leaf.current_template;
    if (element == leaf.element) {
      error_with_help(
          leaf.span, "cannot tell what an empty `[]` is a list of",
          "nothing here ever says what this list holds",
          "An empty `[]` takes its element type from what is later done "
          "with it — a `push`, a return, an argument whose type is "
          "already known. Nothing here pinned it down. Annotate the "
          "binding — `var xs: list[int32] = []` — or start the list with "
          "the elements it should hold.");
      continue;
    }
    if (leaf.report_only) {
      continue;
    }
    wire_default_list(*leaf.literal, element, leaf.count);
  }
  file_.file_id_ = saved_file;
  file_.module_ = saved_module;
  function_.current_template_ = saved_current_template;
}

/// Records a method call whose receiver is still open, and the obligation
/// that will run it.
///
/// The payload is an index into `pending_method_calls_`; the watches are
/// the receiver's own metavariables, so the call is retried exactly when
/// one of them is solved. Registering it does not check the arguments
/// again — those have already been checked, and are what may solve the
/// receiver in the first place. Only the elaboration waits.
///
/// `binds` is what running `finish` may solve (see `obligation::binds`):
/// empty for a call whose type is already known and only waits to name
/// an instance, `nullopt` when it re-checks code that may solve anything.
auto checker::defer_method_call(std::string_view name, type_id receiver_type,
                                std::optional<std::vector<type_id>> binds,
                                std::function<void(type_id)> finish) -> void {
  wire_leaf_queue();
  auto goal =
      std::format("`{}` on `{}`", name, session_.types_.display(receiver_type));
  const auto payload = pending_method_calls_.size();
  pending_method_calls_.push_back(
      pending_method_call{.finish = std::move(finish),
                          .receiver_type = receiver_type,
                          .goal = goal,
                          .file = file_.file_id_,
                          .module = file_.module_,
                          .current_template = function_.current_template_,
                          .sites = session_.instantiation_sites_});
  auto watches = std::vector<type_id>{};
  auto seen = std::unordered_set<type_id>{};
  collect_type_vars(receiver_type, watches, seen);
  leaf_queue_.add(infer::obligation{.kind = infer::obligation_kind::method_call,
                                    .watches = std::move(watches),
                                    .binds = std::move(binds),
                                    .goal = std::move(goal),
                                    .why = infer::k_no_cause,
                                    .payload = payload});
}

auto checker::capture_body_context() const -> body_context {
  return body_context{
      .scopes = function_.scopes_,
      .capture_barriers = function_.capture_barriers_,
      .type_params = function_.type_params_,
      .type_param_slots = session_.type_param_slots_,
      .const_param_slots = session_.const_param_slots_,
      .self_type = function_.self_type_,
      .self_assoc_types = function_.self_assoc_types_,
      .facts = function_.facts_,
      .type_name_aliases = function_.type_name_aliases_,
      .enclosing_block_type_params = session_.enclosing_block_type_params_,
      .in_machine_function = function_.in_machine_function_,
  };
}

/// Trades the body context in place for `context`; calling it again with
/// the same `context` trades back.
auto checker::swap_body_context(body_context &context) -> void {
  std::swap(function_.scopes_, context.scopes);
  std::swap(function_.capture_barriers_, context.capture_barriers);
  std::swap(function_.type_params_, context.type_params);
  std::swap(session_.type_param_slots_, context.type_param_slots);
  std::swap(session_.const_param_slots_, context.const_param_slots);
  std::swap(function_.self_type_, context.self_type);
  std::swap(function_.self_assoc_types_, context.self_assoc_types);
  std::swap(function_.facts_, context.facts);
  std::swap(function_.type_name_aliases_, context.type_name_aliases);
  std::swap(session_.enclosing_block_type_params_,
            context.enclosing_block_type_params);
  std::swap(function_.in_machine_function_, context.in_machine_function);
}

/// The lambda `let name = <lambda>` binds, when that binding is a generic
/// lambda: no annotation on the `let`, and some parameter without one.
auto checker::generic_lambda_of(const ast::let_stmt &stmt)
    -> const ast::lambda_expr * {
  if (stmt.type_annotation != nullptr || stmt.initializer == nullptr ||
      stmt.initializer->kind != ast::node_kind::lambda_expr ||
      stmt.pattern == nullptr ||
      stmt.pattern->kind != ast::node_kind::binding_pattern ||
      !stmt.else_body.empty() ||
      dynamic_cast<const ast::binding_pattern &>(*stmt.pattern).is_mut) {
    return nullptr;
  }
  const auto &lambda =
      dynamic_cast<const ast::lambda_expr &>(*stmt.initializer);
  return has_unannotated_param(lambda) ? &lambda : nullptr;
}

auto checker::has_unannotated_param(const ast::lambda_expr &lambda) -> bool {
  return std::ranges::any_of(lambda.params,
                             [](const ast::lambda_param &param) -> bool {
                               return param.type_annotation == nullptr;
                             });
}

/// A mutable binding initialized with a lambda whose parameter has no
/// annotation and nothing to infer one from. A generic lambda is a family
/// of closures, one per use, and assigning to such a binding would have
/// to replace all of them at once, so only `let` may bind one.
auto checker::report_mutable_generic_lambda(const ast::expr *initializer,
                                            bool annotated,
                                            std::string_view name) -> bool {
  if (annotated || initializer == nullptr ||
      initializer->kind != ast::node_kind::lambda_expr ||
      !has_unannotated_param(
          dynamic_cast<const ast::lambda_expr &>(*initializer))) {
    return false;
  }
  error_with_help(
      initializer->span,
      std::format("`{}` is mutable, so its lambda needs annotated "
                  "parameters",
                  name),
      "a parameter here has no type",
      std::format(
          "A lambda parameter with no annotation is generic, and each use "
          "of a generic lambda compiles its own copy. A mutable binding "
          "holds one value, so it needs one type. Annotate the parameters "
          "(for example `(x: int32) => ...`), give `{}` a function type "
          "(`: fn(int32) -> int32`), or bind it with `let`.",
          name));
  return true;
}

/// Binds `let name = <lambda>` as a generic lambda. Its body is not checked
/// here: like an implicit-generic function, it is checked once per use, for
/// that use's types.
auto checker::bind_generic_lambda(const ast::let_stmt &stmt,
                                  const ast::lambda_expr &lambda) -> void {
  const auto &binding =
      dynamic_cast<const ast::binding_pattern &>(*stmt.pattern);
  generic_lambda_sites_.insert_or_assign(
      &lambda, generic_lambda_site{.context = capture_body_context(),
                                   .name = binding.name,
                                   .pattern = stmt.pattern.get()});
  generic_lambda_instances_.try_emplace(&lambda);
  bind_value(binding.name, k_unknown_type,
             binding.is_mut ? binding_origin::mut_binding
                            : binding_origin::let_binding,
             binding.span);
  if (!function_.scopes_.empty()) {
    if (const auto found = function_.scopes_.back().find(binding.name);
        found != function_.scopes_.back().end()) {
      found->second.generic_lambda = &lambda;
    }
  }
}

/// Checks a fresh copy of the generic lambda `tmpl` for the use `ident`,
/// and returns the copy's `fn` type. `params` gives the parameter types a
/// use in value position expects; a call passes `nullopt`, and each
/// unannotated parameter starts as a fresh leaf its arguments then solve.
auto checker::instantiate_generic_lambda(const ast::lambda_expr &tmpl,
                                         const generic_lambda_use &ident,
                                         const std::vector<type_id> *params)
    -> type_id {
  auto &site = generic_lambda_sites_.at(&tmpl);
  auto cloned = ast::clone_expr(tmpl);
  if (!cloned.has_value()) {
    error_with_help(
        cloned.error().span,
        std::format("`{}` cannot be copied for this use: {}", ident.name,
                    cloned.error().message),
        "this part of the lambda",
        "A lambda with an unannotated parameter is copied for each use. "
        "Annotate its parameters so it has a single type instead.");
    return k_error_type;
  }
  const auto &copy = dynamic_cast<const ast::lambda_expr &>(**cloned);
  synthesized_lambdas_.push_back(std::move(*cloned));

  auto param_types = std::vector<type_id>{};
  for (size_t i = 0; i < copy.params.size(); ++i) {
    if (copy.params[i].type_annotation != nullptr) {
      param_types.push_back(k_unknown_type);
    } else if (params != nullptr && i < params->size()) {
      param_types.push_back((*params)[i]);
    } else {
      param_types.push_back(leaf_ctxt_.fresh_type(
          std::format("a parameter of `{}` in this use", ident.name),
          source_location{.file_id = file_.file_id_, .span = ident.span}));
    }
  }

  session_.instantiation_sites_.push_back(
      instantiation_frame{.call_span = ident.span,
                          .call_file = file_.file_id_,
                          .instance_name = std::string(ident.name),
                          .context_solutions = {},
                          .lambda_use = true});
  swap_body_context(site.context);
  const auto type = infer_expr(
      copy, session_.types_.fn_of(std::move(param_types), k_unknown_type));
  swap_body_context(site.context);
  session_.instantiation_sites_.pop_back();

  // A capture list the copy used belongs to a lambda the use stands in
  // too; tell its unused-capture check what the copy read.
  for (const auto &used : site.context.capture_barriers) {
    for (auto &live : function_.capture_barriers_) {
      if (live.lambda == used.lambda) {
        live.used.insert(used.used.begin(), used.used.end());
      }
    }
  }

  auto &instances = generic_lambda_instances_[&tmpl];
  if (instances.empty() && site.pattern != nullptr) {
    // The binding's own record, for the ownership checker: what kind of
    // value `f` holds. Every copy is a closure, so the first one says.
    record_expr_type(*site.pattern, type);
  }
  instances.push_back(&copy);
  generic_lambda_refs_[ident.node] = &copy;
  generic_lambda_uses_.insert_or_assign(
      &copy, checked_types::generic_lambda_use{.name = std::string(ident.name),
                                               .span = ident.span});
  return type;
}

/// A use of a generic lambda's name anywhere but as a call's callee. Only
/// a use that expects a function type says what the copy's parameters are.
auto checker::generic_lambda_value(const generic_lambda_use &ident,
                                   const value_binding &binding,
                                   type_id expected) -> type_id {
  const auto &lambda = *binding.generic_lambda;
  const auto &entry = session_.types_.entry(strip_refs(expected));
  if (entry.kind == type_kind::fn_kind &&
      entry.args.size() == lambda.params.size()) {
    const auto params = entry.args;
    return instantiate_generic_lambda(lambda, ident, &params);
  }
  auto unannotated = std::string{};
  for (const auto &param : lambda.params) {
    if (param.type_annotation == nullptr && param.pattern != nullptr &&
        param.pattern->kind == ast::node_kind::binding_pattern) {
      unannotated =
          dynamic_cast<const ast::binding_pattern &>(*param.pattern).name;
      break;
    }
  }
  auto diag = diagnostic(
      diagnostic_level::error,
      std::format("cannot tell which type of `{}` is meant here", ident.name),
      file_.file_id_);
  diag.with_label(ident.span, "used as a value, not called");
  diag.with_secondary_label(binding.span, "declared here");
  diag.with_help(std::format(
      "{} has no annotation, so `{}` is generic: each call compiles it for "
      "that call's argument types. Used as a value it needs one type, and "
      "nothing here says which. Annotate the parameter (for example "
      "`({}: int32) => ...`), or use `{}` where a function type is "
      "expected.",
      unannotated.empty() ? std::string("a parameter")
                          : std::format("`{}`", unannotated),
      ident.name, unannotated.empty() ? std::string("x") : unannotated,
      ident.name));
  emit_diag(diag);
  mark_error();
  return k_error_type;
}

/// What a lambda called where it is written (`((x) => x + 1)(0)`) is
/// checked against. It has exactly one use, so it needs no copies: each
/// unannotated parameter is a fresh leaf, solved by this call's arguments.
/// `k_unknown_type` for any other callee.
auto checker::invoked_lambda_expectation(const ast::expr &callee) -> type_id {
  const auto *inner = &callee;
  while (inner->kind == ast::node_kind::group_expr) {
    const auto &group = dynamic_cast<const ast::group_expr &>(*inner);
    if (group.inner == nullptr) {
      return k_unknown_type;
    }
    inner = group.inner.get();
  }
  if (inner->kind != ast::node_kind::lambda_expr) {
    return k_unknown_type;
  }
  const auto &lambda = dynamic_cast<const ast::lambda_expr &>(*inner);
  if (!has_unannotated_param(lambda)) {
    return k_unknown_type;
  }
  auto params = std::vector<type_id>{};
  for (const auto &param : lambda.params) {
    params.push_back(
        param.type_annotation != nullptr
            ? k_unknown_type
            : leaf_ctxt_.fresh_type("a parameter of this lambda",
                                    source_location{.file_id = file_.file_id_,
                                                    .span = param.span}));
  }
  return session_.types_.fn_of(std::move(params), k_unknown_type);
}

/// Gives an explicit generic instance its own copies of every generic
/// lambda its template's body made copies of, before the template's
/// records are copied onto the instance: the records of those copies are
/// the template's too, and are found through the same clone map.
auto checker::clone_generic_lambdas_for(pending_instance &item) -> void {
  if (item.clone_pairs == nullptr || generic_lambda_instances_.empty()) {
    return;
  }
  auto merged = std::make_shared<ast::clone_map>(*item.clone_pairs);
  auto added_any = false;
  // A copy can bind a generic lambda of its own, which this pass then
  // finds in the map it just grew.
  for (auto grew = true; grew;) {
    grew = false;
    auto added =
        std::vector<std::pair<const ast::lambda_expr *,
                              std::vector<const ast::lambda_expr *>>>{};
    for (const auto &[tmpl, instances] : generic_lambda_instances_) {
      const auto found = merged->find(tmpl);
      if (found == merged->end()) {
        continue;
      }
      const auto *target = static_cast<const ast::lambda_expr *>(found->second);
      if (generic_lambda_instances_.contains(target)) {
        continue;
      }
      auto copies = std::vector<const ast::lambda_expr *>{};
      for (const auto *instance : instances) {
        auto cloned = ast::clone_expr(*instance, merged.get());
        if (!cloned.has_value()) {
          continue; // the instance was itself cloned from this node
        }
        const auto *made =
            dynamic_cast<const ast::lambda_expr *>(cloned->get());
        if (const auto use = generic_lambda_uses_.find(instance);
            use != generic_lambda_uses_.end()) {
          generic_lambda_uses_.insert_or_assign(made, use->second);
        }
        copies.push_back(made);
        synthesized_lambdas_.push_back(std::move(*cloned));
      }
      added.emplace_back(target, std::move(copies));
    }
    for (auto &[target, copies] : added) {
      generic_lambda_instances_.emplace(target, std::move(copies));
      grew = true;
      added_any = true;
    }
  }
  if (!added_any) {
    return;
  }
  auto refs = std::vector<std::pair<const void *, const ast::lambda_expr *>>{};
  for (const auto &[use, instance] : generic_lambda_refs_) {
    const auto use_copy = merged->find(use);
    const auto instance_copy = merged->find(instance);
    if (use_copy != merged->end() && instance_copy != merged->end()) {
      refs.emplace_back(use_copy->second, static_cast<const ast::lambda_expr *>(
                                              instance_copy->second));
    }
  }
  generic_lambda_refs_.insert(refs.begin(), refs.end());
  item.clone_pairs = std::move(merged);
}

/// A method call whose receiver is not yet anything a method can be found
/// on — `a.cmp(&b)` for `let a = 5`, before a later `let c: int64 = a`
/// has said which integer `a` is. Defaulting the receiver here would pick
/// `int32` and then report the annotation as the mistake.
///
/// The arguments are checked now, in the scopes they were written in; the
/// call answers with a leaf, and once the receiver settles the call is
/// checked again in full with the receiver and arguments replaying what
/// they were found to be (`replayed_exprs_`). A receiver that never
/// settles is owed its default by the queue's last resort, exactly as
/// `demand` would have spent it, only later.
///
/// The re-run stands in the body the call was written in
/// (`body_context`), so a generic body's type parameters are in scope for
/// it and a temporary receiver (`(a + 0).twice()`) waits like a named one.
/// A lambda argument is not checked now: it is typed by the parameter it
/// meets, which only the settled receiver's method can say, so the re-run
/// checks it for the first time. The one refusal: a lambda argument inside
/// a lambda with an explicit capture list, whose "captured but never used"
/// check runs when that lambda ends and would miss the uses the re-run
/// makes later.
auto checker::defer_open_receiver_call(const ast::call_expr &call,
                                       const ast::field_expr &field)
    -> std::optional<type_id> {
  const auto is_lambda = [](const ast::call_arg &arg) -> bool {
    return arg.value != nullptr &&
           arg.value->kind == ast::node_kind::lambda_expr;
  };
  if (std::ranges::any_of(call.args, is_lambda) &&
      std::ranges::any_of(function_.capture_barriers_,
                          [](const capture_barrier &barrier) -> bool {
                            return barrier.allowed.has_value();
                          })) {
    return std::nullopt;
  }
  for (const auto &arg : call.args) {
    if (arg.value != nullptr && !is_lambda(arg)) {
      infer_expr(*arg.value, k_unknown_type);
    }
  }
  auto replay = std::vector<std::pair<const ast::expr *, type_id>>{};
  const auto recorded = [this](const ast::expr &expr) -> type_id {
    const auto found = session_.node_types_.find(&expr);
    return found != session_.node_types_.end() ? found->second : k_unknown_type;
  };
  replay.emplace_back(field.object.get(), recorded(*field.object));
  for (const auto &arg : call.args) {
    if (arg.value != nullptr && !is_lambda(arg)) {
      replay.emplace_back(arg.value.get(), recorded(*arg.value));
    }
  }
  const auto receiver = strip_refs(settle(replay.front().second));
  const auto result = leaf_ctxt_.fresh_type(
      std::format("the result of this call to `{}`", field.field_name),
      source_location{.file_id = file_.file_id_, .span = call.span});
  // The re-run solves the result leaf and whatever the parameters say
  // about the arguments; a lambda argument is checked afresh and may solve
  // anything it captures.
  auto binds = std::optional<std::vector<type_id>>{};
  if (!std::ranges::any_of(call.args, is_lambda)) {
    binds.emplace(1, result);
    for (const auto &[expr, type] : replay) {
      binds->push_back(type);
    }
  }
  defer_method_call(
      field.field_name, receiver, std::move(binds),
      [this, &call, &field, replay = std::move(replay), result,
       context = capture_body_context()](type_id /*settled*/) mutable -> void {
        swap_body_context(context);
        for (const auto &[expr, type] : replay) {
          replayed_exprs_[expr] = type;
        }
        const auto answer =
            record_expr_type(call, infer_method_call(call, field));
        for (const auto &[expr, type] : replay) {
          replayed_exprs_.erase(expr);
        }
        swap_body_context(context);
        if (!session_.types_.is_unknown(answer) && answer != k_error_type) {
          (void)leaf_engine_.unify(result, answer, infer::k_no_cause);
        }
      });
  return result;
}

/// Registers the resolvers. Lazy because they close over `this`.
auto checker::wire_leaf_queue() -> void {
  if (leaf_queue_wired_) {
    return;
  }
  leaf_queue_wired_ = true;
  leaf_queue_.set_resolver(
      infer::obligation_kind::method_call,
      [this](const infer::obligation &goal) -> infer::obligation_report {
        return resolve_deferred_method_call(
            pending_method_calls_[goal.payload]);
      });
}

/// Runs one deferred call, under the context it was deferred from.
auto checker::resolve_deferred_method_call(pending_method_call &deferred)
    -> infer::obligation_report {
  const auto settled = settle(deferred.receiver_type);
  if (mentions_type_var(settled)) {
    return infer::obligation_report{
        .outcome = infer::obligation_outcome::waiting,
        .detail = std::format("the receiver of {} is still `{}`", deferred.goal,
                              session_.types_.display(settled))};
  }

  const auto saved_file = file_.file_id_;
  const auto *saved_module = file_.module_;
  file_.file_id_ = deferred.file;
  file_.module_ = deferred.module;
  const auto *saved_current_template =
      std::exchange(function_.current_template_, deferred.current_template);
  auto saved_sites =
      std::exchange(session_.instantiation_sites_, std::move(deferred.sites));

  // Moved out first: running the call can defer another, which grows
  // `pending_method_calls_` and would free this closure while it runs.
  // Discharged below, so the obligation never comes back for it.
  const auto finish = std::move(deferred.finish);
  finish(settled);

  session_.instantiation_sites_ = std::move(saved_sites);
  function_.current_template_ = saved_current_template;
  file_.file_id_ = saved_file;
  file_.module_ = saved_module;
  return infer::obligation_report{
      .outcome = infer::obligation_outcome::discharged, .detail = {}};
}

/// Gives the call just registered a result leaf if its callee declares no
/// return type.
auto checker::mint_open_result(const ast::func_decl &decl, source_span span)
    -> void {
  if (decl.return_type != nullptr) {
    return;
  }
  pending_open_param_calls_.back().result_leaf = leaf_ctxt_.fresh_type(
      std::format("the result of this call to `{}`", decl.name),
      source_location{.file_id = file_.file_id_, .span = span});
}

/// The type a call answers with: its result leaf if it has one.
auto checker::open_call_result(const ast::call_expr &call,
                               type_id declared) const -> type_id {
  if (!pending_open_param_calls_.empty() &&
      pending_open_param_calls_.back().call == &call &&
      pending_open_param_calls_.back().result_leaf != k_unknown_type) {
    return pending_open_param_calls_.back().result_leaf;
  }
  return declared;
}

/// Classifies every probed function whose body has been checked.
///
/// This is generalization over the probes as one group. A probe's calls
/// are solved before its own verdict is read, since they are what may
/// pin its parameters or state its result, so its callees go first: `def
/// g(y): add_one(y)` reads `add_one`'s signature, and only then is `g`'s
/// result known to be the type of `y`. Functions calling each other in a
/// cycle are one group, and call each other at their own leaves.
auto checker::classify_param_decls() -> void {
  if (unclassified_param_decls_.empty()) {
    return;
  }
  auto batch = std::move(unclassified_param_decls_);
  unclassified_param_decls_.clear();
  const auto unclassified = [&](const ast::func_decl *decl) -> bool {
    return !open_param_decls_.contains(decl) &&
           std::ranges::any_of(batch, [&](const auto &probe) -> bool {
             return probe.decl == decl;
           });
  };
  const auto waits_on_callee = [&](const ast::func_decl *decl) -> bool {
    return std::ranges::any_of(
        pending_open_param_calls_,
        [&](const pending_open_param_call &call) -> bool {
          return call.probe == decl && call.decl != decl &&
                 unclassified(call.decl);
        });
  };
  auto done = std::vector<bool>(batch.size(), false);
  auto progress = true;
  while (progress) {
    progress = false;
    for (size_t i = 0; i < batch.size(); ++i) {
      if (!done[i] && !waits_on_callee(batch[i].decl)) {
        classify_probe(batch[i], unclassified);
        done[i] = true;
        progress = true;
      }
    }
  }
  for (size_t i = 0; i < batch.size(); ++i) {
    if (!done[i]) {
      classify_probe(batch[i], unclassified);
    }
  }
}

/// Solves the calls `probe`'s body made, then records whether its body
/// left a parameter open and, if so, its signature.
auto checker::classify_probe(
    const unclassified_param_decl &probe,
    const std::function<bool(const ast::func_decl *)> &unclassified) -> void {
  for (auto &call : pending_open_param_calls_) {
    if (call.probe != probe.decl || call.typed) {
      continue;
    }
    if (open_param_decls_.contains(call.decl)) {
      type_open_param_call(call);
    } else if (unclassified(call.decl)) {
      // Within its own group, a function is called at its own leaves.
      type_group_call(call);
    }
  }
  const auto &param_types = param_types_for(*probe.decl, file_.module_);
  const auto open = has_open_param(*probe.decl, param_types);
  open_param_decls_[probe.decl] = open;
  if (!open) {
    return;
  }
  // Read now, before anything is defaulted: the probe's leaves are the
  // implicit type parameters, and nothing may answer them afterwards.
  auto signature = implicit_signature{};
  for (const auto type : param_types) {
    signature.params.push_back(type == k_unknown_type ? k_unknown_type
                                                      : leaf_ctxt_.zonk(type));
  }
  if (const auto found = open_returns_.find(probe.decl);
      found != open_returns_.end()) {
    signature.result = leaf_ctxt_.zonk(found->second);
  }
  implicit_signatures_[probe.decl] = std::move(signature);
  for (auto i = probe.literal_begin;
       i < probe.literal_end && i < pending_leaf_literals_.size(); ++i) {
    pending_leaf_literals_[i].skip = true;
  }
  for (auto i = probe.call_begin;
       i < probe.call_end && i < pending_method_calls_.size(); ++i) {
    pending_method_calls_[i].finish = [](type_id) -> void {};
  }
}

/// A call between functions of one group: its arguments and result are
/// the callee's own leaves, as a recursive call's are.
auto checker::type_group_call(pending_open_param_call &call) -> void {
  const auto &callee_types = param_types_for(*call.decl, call.owner);
  for (size_t i = 0; i < call.decl->params.size() &&
                     i < call.call_params.size() && i < callee_types.size();
       ++i) {
    if (call.decl->params[i].type_annotation == nullptr &&
        callee_types[i] != k_unknown_type) {
      (void)leaf_engine_.unify(callee_types[i], call.call_params[i],
                               infer::k_no_cause);
    }
  }
  if (const auto found = open_returns_.find(call.decl);
      found != open_returns_.end() && call.result_leaf != k_unknown_type) {
    (void)leaf_engine_.unify(found->second, call.result_leaf,
                             infer::k_no_cause);
  }
  call.typed = true;
}

/// Whether any unannotated parameter of `decl` is still an open leaf.
auto checker::has_open_param(const ast::func_decl &decl,
                             const std::vector<type_id> &types) -> bool {
  for (size_t i = 0; i < decl.params.size() && i < types.size(); ++i) {
    if (decl.params[i].type_annotation == nullptr &&
        types[i] != k_unknown_type && mentions_type_var(settle(types[i]))) {
      return true;
    }
  }
  return false;
}

/// Resolves each pending call whose callee's body has been classified.
///
/// Runs before any literal is defaulted, so the call's types are stated
/// before anything guesses at them (`type_open_param_call`); a call to an
/// implicit generic then names its instance once its arguments settle.
/// A call made in a probed body waits for that body's verdict: if the
/// function is an implicit generic, the call is dropped, since each
/// instance's body makes its own.
auto checker::resolve_open_param_calls() -> void {
  auto remaining = std::vector<pending_open_param_call>{};
  auto pending = std::vector<pending_open_param_call>{};
  pending.swap(pending_open_param_calls_);
  for (auto &item : pending) {
    if (item.probe != nullptr) {
      const auto enclosing = open_param_decls_.find(item.probe);
      if (enclosing == open_param_decls_.end()) {
        remaining.push_back(std::move(item));
        continue;
      }
      if (enclosing->second) {
        continue;
      }
    }
    const auto classified = open_param_decls_.find(item.decl);
    if (classified == open_param_decls_.end()) {
      remaining.push_back(std::move(item));
      continue;
    }
    if (!item.typed) {
      type_open_param_call(item);
    }
    if (classified->second) {
      name_open_param_instance(std::move(item));
    } else if (item.block_generic) {
      // An ordinary method after all: its instance is the block's alone.
      name_block_method_instance(item);
    }
  }
  // Anything queued while instantiating (there is nothing today, since the
  // record is only made from a call walk) is kept alongside what waited.
  for (auto &item : pending_open_param_calls_) {
    remaining.push_back(std::move(item));
  }
  pending_open_param_calls_ = std::move(remaining);
}

/// States a call's types from its classified callee's signature.
///
/// A call to a *concrete* callee has its arguments unified with the types
/// the body pinned: `f(3)` where `f`'s body says `int64` makes the `3` an
/// `int64`. A call to an implicit generic is solved against the callee's
/// signature like any generic call. The signature is the probe's: each
/// unannotated parameter's leaf, and the return type the body inferred in
/// terms of them (`x + 1` returns the type of `x`). Its leaves are the
/// function's implicit type parameters, so each call takes a fresh copy of
/// them, exactly as a call to `def add_one[T](x: T) -> T` takes a fresh
/// `T`. That ties the call's result to its arguments before any literal
/// is defaulted: `let r = add_one(5)` then `let c: int64 = r` makes the
/// `5` an `int64`.
///
/// A return type the body could only state in terms of something other
/// than its parameters (a method on one, still waiting) is not part of the
/// signature; that call's result is the instance's, read once the
/// instance's body is checked (`finish_open_results`).
auto checker::type_open_param_call(pending_open_param_call &item) -> void {
  item.typed = true;
  const auto &decl = *item.decl;
  const auto saved_file = std::exchange(file_.file_id_, item.file);
  const auto *saved_module = std::exchange(file_.module_, item.module);
  // A method of a generic block states its signature in the block's
  // parameters (`T`); this call's receiver says what they are.
  const auto block = open_call_block_bindings(item);
  if (!open_param_decls_.at(item.decl)) {
    const auto &callee_types = param_types_for(decl, item.owner);
    for (size_t i = 0; i < decl.params.size() && i < item.call_params.size() &&
                       i < callee_types.size();
         ++i) {
      if (decl.params[i].type_annotation != nullptr ||
          callee_types[i] == k_unknown_type) {
        continue;
      }
      const auto pinned = substitute_solved(settle(callee_types[i]), block);
      if (!leaf_engine_.unify(pinned, item.call_params[i], infer::k_no_cause)
               .has_value()) {
        error_with_help(
            item.call->span,
            std::format("argument for `{}` has type `{}`, but `{}`'s "
                        "body needs `{}`",
                        param_name_of(decl.params[i]),
                        session_.types_.display(settle(item.call_params[i])),
                        decl.name, session_.types_.display(pinned)),
            "wrong type for this parameter",
            std::format("`{}` has no annotation, so its type comes from "
                        "how `{}`'s own body uses it — never from a "
                        "call. Here the body requires `{}`.",
                        param_name_of(decl.params[i]), decl.name,
                        session_.types_.display(pinned)));
      }
    }
    if (item.result_leaf != k_unknown_type) {
      const auto found = inferred_returns_.find(item.decl);
      if (found != inferred_returns_.end() && found->second != k_unknown_type) {
        type_mismatch(item.call->span, item.result_leaf,
                      substitute_solved(found->second, block),
                      "as the result of this call");
      }
    }
    file_.file_id_ = saved_file;
    file_.module_ = saved_module;
    return;
  }
  const auto &signature = implicit_signatures_.at(item.decl);
  const auto where =
      source_location{.file_id = item.file, .span = item.call->span};
  // Read off the snapshot, never re-settled: the probe's leaves are the
  // parameters, whatever a later default made of them. Its leaves are
  // fresh at each call; the block's parameters are the receiver's.
  auto fresh = param_subst{};
  const auto instantiate = [&](type_id type) -> type_id {
    auto vars = std::vector<type_id>{};
    auto seen = std::unordered_set<type_id>{};
    collect_type_vars(type, vars, seen);
    for (const auto var : vars) {
      if (!fresh.contains(var)) {
        fresh.emplace(
            var,
            leaf_ctxt_.fresh_type(
                std::format("a type in this call to `{}`", decl.name), where));
      }
    }
    return substitute_solved(substitute_solved(type, fresh), block);
  };
  for (size_t i = 0; i < decl.params.size() && i < item.call_params.size() &&
                     i < signature.params.size();
       ++i) {
    if (decl.params[i].type_annotation != nullptr ||
        signature.params[i] == k_unknown_type) {
      continue;
    }
    // A failure is the instance's to report, against a concrete type.
    (void)leaf_engine_.unify(instantiate(signature.params[i]),
                             item.call_params[i], infer::k_no_cause);
  }
  auto result_vars = std::vector<type_id>{};
  auto result_seen = std::unordered_set<type_id>{};
  collect_type_vars(signature.result, result_vars, result_seen);
  if (item.result_leaf != k_unknown_type &&
      signature.result != k_unknown_type &&
      std::ranges::all_of(result_vars, [&](type_id var) -> bool {
        return fresh.contains(var);
      })) {
    (void)leaf_engine_.unify(instantiate(signature.result), item.result_leaf,
                             infer::k_no_cause);
  }
  file_.file_id_ = saved_file;
  file_.module_ = saved_module;
}

/// Names the instance a call to an implicit generic asks for, once the
/// call's arguments settle.
auto checker::name_open_param_instance(pending_open_param_call item) -> void {
  const auto &decl = *item.decl;
  auto open = std::vector<type_id>{};
  for (size_t i = 0; i < decl.params.size() && i < item.call_params.size();
       ++i) {
    if (decl.params[i].type_annotation == nullptr &&
        mentions_type_var(settle(item.call_params[i]))) {
      open.push_back(item.call_params[i]);
    }
  }
  if (item.block_generic && mentions_type_var(settle(item.self_type))) {
    open.push_back(item.self_type);
  }
  // In the calling file and module, and in no template's body: the
  // queue may run the naming from anywhere.
  const auto saved_file = std::exchange(file_.file_id_, item.file);
  const auto *saved_module = std::exchange(file_.module_, item.module);
  const auto *saved_template =
      std::exchange(function_.current_template_, nullptr);
  if (open.empty()) {
    instantiate_open_param_call(item, param_types_for(decl, item.owner));
  } else {
    defer_method_call(decl.name, session_.types_.tuple_of(std::move(open)),
                      std::vector<type_id>{},
                      [this, item = std::move(item)](type_id) -> void {
                        instantiate_open_param_call(
                            item, param_types_for(*item.decl, item.owner));
                      });
  }
  function_.current_template_ = saved_template;
  file_.file_id_ = saved_file;
  file_.module_ = saved_module;
}

/// The generic block's parameters, solved from the call's receiver:
/// `T := int32` for `b.tag(3)` on a `box[int32]`. Empty for anything but
/// a method of a generic block.
auto checker::open_call_block_bindings(const pending_open_param_call &item)
    -> param_subst {
  auto bindings = param_subst{};
  if (item.block_generic) {
    const auto self = settle(item.self_type);
    match_params(item.method->impl_target_pattern, self, bindings);
    solve_impl_value_params(*item.method, self, bindings);
  }
  return bindings;
}

/// Names the block's instance for a call to a method of a generic block
/// whose body pinned every unannotated parameter, once the receiver
/// settles.
auto checker::name_block_method_instance(const pending_open_param_call &item)
    -> void {
  const auto saved_file = std::exchange(file_.file_id_, item.file);
  const auto *saved_module = std::exchange(file_.module_, item.module);
  const auto *saved_template =
      std::exchange(function_.current_template_, nullptr);
  const auto self = settle(item.self_type);
  if (mentions_type_var(self)) {
    defer_method_call(item.decl->name, self, std::vector<type_id>{},
                      [this, &call = *item.call, name = item.decl->name,
                       &receiver = *item.receiver](type_id settled) -> void {
                        name_impl_method_instance(call, name, receiver,
                                                  settled);
                      });
  } else {
    name_impl_method_instance(*item.call, item.decl->name, *item.receiver,
                              self);
  }
  function_.current_template_ = saved_template;
  file_.file_id_ = saved_file;
  file_.module_ = saved_module;
}

/// Names the instance of a generic block's method for a settled receiver
/// and points the call at it.
auto checker::name_impl_method_instance(const ast::call_expr &call,
                                        std::string_view method_name,
                                        const ast::expr &receiver,
                                        type_id settled) -> void {
  // The settled receiver's own entry and method, not the open one's:
  // `list[?a]` and `list[int32]` are different entries with different
  // method tables.
  if (mentions_type_param(settled)) {
    defer_impl_method_call(call, settled);
    return;
  }
  const auto &entry = session_.types_.entry(settled);
  const auto *found = resolve_method(settled, method_name).method;
  if (found == nullptr) {
    return;
  }
  auto bindings = param_subst{};
  match_params(found->impl_target_pattern, settled, bindings);
  solve_impl_value_params(*found, settled, bindings);
  auto scoped = found->fixed_type_params;
  scoped.merge(bindings_by_name(bindings));
  auto solution = generic_solution{};
  carry_impl_value_slots(*found, bindings, solution);
  solution.suffix = std::format("${}", mangle_type_for_instance(settled));
  (void)finish_impl_generic_method_call(call, *found, entry.name, receiver,
                                        settled, bindings, scoped, solution);
}

/// Queues a call to a function with an unannotated parameter.
///
/// A call written in the calling template's parameters (`twice(v)` with
/// `v: T`) is typed in the template like any other — its result is stated
/// in terms of `T` — but names no instance there. Each instance of the
/// caller queues it again with its own argument types, which names the
/// callee's instance for them.
///
/// Whether it is so written is asked at the replay, once the template's
/// leaves have settled: in `twice(twice(v))` the outer call's argument is
/// the inner call's result, which is only `T` once the inner call is typed.
auto checker::queue_open_param_call(pending_open_param_call item) -> void {
  if (function_.current_template_ != nullptr) {
    defer_to_instances(*item.call, [this, item](instance_subst &subst) -> void {
      const auto written_in_template_params =
          mentions_template_param(settle(item.self_type)) ||
          std::ranges::any_of(item.call_params, [&](type_id type) -> bool {
            return mentions_template_param(settle(type));
          });
      if (!written_in_template_params) {
        // The template named this call's instance itself.
        return;
      }
      auto replay = item;
      replay.call = clone_of(subst, item.call);
      replay.receiver = clone_of(subst, item.receiver);
      for (auto &type : replay.call_params) {
        if (type != k_unknown_type) {
          type = substitute_type(type, subst);
        }
      }
      if (replay.self_type != k_unknown_type) {
        replay.self_type = substitute_type(replay.self_type, subst);
      }
      // The template already stated the call's types; this names only,
      // and the call's result is the instance's.
      replay.result_leaf = k_unknown_type;
      replay.probe = nullptr;
      replay.typed = true;
      pending_open_param_calls_.push_back(std::move(replay));
    });
  }
  pending_open_param_calls_.push_back(std::move(item));
}

/// Compiles `item.decl` for this call's argument types and points the call
/// at the instance.
auto checker::instantiate_open_param_call(
    const pending_open_param_call &item,
    const std::vector<type_id> &callee_types) -> void {
  const auto &decl = *item.decl;
  auto seeds = std::vector<type_id>(decl.params.size(), k_unknown_type);
  auto suffix = std::string{};
  for (size_t i = 0; i < decl.params.size(); ++i) {
    if (decl.params[i].type_annotation != nullptr || i >= callee_types.size() ||
        callee_types[i] == k_unknown_type) {
      continue;
    }
    // The callee's own leaf says nothing here — that is what made the
    // function generic — so the parameter's type is this call's argument.
    const auto type = settle(item.call_params[i]);
    if (mentions_type_var(type) || mentions_template_param(type)) {
      // Still open: the argument is itself an unsolved leaf, which is
      // either a parameter of an enclosing implicit generic (its own
      // instances re-check this call with a concrete type) or a literal
      // that already reported its own problem — or it is written in the
      // calling template's parameters, which names no instance.
      return;
    }
    seeds[i] = type;
    suffix += "$" + mangle_type_for_instance(type);
  }
  // A method's instance is named for its block's target (`point::tag$int64`,
  // the way `hir` names an impl member) and checked with the block's fixed
  // bindings and `self` in place; a free function's is its bare name.
  // A method of a generic block is named for its receiver as well
  // (`box::tag$box_int32_$int32`) and checked with the block's parameters
  // solved from it: one instance for both halves of the call.
  const auto block = open_call_block_bindings(item);
  const auto self_type = settle(item.self_type);
  auto block_solution = generic_solution{};
  auto scoped = type_scope{};
  if (item.method.has_value()) {
    scoped = item.method->fixed_type_params;
  }
  if (item.block_generic) {
    if (mentions_type_var(self_type) || mentions_template_param(self_type)) {
      return;
    }
    scoped.merge(bindings_by_name(block));
    carry_impl_value_slots(*item.method, block, block_solution);
    block_solution.suffix =
        std::format("${}", mangle_type_for_instance(self_type));
    const auto saved_file = file_.file_id_;
    file_.file_id_ = item.file;
    const auto bounds_hold = check_call_bounds(
        *item.call, decl, item.owner, item.decl_file, block_solution, &scoped);
    file_.file_id_ = saved_file;
    if (!bounds_hold) {
      return;
    }
  }
  const auto *instance =
      item.method.has_value()
          ? find_or_check_generic_instance(
                *item.call, decl, item.owner, item.decl_file, block_solution,
                std::format("{}::{}{}{}", item.target_type_name, decl.name,
                            block_solution.suffix, suffix),
                &scoped, self_type, item.method->block_type_params, &seeds)
          : find_or_check_generic_instance(
                *item.call, decl, item.owner, item.decl_file,
                generic_solution{}, decl.name + suffix,
                /*fixed_type_params=*/nullptr, k_unknown_type,
                /*block_type_params=*/nullptr, &seeds);
  if (instance == nullptr) {
    return;
  }
  const auto *block_params =
      item.method.has_value() ? item.method->block_type_params : nullptr;
  const auto declared = signature_params(decl, item.owner,
                                         /*skip_self=*/false, block_params);
  auto fn_params = std::vector<type_id>{};
  for (size_t i = 0; i < decl.params.size(); ++i) {
    fn_params.push_back(seeds[i] != k_unknown_type
                            ? seeds[i]
                            : substitute_solved(declared[i].type, block));
  }
  if (decl.return_type != nullptr) {
    if (item.call->callee != nullptr) {
      record_expr_type(
          *item.call->callee,
          session_.types_.fn_of(
              std::move(fn_params),
              substitute_solved(
                  signature_return_type(decl, item.owner, block_params),
                  block)));
    }
  } else {
    // The instance's body may not have been checked yet — it is queued —
    // so its result is read once the queue has drained.
    pending_open_results_.push_back(
        pending_open_result{.call = item.call,
                            .instance = instance,
                            .leaf = item.result_leaf,
                            .fn_params = std::move(fn_params),
                            .file = item.file});
  }
  session_.resolved_callees_[item.call] =
      resolved_callee{.decl = instance,
                      .owner_module = item.owner->module_name,
                      .impl_target_type = "",
                      .receiver = item.receiver};
}

/// Hands each instantiated call the return type its instance's body
/// inferred, which is what the call's result leaf was waiting for.
auto checker::finish_open_results(bool last) -> void {
  auto pending = std::vector<pending_open_result>{};
  pending.swap(pending_open_results_);
  const auto saved_file = file_.file_id_;
  for (auto &item : pending) {
    const auto found = inferred_returns_.find(item.instance);
    // An instance made by substitution has no result until its template
    // is checked, which may be in a file still to come.
    if (found == inferred_returns_.end() && !last) {
      pending_open_results_.push_back(std::move(item));
      continue;
    }
    const auto result =
        found != inferred_returns_.end() ? found->second : k_unknown_type;
    file_.file_id_ = item.file;
    if (item.leaf != k_unknown_type && result != k_unknown_type) {
      type_mismatch(item.call->span, item.leaf, result,
                    "as the result of this call");
    }
    if (item.call->callee != nullptr) {
      record_expr_type(
          *item.call->callee,
          session_.types_.fn_of(std::move(item.fn_params), result));
    }
  }
  file_.file_id_ = saved_file;
}

/// One annotated parameter's declared type, resolved as `signature_params`
/// would.
auto checker::signature_param_type(const ast::func_decl &decl,
                                   const module_members *owner, size_t index)
    -> type_id {
  return signature_params(decl, owner, /*skip_self=*/false)[index].type;
}

/// Drains both deferred queues until neither has anything left.
///
/// They feed each other, so neither can be drained once. Wiring a leaf
/// literal requests instances of its own, and checking an instance walks a
/// *cloned* body — whose `[]` is a different AST node from the template's
/// and mints a leaf of its own. Flushing leaves and then instances leaves
/// every instance's leaf queued behind the walk that is already over,
/// which surfaces as "this array literal's element count is not statically
/// known": the conversion was recorded for the template's node and never
/// for the clone's.
auto checker::flush_deferred() -> void {
  classify_param_decls();
  resolve_open_param_calls();
  // Leaf literals first, because a leaf minted in one function can be
  // pinned by a constraint in another in the same file, so they have to be
  // settled before anything reads a type off them.
  // Unconditionally, and before the loop: most leaves belong to no
  // deferred literal at all — an integer literal in an ordinary statement
  // is one — so gating the queue on there being a pending literal leaves
  // every one of them open, to be discovered by lowering as "no concrete
  // checked type is available for this node".
  (void)leaf_queue_.flush();
  flush_leaf_values();
  while (!pending_leaf_literals_.empty() || instances_.has_walks()) {
    flush_leaf_literals();
    flush_pending_instances();
  }
  finish_open_results();
  finish_pending_interp_segments();
  // Deferred method calls last: every leaf that is ever going to be solved
  // has been by now, so a call still waiting here is one nothing answers.
  // A failure is not reported from here — the receiver's own literal has
  // already said "cannot tell what an empty `[]` is a list of", which is
  // the same mistake in the words the user can act on.
  if (leaf_queue_wired_) {
    (void)leaf_queue_.flush();
    // Running a call can request instances and wire literals of its own.
    while (!pending_leaf_literals_.empty() || instances_.has_walks()) {
      flush_leaf_literals();
      flush_pending_instances();
    }
    finish_open_results();
    finish_pending_interp_segments();
  }
  // Last, once every leaf any template left is settled: the instances
  // made by substitution. Their elaborations request instances of their
  // own — walked or substituted — so this drains to a fixpoint.
  while (instances_.has_substitutions() || instances_.has_walks() ||
         !pending_leaf_literals_.empty()) {
    flush_substitutions();
    // A substituted instance's call to an implicit generic is queued by
    // its replay.
    resolve_open_param_calls();
    flush_leaf_literals();
    flush_pending_instances();
    finish_open_results();
    finish_pending_interp_segments();
  }
  finish_param_probes();
}

/// Runs the range check on every integer literal whose type was still open
/// at the literal itself.
///
/// This is the half of literal checking that had to move with the type. A
/// range is a fact about a *type*, so `let x = 300` is a mistake only once
/// something says `int8`, and before the fixpoint has settled nothing has
/// said anything. Checking eagerly against the default is how a literal
/// about to be given a wider type got refused for not fitting one nobody
/// asked for.
auto checker::flush_leaf_values() -> void {
  auto pending = std::vector<pending_leaf_literal_value>{};
  pending.swap(pending_leaf_values_);
  const auto saved_file = file_.file_id_;
  for (const auto &value : pending) {
    file_.file_id_ = value.file;
    check_integer_fit(*value.literal, leaf_ctxt_.zonk(value.leaf),
                      value.negated);
  }
  file_.file_id_ = saved_file;
}

/// Whether `item` is checked by walking its clone rather than substituted
/// from its template. Two kinds are: a function with unannotated
/// parameters (phase 8b), whose template is a probe rather than a check —
/// each instance is where its body is actually checked; and a
/// compile-time-only function, whose instance is the answer to a call
/// being checked right now and is evaluated, never compiled.
auto checker::walks_instance(const pending_instance &item) -> bool {
  if (item.clone_pairs == nullptr || item.open_params ||
      session_.comptime_only_functions_.contains(item.tmpl)) {
    return true;
  }
  const auto open = open_param_decls_.find(item.tmpl);
  return open != open_param_decls_.end() && open->second;
}

/// The checker's half of `instance_queue`.
class checker::instance_maker final : public instance_hooks {
public:
  explicit instance_maker(checker &owner) : checker_(&owner) {}

  auto walks(const pending_instance &item) -> bool override {
    return checker_->walks_instance(item);
  }

  auto answers_are_concrete(const pending_instance &item) -> bool override {
    return checker_->instance_answers_are_concrete(item);
  }

  auto check_clone(const pending_instance &item) -> void override {
    checker_->with_instance_context(item, [&] -> void {
      checker_->check_function(*item.instance, /*at_module_scope=*/false);
    });
  }

  auto check_template(const pending_instance &item) -> void override {
    checker_->with_instance_context(item, [&] -> void {
      checker_->check_function(*item.tmpl, /*at_module_scope=*/false);
    });
  }

  auto substitute(pending_instance &item) -> void override {
    checker_->with_instance_context(
        item, [&] -> void { checker_->substitute_instance(item); });
  }

  auto record(const pending_instance &item) -> void override {
    checker_->record_instance(item);
  }

  auto walked(const pending_instance &item) -> void override {
    // The clone is a distinct `func_decl` from the template it came from,
    // so membership has to be propagated explicitly — see
    // `session_.comptime_only_functions_`'s doc comment; without this,
    // `hir::lower` would try to lower every instantiation of a compile-time-
    // only `static def` (e.g. `is_integer[int32]`) as if it were ordinary
    // runtime code, exactly the failure this set exists to prevent.
    auto &only = checker_->session_.comptime_only_functions_;
    if (only.contains(item.tmpl)) {
      only.insert(item.instance);
    }
  }

private:
  checker *checker_;
};

/// Makes every queued instance whose template is checked, by substitution;
/// one whose template is not waits for it.
auto checker::flush_substitutions() -> void {
  auto maker = instance_maker{*this};
  instances_.flush_substitutions(maker);
}

/// Registers a made instance for lowering, and the template it came from.
auto checker::record_instance(const pending_instance &item) -> void {
  session_.const_generic_instances_.push_back(const_generic_instance{
      .decl = item.instance, .owner_module = item.owner->module_name});
  instance_templates_[item.instance] = item.tmpl;
}

auto checker::instance_answers_are_concrete(const pending_instance &item)
    -> bool {
  const auto concrete = [&](type_id answer) -> bool {
    return !mentions_abstract_type(settle(answer)) &&
           !mentions_template_param(settle(answer));
  };
  const auto all = [&](const auto &slots) -> auto {
    return std::ranges::all_of(
        slots, [&](const auto &slot) -> auto { return concrete(slot.second); });
  };
  return all(item.solution.type_slots) && all(item.solution.const_slots) &&
         (!item.fixed_type_params.has_value() || all(*item.fixed_type_params));
}

/// At the end of the session: instances whose template was never checked
/// in its own right.
auto checker::flush_unchecked_templates() -> void {
  auto maker = instance_maker{*this};
  instances_.flush_unchecked(maker);
}

auto checker::flush_pending_instances() -> void {
  auto maker = instance_maker{*this};
  instances_.flush_walks(maker);
}

/// Makes one instance now, under the context captured at its request.
auto checker::check_instance(pending_instance &item) -> void {
  auto maker = instance_maker{*this};
  instances_.make_now(item, maker);
}

/// Runs `body` under the checker context captured at `item`'s request:
/// its module and file, its parameters' answers, `self`, the enclosing
/// type-parameter scopes, and the instantiation chain its diagnostics are
/// reported under. Never inside a template, whatever the caller was in.
template <typename Body>
auto checker::with_instance_context(pending_instance item, Body body) -> void {
  const auto saved_module = file_.module_;
  const auto saved_file_id = file_.file_id_;
  auto saved_const_slots = std::move(session_.const_param_slots_);
  auto saved_values = std::move(session_.const_param_values_);
  auto saved_type_slots = std::move(session_.type_param_slots_);
  const auto saved_contract = function_.in_contract_;
  const auto saved_postcondition = function_.in_postcondition_;
  const auto saved_self_type = function_.self_type_;
  const auto saved_block_type_params = session_.enclosing_block_type_params_;
  auto saved_type_params = std::move(function_.type_params_);
  auto saved_sites = std::move(session_.instantiation_sites_);
  const auto saved_depth = session_.instantiation_depth_;
  const auto saved_template =
      std::exchange(function_.current_template_, nullptr);

  session_.enclosing_block_type_params_ = item.block_type_params;
  file_.module_ = item.owner;
  if (!session_.types_.is_unknown(item.self_type)) {
    function_.self_type_ = item.self_type;
  }
  if (item.decl_file.has_value()) {
    file_.file_id_ = *item.decl_file;
  }
  // Both kinds of binding go in together: `push_type_params` consults the
  // two maps side by side, so a mixed template's `n` interns as its
  // constant and its `T` as its concrete type in the same scope.
  session_.const_param_slots_ = item.solution.const_slots;
  session_.const_param_values_ = item.solution.values;
  session_.type_param_slots_ = item.solution.type_slots;
  function_.in_contract_ = false;
  function_.in_postcondition_ = false;
  function_.type_params_ = std::move(item.enclosing_type_params);
  if (item.fixed_type_params.has_value()) {
    function_.type_params_.push_back(*item.fixed_type_params);
  }
  session_.instantiation_sites_ = std::move(item.sites);
  session_.instantiation_depth_ = item.depth;

  body();

  function_.current_template_ = saved_template;
  session_.instantiation_depth_ = saved_depth;
  session_.instantiation_sites_ = std::move(saved_sites);
  function_.type_params_ = std::move(saved_type_params);
  function_.self_type_ = saved_self_type;
  session_.enclosing_block_type_params_ = saved_block_type_params;
  function_.in_postcondition_ = saved_postcondition;
  function_.in_contract_ = saved_contract;
  session_.type_param_slots_ = std::move(saved_type_slots);
  session_.const_param_values_ = std::move(saved_values);
  session_.const_param_slots_ = std::move(saved_const_slots);
  file_.file_id_ = saved_file_id;
  file_.module_ = saved_module;
}

} // namespace cinder::semantic::detail
