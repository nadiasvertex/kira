#include "checker.h"

namespace cinder::semantic::detail {

// ==========================================================================
//  Declarations
// ==========================================================================

/// Checks an `intrinsic def` declaration: its name must be one the
/// compiler's backends actually implement (see `src/intrinsics.h` and
/// `spec/stdlib.md`), it must sit at module scope rather than inside a
/// trait/impl, and — since there is no body to infer from — every
/// parameter and the return type must be explicitly annotated.
auto checker::check_intrinsic_decl(const ast::func_decl &decl,
                                   bool at_module_scope) -> void {
  if (!at_module_scope) {
    error_with_help(
        decl.span,
        std::format("`intrinsic def {}` must be a module-level declaration",
                    decl.name),
        "intrinsic declarations cannot be a trait or impl member",
        "Move this declaration to module scope. A trait or impl method "
        "can call a module-level intrinsic from its body instead.");
  }

  if (!is_known_intrinsic(decl.name)) {
    auto candidates = std::vector<std::string>{};
    candidates.reserve(known_intrinsic_names.size());
    for (const auto known : known_intrinsic_names) {
      candidates.emplace_back(known);
    }
    auto diag =
        diagnostic(diagnostic_level::error,
                   std::format("`{}` is not a recognized intrinsic", decl.name),
                   file_.file_id_);
    diag.with_label(decl.span, "no backend implements this name");
    if (const auto suggestion = best_suggestion(decl.name, candidates)) {
      diag.with_help(std::format("did you mean `{}`?", *suggestion));
    } else {
      diag.with_help(
          "see spec/specification/stdlib/ for the modules that declare "
          "recognized intrinsics");
    }
    emit_diag(diag);
    mark_error();
  }

  for (const auto &param : decl.params) {
    if (param.type_annotation == nullptr) {
      error_with_help(
          param.span,
          std::format("intrinsic `{}` must annotate every parameter",
                      decl.name),
          "missing type annotation",
          "An intrinsic has no body for the compiler to infer types "
          "from — every parameter needs an explicit type.");
    }
  }
  if (decl.return_type == nullptr) {
    error_with_help(
        decl.span,
        std::format("intrinsic `{}` must annotate its return type", decl.name),
        "missing return type",
        "An intrinsic has no body for the compiler to infer a return "
        "type from — add `-> Type`.");
  }
}

/// True when a block's final statement guarantees the enclosing
/// function's result is produced on every control-flow path through it —
/// by `return`ing, or by every branch of a tail `if`/`match` doing so.
///
/// Deliberately permissive: any statement kind this doesn't recognize
/// counts as providing a value, because nested branch/arm tails are
/// already join-checked against the expected type (`join_branch_type`,
/// `check_match`) and diagnosed there. This predicate exists to close
/// the holes typing alone can't see: a tail `if` with no `else` (its
/// statement typing seeds the branch join with `expected_tail`, so the
/// missing fall-through path never surfaces as a mismatch), and the same
/// shape nested inside branch or arm tails.
auto checker::block_provides_function_value(
    const std::vector<ast::ptr<ast::node>> &stmts) -> bool {
  for (const auto &item : stmts | std::views::reverse) {
    if (item != nullptr) {
      return node_provides_function_value(*item);
    }
  }
  // An empty (or fully recovered-away) block: stay permissive — the
  // parser has already complained about whatever went missing.
  return true;
}

/// True for a tail `while true:` — the one unit-typed statement control
/// never falls past, since Cinder has no `break`: the loop can only be
/// left by `return`ing out of the enclosing function.
auto checker::is_while_true(const ast::node &node) -> bool {
  const auto *stmt = dynamic_cast<const ast::while_stmt *>(&node);
  if (stmt == nullptr) {
    return false;
  }
  const auto *condition =
      dynamic_cast<const ast::literal_expr *>(stmt->condition.get());
  return condition != nullptr && condition->lit_kind == token_kind::kw_true;
}

/// See `block_provides_function_value` — the per-statement half of the
/// recursion.
auto checker::node_provides_function_value(const ast::node &node) -> bool {
  switch (node.kind) {
  case ast::node_kind::return_stmt:
    return true;
  case ast::node_kind::if_stmt: {
    const auto &stmt = dynamic_cast<const ast::if_stmt &>(node);
    return !stmt.else_body.empty() &&
           std::ranges::all_of(stmt.branches,
                               [](const ast::if_branch &branch) -> bool {
                                 return block_provides_function_value(
                                     branch.body);
                               }) &&
           block_provides_function_value(stmt.else_body);
  }
  case ast::node_kind::match_stmt: {
    const auto &stmt = dynamic_cast<const ast::match_stmt &>(node);
    // Compact `=> expr` arms always produce a value; recovered arms have
    // already been diagnosed.
    return std::ranges::all_of(
        stmt.arms, [](const ast::match_arm &arm) -> bool {
          return arm.body_expr != nullptr || arm.has_error ||
                 block_provides_function_value(arm.body_stmts);
        });
  }
  case ast::node_kind::static_decl: {
    const auto &stmt = dynamic_cast<const ast::static_decl &>(node);
    if (stmt.decl_kind != ast::static_decl_kind::conditional_compilation) {
      return true;
    }
    return !stmt.else_body.empty() &&
           block_provides_function_value(stmt.if_body) &&
           block_provides_function_value(stmt.else_body);
  }
  default:
    return true;
  }
}

/// Checks a function/method declaration end to end: enforces the
/// `pub`-must-annotate rule (`at_module_scope`), binds type parameters
/// and value parameters (including a leading `self` typed from
/// `function_.self_type_`) into a fresh scope stack, checks contract conditions
/// under purity enforcement, and checks the body against the declared or
/// inferred return type. Saves and restores all per-function state
/// (scopes, return type, undefined-name dedup) so nested/sibling checks
/// don't see stale state.
auto checker::check_function(const ast::func_decl &decl, bool at_module_scope)
    -> void {
  if (early_checked_.contains(&decl) &&
      return_inference_started_.contains(&decl) &&
      inferred_returns_.contains(&decl)) {
    return;
  }
  // A function with an unannotated parameter is checked once against that
  // parameter's leaf to find out whether the body pins it. The answer is
  // only known afterwards, and what the body says about a still-open leaf
  // is not evidence of a mistake — `bits(x)` on an open `x` is exactly a
  // call waiting for its instance — so the diagnostics of that first check
  // are held. If the body pinned every parameter the function is ordinary
  // and they are emitted (`finish_param_probes`); if one stayed open, each
  // instance is checked against a concrete type and reports for real.
  if (decl.has_error || probe_capture_ != nullptr ||
      decl.modifiers.is_intrinsic ||
      cache_.inferred_param_types_.contains(&decl) ||
      !has_unannotated_params(decl)) {
    check_function_impl(decl, at_module_scope);
    return;
  }
  auto held = std::vector<diagnostic>{};
  auto mark =
      unclassified_param_decl{.decl = &decl,
                              .literal_begin = pending_leaf_literals_.size(),
                              .call_begin = pending_method_calls_.size()};
  probe_capture_ = &held;
  probing_decl_ = &decl;
  check_function_impl(decl, at_module_scope);
  probing_decl_ = nullptr;
  probe_capture_ = nullptr;
  mark.literal_end = pending_leaf_literals_.size();
  mark.call_end = pending_method_calls_.size();
  unclassified_param_decls_.push_back(mark);
  if (!held.empty()) {
    param_probes_.push_back(param_probe{
        .decl = &decl, .diags = std::move(held), .file = file_.file_id_});
  }
}

/// Emits the held diagnostics of every probed function whose parameters
/// the body fully determined, and drops the rest.
auto checker::finish_param_probes() -> void {
  auto pending = std::vector<param_probe>{};
  pending.swap(param_probes_);
  const auto saved_file = file_.file_id_;
  for (auto &probe : pending) {
    const auto found = open_param_decls_.find(probe.decl);
    if (found != open_param_decls_.end() && found->second) {
      continue;
    }
    file_.file_id_ = probe.file;
    for (const auto &diag : probe.diags) {
      emit_diag(diag);
      mark_error();
    }
  }
  file_.file_id_ = saved_file;
}

auto checker::check_function_impl(const ast::func_decl &decl,
                                  bool at_module_scope) -> void {
  if (decl.has_error) {
    return;
  }

  if (at_module_scope && decl.visibility == ast::visibility::pub) {
    auto unannotated = false;
    for (size_t i = 0; i < decl.params.size(); ++i) {
      if (decl.params[i].type_annotation == nullptr &&
          !(i == 0 && param_name_of(decl.params[i]) == "self")) {
        unannotated = true;
      }
    }
    if (unannotated || decl.return_type == nullptr) {
      error_with_help(
          decl.span,
          std::format("public function `{}` must annotate its parameters "
                      "and return type",
                      decl.name),
          "exported signature is incomplete",
          "An exported function is a contract other code depends on; "
          "inference is for code inside your module.");
    }
  }

  if (decl.modifiers.is_intrinsic) {
    check_intrinsic_decl(decl, at_module_scope);
  }

  // Everything a body can change is per-function state. It is moved out
  // here and moved back on exit, so no field can be missed. What the caller
  // set up for this body (its `Self`, its type parameters, and the contract
  // and reachability flags) carries over into the fresh state.
  auto outer = std::move(function_);
  function_ = function_state{
      .type_params_ = outer.type_params_,
      .self_type_ = outer.self_type_,
      .self_assoc_types_ = outer.self_assoc_types_,
      .in_contract_ = outer.in_contract_,
      .in_postcondition_ = outer.in_postcondition_,
      .in_speculative_static_branch_ = outer.in_speculative_static_branch_,
      .in_known_unreachable_code_ = outer.in_known_unreachable_code_,
      .current_template_ = outer.current_template_,
  };
  push_type_params(decl.type_params);
  record_declared_facts(decl.type_params, &decl.where_constraints);
  // Every call inside a comptime-only `static def`'s own body (template or
  // instantiated clone alike — see `session_.comptime_only_functions_`'s doc
  // comment) is already reachable through direct evaluator execution
  // (`comptime::evaluator::eval_call`/`call_function`) whenever this
  // function is actually invoked at compile time; it is never lowered
  // itself. Constant-folding a nested call here too would be redundant at
  // best, and at worst would hand `comptime_eval_.evaluate` an argument
  // expression that only makes sense against this function's own abstract
  // template parameters or runtime locals — not something compile-time
  // evaluable on its own — and spuriously report an error. See
  // `try_fold_comptime_only_call`.
  function_.in_comptime_only_function_ =
      session_.comptime_only_functions_.contains(&decl);
  function_.in_machine_function_ = decl.modifiers.is_machine;
  // A parameter of this function's own with no answer in
  // `session_.const_param_slots_`/`session_.type_param_slots_` is still a
  // symbol here, so this check is its template's: an error in it means no
  // instance of it is ever made (`failed_templates_`).
  const auto errors_before_body = errors_emitted_;
  const auto checking_template =
      std::ranges::any_of(decl.type_params, [this](const auto &param) -> bool {
        return !param.name.empty() &&
               !(param.is_value_param ? session_.const_param_slots_
                                      : session_.type_param_slots_)
                    .contains(param.name);
      });
  // A body with any parameter still abstract is a template: its records
  // are written in those parameters, and every decision that needs them
  // concrete is deferred to its instances (`defer_to_instances`).
  const auto is_template = in_abstract_type_param_scope();
  if (is_template) {
    function_.current_template_ = &decl;
    template_elaborations_[&decl].clear();
    auto &abstract = template_abstract_params_[&decl];
    abstract.clear();
    for (const auto &scope : function_.type_params_) {
      for (const auto &[name, id] : scope) {
        if (session_.types_.entry(id).kind == type_kind::type_param_kind) {
          abstract.insert_or_assign(name, id);
        }
      }
    }
  }
  // Bind each of this instantiation's own type parameters into the
  // compile-time evaluator's locals as a `type_value` (`comptime::value`),
  // so a `static if`/`static assert` condition *inside this function's own
  // body* can reflect on its own type parameter (e.g. `T.name()`,
  // `is_integer[T]()`) the same way ordinary body statements already can
  // via `session_.type_param_reflections_` (`type_param_comptime_values`,
  // above). Pushed/popped unconditionally (empty when this is the template pass
  // or none of the type params are types) to keep the evaluator's locals stack
  // symmetric with `function_.scopes_`/`function_.capture_barriers_` above.
  auto type_param_values =
      type_param_comptime_values(decl.type_params, session_.type_param_slots_);
  comptime_eval_.push_locals(
      {type_param_values.begin(), type_param_values.end()});
  // Barriers index into `function_.scopes_`, so they have to travel with it: a
  // stale `scope_floor` left behind here would refuse names in the fresh stack.
  push_scope();
  // A function proves things only from what *its own* signature and body
  // establish; a sibling's facts are not in scope here any more than its
  // locals are.

  bind_value_params(decl.type_params);
  if (session_.enclosing_block_type_params_ != nullptr) {
    bind_value_params(*session_.enclosing_block_type_params_);
  }

  const auto &inferred_types = param_types_for(decl, file_.module_);
  for (size_t i = 0; i < decl.params.size(); ++i) {
    const auto &param = decl.params[i];
    auto type = k_unknown_type;
    if (param.type_annotation != nullptr) {
      // `current_resolve_ctx()` leaves `existential_allowed` at its
      // default `false`, so a `some Trait[Args]` annotation here is
      // rejected by `resolve_existential_type` itself with a clear
      // "not allowed here" diagnostic — parameters get no dedicated
      // message of their own.
      type = resolve_type(*param.type_annotation, current_resolve_ctx());
    } else if (i == 0 && param_name_of(param) == "self") {
      type = self_param_type(param);
    } else if (i < inferred_types.size()) {
      type = inferred_types[i];
    }
    if (param.pattern != nullptr) {
      // Parameters aren't expressions, so `infer_expr`'s chokepoint never
      // sees them; record the resolved type here so a later lowering pass
      // (spec/typed-ir-design.md) can read a parameter's declared type
      // without re-running `resolve_type`'s module-context machinery.
      record_expr_type(*param.pattern, type);
      if (param.pattern->kind == ast::node_kind::binding_pattern) {
        const auto &binding =
            dynamic_cast<const ast::binding_pattern &>(*param.pattern);
        bind_value(binding.name, type, binding_origin::parameter, param.span);
      } else {
        check_pattern(*param.pattern, strip_refs(type));
      }
    }
    if (param.default_value != nullptr) {
      const auto found = infer_expr(*param.default_value, type);
      type_mismatch(param.default_value->span, type, found,
                    "for this default value");
    }
  }

  function_.inferred_return_seen_ = false;
  function_.return_annotated_ = decl.return_type != nullptr;
  // A template infers its result too, written in its own parameters
  // (`def identity[T](x: T):` returns a `T`): an instance's result is that
  // type under the instance's substitution (phase 9.7).
  function_.inferring_return_ = !function_.return_annotated_ &&
                                !decl.modifiers.is_generator &&
                                !decl.modifiers.is_intrinsic;
  function_.inferred_return_ = k_unknown_type;
  auto return_ctx = current_resolve_ctx();
  return_ctx.existential_allowed = true;
  return_ctx.is_generator_return = decl.modifiers.is_generator;
  function_.return_type_ = function_.return_annotated_
                               ? resolve_type(*decl.return_type, return_ctx)
                               : k_unknown_type;
  if (decl.return_type != nullptr) {
    // Same rationale as the parameter loop above: a declared return type
    // is resolved from a `type_expr`, not an expression, so it needs its
    // own persistence hook for lowering to read.
    record_expr_type(*decl.return_type, function_.return_type_);
  }

  function_.in_generator_ = decl.modifiers.is_generator;
  function_.generator_item_type_ = k_unknown_type;
  if (function_.in_generator_) {
    // `return_ctx.is_generator_return` (set above) already made
    // `resolve_type` resolve `some iterator[T]` straight to the concrete
    // `generator[T]` — see `resolve_existential_type`'s doc comment —
    // so `function_.return_type_` here is never the general opaque wrapper.
    const auto &return_entry = session_.types_.entry(function_.return_type_);
    const auto is_generator_type =
        function_.return_annotated_ &&
        return_entry.kind == type_kind::builtin_generic_kind &&
        return_entry.name == "generator" && !return_entry.args.empty();
    if (is_generator_type) {
      function_.generator_item_type_ = return_entry.args.front();
    } else {
      error_with_help(
          decl.span,
          std::format("generator `{}` must declare a `some iterator[T]` "
                      "return type",
                      decl.name),
          "missing or invalid generator return type",
          "Write `-> some iterator[T]`, naming the type of value each "
          "`yield` produces.");
    }
    if (decl.body_expr != nullptr) {
      error_with_help(
          decl.span,
          std::format("generator `{}` cannot use a compact expression body",
                      decl.name),
          "expression-bodied generators aren't supported",
          "Give the generator an indented block body and `yield` values "
          "from inside it.");
    }
  }

  // A general `some Trait[Args]` existential return type (not the
  // generator sugar above): the body's concrete return points are
  // inferred and joined via `join_branch_type` into
  // `function_.existential_underlying_` rather than checked against
  // `function_.return_type_` directly, since `function_.return_type_` is the
  // fresh opaque id itself — no concrete expression could ever match it.
  function_.checking_existential_return_ = false;
  function_.existential_underlying_ = k_unknown_type;
  const auto existential_return_id = function_.return_type_;
  if (!function_.in_generator_ && function_.return_annotated_ &&
      session_.types_.entry(function_.return_type_).kind ==
          type_kind::existential_kind) {
    function_.checking_existential_return_ = true;
  }

  for (const auto &constraint : decl.where_constraints) {
    if (constraint.subject != nullptr) {
      resolve_type(*constraint.subject, current_resolve_ctx());
    }
    if (constraint.bound_or_type != nullptr) {
      resolve_type(*constraint.bound_or_type, current_resolve_ctx());
    }
  }

  function_.in_contract_ = true;
  for (const auto &contract : decl.contracts) {
    // A generator has no single exit a `post` could describe. Its body runs
    // in steps and hands values out through `yield`; `return` there means
    // "exhausted", not "here is the result", and the value the *call*
    // produces is the generator object itself, which the body never names.
    // A `pre` still means exactly what it always meant — a condition on the
    // arguments, checked once before the body starts — so only the `post`
    // face is refused, and it is refused here, where the declaration is,
    // rather than later where only the lowering is visible.
    if (function_.in_generator_ && !contract.is_pre) {
      error_with_help(
          contract.span,
          std::format("a `post` condition cannot be written on the "
                      "`generator def` `{}`",
                      decl.name),
          "a generator has no single result to describe",
          "A generator's body runs in steps and produces its values with "
          "`yield`; `return` inside it means the generator is exhausted, "
          "not that it is handing back a result. Constrain the arguments "
          "with a `pre` condition, or check each produced value in the "
          "body before yielding it.");
      continue;
    }
    if (contract.condition != nullptr && !contract.condition->has_error) {
      function_.in_postcondition_ = !contract.is_pre;
      require_bool(*contract.condition, "a contract condition");
    }
  }
  function_.in_postcondition_ = false;
  function_.in_contract_ = false;

  // Only now, with every parameter bound and every contract checked, is
  // there anything to assume — `assume_binding` reads the parameters' types
  // back out of the scope the loop above filled in.
  collect_function_facts(decl);

  if (function_.in_generator_) {
    // A generator body's tail statement isn't a return value the way an
    // ordinary function's is — its only output channel is `yield`
    // (checked in place via `infer_yield`, which reads
    // `function_.generator_item_type_` directly) — so there's nothing to
    // compare against `function_.return_type_` here.
    if (decl.body_expr == nullptr) {
      check_body_nodes(decl.body_stmts, k_unknown_type);
    }
  } else if (decl.body_expr != nullptr) {
    const auto expected_for_body =
        function_.checking_existential_return_
            ? k_unknown_type
            : (function_.return_annotated_ ? function_.return_type_
                                           : k_unknown_type);
    const auto found = infer_expr(*decl.body_expr, expected_for_body);
    if (function_.checking_existential_return_) {
      function_.existential_underlying_ =
          join_branch_type(function_.existential_underlying_, found,
                           decl.body_expr->span, "existential return");
    } else if (function_.return_annotated_) {
      type_mismatch(decl.body_expr->span, function_.return_type_, found,
                    "as the function result", decl.body_expr.get());
    } else if (function_.inferring_return_) {
      note_inferred_return(found, decl.body_expr->span);
    }
  } else if (!decl.body_stmts.empty()) {
    const auto expected_for_body =
        function_.checking_existential_return_
            ? k_unknown_type
            : (function_.return_annotated_ ? function_.return_type_
                                           : k_unknown_type);
    const auto tail = check_body_nodes(decl.body_stmts, expected_for_body);
    if (function_.inferring_return_ && !session_.types_.is_unit(tail)) {
      note_inferred_return(tail, decl.body_stmts.back() != nullptr
                                     ? decl.body_stmts.back()->span
                                     : decl.span);
    }
    if (function_.checking_existential_return_) {
      if (!session_.types_.is_unit(tail) && !session_.types_.is_unknown(tail)) {
        function_.existential_underlying_ = join_branch_type(
            function_.existential_underlying_, tail,
            decl.body_stmts.back() != nullptr ? decl.body_stmts.back()->span
                                              : decl.span,
            "existential return");
      }
    } else if (function_.return_annotated_ &&
               !session_.types_.is_unit(function_.return_type_) &&
               !session_.types_.is_unknown(function_.return_type_) &&
               !session_.types_.is_unknown(tail) && tail != k_error_type) {
      const auto tail_span = decl.body_stmts.back() != nullptr
                                 ? decl.body_stmts.back()->span
                                 : decl.span;
      if (!session_.types_.is_unit(tail) &&
          !agrees(function_.return_type_, tail)) {
        error(tail_span,
              std::format("function `{}` returns `{}`, but its final "
                          "expression has type `{}`",
                          decl.name,
                          session_.types_.display(function_.return_type_),
                          session_.types_.display(tail)),
              "mismatched final expression");
      } else if (session_.types_.is_unit(tail)
                     ? (decl.body_stmts.back() == nullptr ||
                        !is_while_true(*decl.body_stmts.back()))
                     : !block_provides_function_value(decl.body_stmts)) {
        // The body type-checked, but control can still fall off the end
        // without a value: either the tail statement produces `unit`
        // outright (e.g. the body ends in a `let` or a non-`while true`
        // loop), or a tail `if`/`match` leaves some path — typically a
        // missing `else` — that returns nothing.
        auto diag = diagnostic(
            diagnostic_level::error,
            std::format("function `{}` is declared to return `{}`, but "
                        "not every path through its body returns a value",
                        decl.name,
                        session_.types_.display(function_.return_type_)),
            file_.file_id_);
        diag.with_label(tail_span,
                        "control can reach the end of the function from "
                        "here without a value");
        if (decl.return_type != nullptr) {
          diag.with_label(
              decl.return_type->span,
              std::format("`{}` promised here",
                          session_.types_.display(function_.return_type_)));
        }
        diag.with_help(std::format(
            "Make every path produce a `{}`: add a `return`, cover the "
            "missing `else` or `match` arm, or make the final statement "
            "an expression of that type. If the function has nothing to "
            "return, change the return type to `unit`.",
            session_.types_.display(function_.return_type_)));
        emit_diag(diag);
        mark_error();
      }
    }
  }

  if (function_.checking_existential_return_) {
    if (session_.types_.is_unknown(function_.existential_underlying_)) {
      error_with_help(
          decl.span,
          std::format("could not infer a concrete return type for `{}`",
                      decl.name),
          "existential return type has no concrete value",
          "An existential return type still needs a real value flowing "
          "out of the function on every path — return or produce a "
          "concrete value, not just `panic`/an infinite loop.");
    } else {
      auto &existential_entry =
          session_.types_.mutable_entry(existential_return_id);
      if (session_.types_.is_unknown(existential_entry.result)) {
        existential_entry.result = function_.existential_underlying_;
        const auto &underlying_entry =
            session_.types_.entry(function_.existential_underlying_);
        for (const auto &required : existential_entry.existential_bound) {
          if (!type_has_trait(underlying_entry, required.trait_name)) {
            error_with_help(
                decl.return_type->span,
                std::format(
                    "`{}` does not implement `{}`",
                    session_.types_.display(function_.existential_underlying_),
                    required.trait_name),
                "existential bound not satisfied",
                std::format("Add `impl {} for {}: ...` providing the trait's "
                            "methods, or return a type that already implements "
                            "it.",
                            required.trait_name,
                            session_.types_.display(
                                function_.existential_underlying_)));
          }
        }
      }
    }
  }

  if (function_.inferring_return_) {
    // No value on any path means the function returns `unit`.
    // A literal's leaf has to be settled here: the body compiles once, so
    // nothing later can still choose its width.
    if (const auto own = recursive_returns_.find(&decl);
        own != recursive_returns_.end() && function_.inferred_return_seen_) {
      solve_leaves(own->second, function_.inferred_return_);
    }
    auto result = session_.types_.builtin("unit");
    if (function_.inferred_return_seen_) {
      result = settle(function_.inferred_return_);
      // A result that still hangs on one of the function's own parameters
      // (`x + x` over an open `x`) is not defaulted: the parameter is what
      // makes the function generic, and each instance answers for itself.
      // Anything else still open is a literal, which nothing later can pin.
      auto vars = std::vector<type_id>{};
      auto seen = std::unordered_set<type_id>{};
      collect_type_vars(result, vars, seen);
      const auto own = cache_.inferred_param_types_.find(&decl);
      const auto hangs_on_param =
          own != cache_.inferred_param_types_.end() &&
          std::ranges::any_of(own->second, [&](type_id param) -> bool {
            auto param_vars = std::vector<type_id>{};
            auto param_seen = std::unordered_set<type_id>{};
            collect_type_vars(settle(param), param_vars, param_seen);
            return std::ranges::any_of(param_vars, [&](type_id v) -> bool {
              return std::ranges::find(vars, v) != vars.end();
            });
          });
      if (!vars.empty() && !hangs_on_param) {
        result = demand(result);
      }
    }
    // An answer still open (`return forever()`) is no answer: the function
    // keeps no inferred return type, and lowering says so. One open on the
    // parameters is the implicit generic's result type, kept for its
    // signature (`classify_param_decls`).
    inferred_returns_[&decl] =
        mentions_type_var(result) ? k_unknown_type : result;
    if (mentions_type_var(result)) {
      open_returns_[&decl] = result;
    }
  }
  if (checking_template && errors_emitted_ != errors_before_body) {
    failed_templates_.insert(&decl);
  }
  // Every checked body, not only templates: an instance of a method of
  // `impl ... for boxed[int32]` is named per receiver but has nothing to
  // substitute, so its records are the body's own, copied.
  checked_templates_.insert(&decl);
  if (const auto waiting = waiting_instances_.find(&decl);
      waiting != waiting_instances_.end()) {
    for (auto &item : waiting->second) {
      substitution_queue_.push_back(std::move(item));
    }
    waiting_instances_.erase(waiting);
  }
  pop_type_params();
  function_ = std::move(outer);
  comptime_eval_.pop_locals();

  // A `def` nested inside another function's body is invisible to
  // `file_.module_->functions` (only module-scope items are indexed there), so
  // without this it can never be found by name — neither by sibling
  // statements after it nor, via the borrow-checker's own walk, anywhere
  // at all. Bind it into the enclosing scope, the same way `let`/`var`
  // introduce a name, so `lookup_value` finds it exactly like any other
  // local.
  if (!at_module_scope && !decl.name.empty()) {
    bind_value(decl.name, fn_type_of(decl, file_.module_),
               binding_origin::let_binding, decl.span);
  }
}

/// Checks a `type` declaration: validates its `deriving` clause against
/// the derivable-trait allowlist, checks for duplicate struct fields or
/// sum variants, resolves every field/variant-payload/alias type, and (for
/// an `invariant` clause) checks it as a `bool` contract with `self`
/// bound to the type being declared.
auto checker::check_type_decl(const ast::type_decl &decl) -> void {
  push_type_params(decl.type_params);

  for (const auto &derived : decl.deriving) {
    if (derived != "eq" && derived != "ord" && derived != "show" &&
        derived != "hash" && derived != "debug") {
      error_with_help(
          decl.span,
          std::format("cannot derive `{}` for `{}`", derived, decl.name),
          "not a derivable trait",
          "The derivable traits are `eq`, `ord`, `show`, `hash`, and "
          "`debug`; implement other traits with an explicit `impl` "
          "block.");
    }
  }

  if (decl.modifiers.is_packed && decl.definition != nullptr &&
      decl.definition->kind != ast::node_kind::struct_type_def) {
    const auto *kind_name = [&]() -> const char * {
      switch (decl.definition->kind) {
      case ast::node_kind::sum_type_def:
        return "a sum type";
      case ast::node_kind::refinement_type:
        return "a refinement type";
      default:
        return "a type alias";
      }
    }();
    error_with_help(
        decl.span,
        std::format("`packed` only applies to struct types; `{}` is {}",
                    decl.name, kind_name),
        "has no field layout to pack",
        "Remove `packed` here — only a struct-shaped `type` declaration "
        "has fields whose byte layout `packed` controls.");
  }

  if (decl.definition != nullptr) {
    switch (decl.definition->kind) {
    case ast::node_kind::struct_type_def: {
      const auto &body =
          dynamic_cast<const ast::struct_type_def &>(*decl.definition).body;
      auto seen = std::unordered_set<std::string>{};
      for (const auto &field : body.fields) {
        if (!field.name.empty() && !seen.insert(field.name).second) {
          error(field.span,
                std::format("duplicate field `{}` in struct `{}`", field.name,
                            decl.name),
                "field declared more than once");
        }
        if (field.type != nullptr) {
          resolve_type(*field.type, current_resolve_ctx());
        }
      }
      break;
    }
    case ast::node_kind::sum_type_def: {
      const auto &body =
          dynamic_cast<const ast::sum_type_def &>(*decl.definition).body;
      auto seen = std::unordered_set<std::string>{};
      for (const auto &variant : body.variants) {
        if (!variant.name.empty() && !seen.insert(variant.name).second) {
          error(variant.span,
                std::format("duplicate variant `@{}` in sum type `{}`",
                            variant.name, decl.name),
                "variant declared more than once");
        }
        for (const auto &payload : variant.payload_types) {
          if (payload != nullptr) {
            resolve_type(*payload, current_resolve_ctx());
          }
        }
      }
      break;
    }
    case ast::node_kind::refinement_type: {
      const auto &refinement =
          dynamic_cast<const ast::refinement_type &>(*decl.definition);
      const auto base =
          refinement.base != nullptr
              ? resolve_type(*refinement.base, current_resolve_ctx())
              : k_unknown_type;
      if (refinement.predicate != nullptr && !refinement.predicate->has_error) {
        if (const auto *predicate =
                dynamic_cast<const ast::expr *>(refinement.predicate.get())) {
          push_scope();
          const auto saved_self = function_.self_type_;
          function_.self_type_ = base;
          bind_value("self", base, binding_origin::parameter, decl.span);
          // The declaration's value parameters are ordinary values inside
          // its predicate — `index[n]`'s `self < n` reads `n` as a `usize`,
          // not as a type. Same rule as a `def`'s value parameters inside
          // its body (`check_function`).
          bind_value_params(decl.type_params);
          // A refinement predicate is a contract on a value, and lives
          // under the same purity rule as `pre`/`post`: it must be a
          // question about the value, not an action taken on the way past.
          function_.in_contract_ = true;
          require_bool(*predicate, "a refinement predicate");
          function_.in_contract_ = false;
          function_.self_type_ = saved_self;
          pop_scope();
        }
      }
      break;
    }
    default:
      if (const auto *type =
              dynamic_cast<const ast::type_expr *>(decl.definition.get())) {
        resolve_type(*type, current_resolve_ctx());
      }
      break;
    }
  }

  if (decl.invariant != nullptr && !decl.invariant->has_error) {
    push_scope();
    const auto saved_self = function_.self_type_;
    function_.self_type_ = make_user_type(decl, file_.module_name_, {});
    bind_value("self", function_.self_type_, binding_origin::parameter,
               decl.span);
    function_.in_contract_ = true;
    require_bool(*decl.invariant, "a type invariant");
    function_.in_contract_ = false;
    function_.self_type_ = saved_self;
    pop_scope();
  }

  pop_type_params();
}

/// Checks an `impl` block: resolves its target type and (if present) its
/// trait, validating that a name written before `for` from a type
/// declaration rather than a trait is flagged; when the trait is known,
/// checks member completeness/extras via `check_impl_members`; then checks
/// every member declaration (methods, associated types) with
/// `function_.self_type_` bound to the impl's target.
auto checker::check_impl_decl(const ast::impl_decl &decl) -> void {
  push_type_params(decl.type_params);
  record_declared_facts(decl.type_params, &decl.where_constraints);

  // The trait resolves first: its parameter's *kind* directs how the
  // `for` target is read — `impl monad for option` names the constructor
  // `option`, while `impl show for option` would name an instantiation.
  const ast::trait_decl *trait_decl = nullptr;
  const module_members *trait_owner = nullptr;
  auto trait_name = std::string{};
  if (decl.trait_type != nullptr) {
    resolve_type(*decl.trait_type, current_resolve_ctx());
    if (decl.trait_type->kind == ast::node_kind::named_type) {
      const auto &named =
          dynamic_cast<const ast::named_type &>(*decl.trait_type);
      if (!named.path.empty()) {
        trait_name = named.path.back();
      }
    }
    if (!trait_name.empty()) {
      if (const auto found = find_trait_anywhere(trait_name)) {
        trait_decl = found->first;
        trait_owner = found->second;
      } else if (file_.module_ != nullptr &&
                 file_.module_->types.contains(trait_name)) {
        error_with_help(
            decl.trait_type->span,
            std::format("`{}` is a type, not a trait", trait_name),
            "impl requires a trait before `for`",
            std::format("Write `extend {}:` to add inherent methods to a "
                        "type instead.",
                        trait_name));
      }
    }
  }

  auto target_ctx = current_resolve_ctx();
  const auto *hk_param =
      trait_decl != nullptr && !trait_decl->type_params.empty() &&
              trait_decl->type_params.front().higher_kinded_arity > 0
          ? &trait_decl->type_params.front()
          : nullptr;
  if (hk_param != nullptr) {
    target_ctx.expected_ctor_arity = hk_param->higher_kinded_arity;
  }
  const auto target = decl.for_type != nullptr
                          ? strip_refs(resolve_type(*decl.for_type, target_ctx))
                          : k_unknown_type;
  const auto &target_entry = session_.types_.entry(target);

  // A higher-kinded trait is only implementable *for a constructor*: a
  // plain type has the wrong kind. (A wrong-arity constructor was already
  // diagnosed during resolution — `check_ctor_against_expected`.)
  if (hk_param != nullptr && decl.for_type != nullptr &&
      !session_.types_.is_unknown(target) &&
      target_entry.kind != type_kind::ctor_ref_kind) {
    error_with_help(
        decl.for_type->span,
        std::format("trait `{}` abstracts over a type constructor, but "
                    "`{}` is a plain type",
                    trait_name, session_.types_.display(target)),
        "impl target has the wrong kind",
        std::format("`{}` declares its parameter as `{}[{}]`, so it is "
                    "implemented for a name that still *takes* type "
                    "arguments — `option` or `list`, say — not for an "
                    "already-complete type.",
                    trait_name, hk_param->name,
                    hk_param->higher_kinded_arity == 1 ? "_" : "_, _"));
  }

  if (trait_decl != nullptr) {
    check_impl_members(decl, *trait_decl, trait_name, target_entry);
  }

  for (const auto &constraint : decl.where_constraints) {
    if (constraint.subject != nullptr) {
      resolve_type(*constraint.subject, current_resolve_ctx());
    }
    if (constraint.bound_or_type != nullptr) {
      resolve_type(*constraint.bound_or_type, current_resolve_ctx());
    }
  }

  const auto saved_self = function_.self_type_;
  const auto saved_assoc = function_.self_assoc_types_;
  function_.self_type_ = target;
  function_.self_assoc_types_.clear();

  // Populate `self.<name>` associated-type references (impl-defined first,
  // falling back to the trait's default) before checking any method body,
  // since a method may reference an associated type defined later in the
  // same impl.
  if (trait_decl != nullptr) {
    for (const auto &item : trait_decl->items) {
      if (item == nullptr ||
          item->kind != ast::node_kind::associated_type_decl_node) {
        continue;
      }
      const auto &assoc =
          dynamic_cast<const ast::associated_type_decl_node &>(*item);
      if (assoc.value.default_type != nullptr) {
        function_.self_assoc_types_.emplace(
            assoc.value.name,
            resolve_type(*assoc.value.default_type, current_resolve_ctx()));
      }
    }
  }
  for (const auto &item : decl.items) {
    if (item == nullptr || item->has_error ||
        item->kind != ast::node_kind::associated_type_def_node) {
      continue;
    }
    const auto &assoc =
        dynamic_cast<const ast::associated_type_def_node &>(*item);
    const auto resolved =
        assoc.value.type != nullptr
            ? resolve_type(*assoc.value.type, current_resolve_ctx())
            : k_unknown_type;
    function_.self_assoc_types_.insert_or_assign(assoc.value.name, resolved);
  }

  // Persist this impl's associated-type bindings under (target, trait) so
  // a method call resolved from outside the impl (an operator overload's
  // dispatch — see `resolve_operator_return_type`) can still resolve a
  // `self.output`-shaped return type correctly.
  if (!trait_name.empty() && !session_.types_.is_unknown(target)) {
    session_.impl_assoc_types_[target][trait_name] =
        function_.self_assoc_types_;
    // A parameterized trait can be implemented more than once for the
    // same target — `index[usize]` for elements and `index[range[usize]]`
    // for sub-views, with different `output`s. Under the bare trait name
    // those two overwrite each other, so also file them apart by their
    // arguments; `parameterized_trait_key` is how a use site asks for the
    // one it resolved against.
    if (const auto key =
            impl_trait_arguments_key(decl, trait_name, current_resolve_ctx());
        key != trait_name) {
      session_.impl_assoc_types_[target][key] = function_.self_assoc_types_;
    }
  }

  // After `function_.self_type_` and the associated types are bound, so the
  // trait's
  // `&self` and `self.output` read as this impl's types.
  if (trait_decl != nullptr && hk_param == nullptr) {
    check_impl_signatures(decl, *trait_decl, trait_owner, trait_name);
  }

  const auto saved_block_type_params = session_.enclosing_block_type_params_;
  session_.enclosing_block_type_params_ = &decl.type_params;
  for (const auto &item : decl.items) {
    if (item == nullptr || item->has_error) {
      continue;
    }
    if (item->kind == ast::node_kind::func_decl) {
      check_function(dynamic_cast<const ast::func_decl &>(*item),
                     /*at_module_scope=*/false);
    } else if (item->kind == ast::node_kind::associated_type_def_node) {
      // Already resolved above.
    } else {
      check_item(*item, /*at_module_scope=*/false);
    }
  }
  session_.enclosing_block_type_params_ = saved_block_type_params;
  function_.self_type_ = saved_self;
  function_.self_assoc_types_ = saved_assoc;
  pop_type_params();
}

/// Checks an `extend` block: resolves its target type and checks each
/// method with `function_.self_type_` bound to it. Unlike `check_impl_decl`,
/// there is no trait to satisfy, so there is no coherence, completeness, or
/// `requires` checking — `extend` makes no conformance claim, so it needs
/// none of that bookkeeping.
auto checker::check_extend_decl(const ast::extend_decl &decl) -> void {
  // A parameterized block's parameters scope its target type *and* every
  // member, so they go on the stack before the target resolves — otherwise
  // `extend[T] holder[T]` cannot even name `T` in its own `extend` clause.
  push_type_params(decl.type_params);
  record_declared_facts(decl.type_params, nullptr);
  const auto target =
      decl.for_type != nullptr
          ? strip_refs(resolve_type(*decl.for_type, current_resolve_ctx()))
          : k_unknown_type;

  // `extend gen:` where `gen` is generic: the target names a constructor
  // that still takes arguments, so no member can say what `self`'s fields
  // hold. This used to type-check and then die in lowering on the first use
  // of a field ("no concrete checked type is available for this node"),
  // which told the user nothing about the actual mistake.
  const auto &target_entry = session_.types_.entry(target);
  const auto target_is_unapplied_ctor =
      target_entry.kind == type_kind::ctor_ref_kind ||
      (target_entry.decl != nullptr &&
       !target_entry.decl->type_params.empty() && target_entry.args.empty());
  if (decl.type_params.empty() && decl.for_type != nullptr &&
      target_is_unapplied_ctor) {
    const auto name = std::string(target_entry.name);
    error_with_help(
        decl.for_type->span,
        std::format("`{}` still takes type arguments, so `extend {}:` does "
                    "not name a type to extend",
                    name, name),
        "extend target is a type constructor, not a type",
        std::format(
            "Give the block its own parameter and pass it along — "
            "`extend[T] {}[T]:` — to add methods to every `{}`, or name one "
            "instantiation, as in `extend {}[int32]:`.",
            name, name, name));
  }

  const auto saved_self = function_.self_type_;
  function_.self_type_ = target;
  const auto saved_block_type_params = session_.enclosing_block_type_params_;
  session_.enclosing_block_type_params_ = &decl.type_params;
  for (const auto &item : decl.items) {
    if (item == nullptr || item->has_error) {
      continue;
    }
    if (item->kind == ast::node_kind::func_decl) {
      check_function(dynamic_cast<const ast::func_decl &>(*item),
                     /*at_module_scope=*/false);
    }
  }
  session_.enclosing_block_type_params_ = saved_block_type_params;
  function_.self_type_ = saved_self;
  pop_type_params();
}

/// How a parameter type passes its value, read off the written type:
/// owned, `&`, `&mut`, or a bare `mut` view.
auto checker::passing_mode(const ast::type_expr *type) -> std::string_view {
  if (type == nullptr) {
    return "";
  }
  if (type->kind == ast::node_kind::ref_type) {
    return dynamic_cast<const ast::ref_type &>(*type).is_mut ? "&mut" : "&";
  }
  if (type->kind == ast::node_kind::mut_type) {
    return "mut";
  }
  return "owned";
}

/// Checks that each impl method's parameters and return type are the ones
/// its trait declares. A caller reaching the method through the trait (an
/// operator, a `where T: eq` bound) passes what the trait says, so any
/// difference is a mismatch between what the caller hands over and what
/// the body does with it. Passing mode matters most: an impl taking
/// `other: t` where the trait says `&self` would drop a value the caller
/// still owns.
///
/// Runs with `function_.self_type_` and `function_.self_assoc_types_` bound to
/// the impl. Full types are compared only when both sides resolve to known
/// types and neither method has its own type parameters; otherwise only the
/// passing mode is compared.
auto checker::check_impl_signatures(const ast::impl_decl &impl,
                                    const ast::trait_decl &trait,
                                    const module_members *trait_owner,
                                    std::string_view trait_name) -> void {
  auto trait_bindings = type_scope{};
  auto trait_args_known = true;
  if (!trait.type_params.empty()) {
    const auto *named =
        impl.trait_type != nullptr &&
                impl.trait_type->kind == ast::node_kind::named_type
            ? &dynamic_cast<const ast::named_type &>(*impl.trait_type)
            : nullptr;
    if (named == nullptr ||
        named->type_args.size() != trait.type_params.size()) {
      trait_args_known = false;
    } else {
      auto quiet_ctx = current_resolve_ctx();
      quiet_ctx.quiet = true;
      for (size_t i = 0; i < trait.type_params.size(); ++i) {
        const auto *argument = dynamic_cast<const ast::type_expr *>(
            named->type_args[i].value.get());
        if (argument == nullptr) {
          trait_args_known = false;
          break;
        }
        trait_bindings.emplace(trait.type_params[i].name,
                               resolve_type(*argument, quiet_ctx));
      }
    }
  }
  const auto trait_ctx = resolve_ctx{.module = trait_owner,
                                     .param_bindings = &trait_bindings,
                                     .use_type_param_stack = false,
                                     .quiet = true};
  auto impl_ctx = current_resolve_ctx();
  impl_ctx.quiet = true;
  const auto known = [&](type_id type) -> bool {
    return !mentions_unknown(type);
  };

  for (const auto &item : impl.items) {
    if (item == nullptr || item->has_error ||
        item->kind != ast::node_kind::func_decl) {
      continue;
    }
    const auto &fn = dynamic_cast<const ast::func_decl &>(*item);
    const ast::func_decl *declared = nullptr;
    for (const auto &trait_item : trait.items) {
      if (trait_item != nullptr &&
          trait_item->kind == ast::node_kind::func_decl &&
          dynamic_cast<const ast::func_decl &>(*trait_item).name == fn.name) {
        declared = &dynamic_cast<const ast::func_decl &>(*trait_item);
        break;
      }
    }
    // A missing member or a parameter-count mismatch is reported by
    // `check_impl_members`.
    if (declared == nullptr || declared->params.size() != fn.params.size()) {
      continue;
    }
    const auto compare_types = trait_args_known && fn.type_params.empty() &&
                               declared->type_params.empty();

    for (size_t i = 0; i < fn.params.size(); ++i) {
      const auto &param = fn.params[i];
      const auto &expected = declared->params[i];
      const auto name = param_name_of(param);
      if (name == "self" && param_name_of(expected) == "self") {
        const auto is_mut_self = [](const ast::param &p) -> bool {
          return p.pattern != nullptr &&
                 p.pattern->kind == ast::node_kind::binding_pattern &&
                 dynamic_cast<const ast::binding_pattern &>(*p.pattern).is_mut;
        };
        if (is_mut_self(param) != is_mut_self(expected)) {
          const auto *wrote = is_mut_self(param) ? "mut self" : "self";
          const auto *wants = is_mut_self(expected) ? "mut self" : "self";
          error_with_help(
              param.span,
              std::format("method `{}` takes `{}`, but trait `{}` declares "
                          "it with `{}`",
                          fn.name, wrote, trait_name, wants),
              "receiver differs from the trait",
              std::format("Write `{}` here. Code that calls `{}` through "
                          "the trait passes the receiver the way the trait "
                          "declares it.",
                          wants, fn.name));
        }
        continue;
      }
      if (param.type_annotation == nullptr ||
          expected.type_annotation == nullptr) {
        continue;
      }
      const auto expected_type =
          compare_types ? resolve_type(*expected.type_annotation, trait_ctx)
                        : k_unknown_type;
      const auto written_type =
          compare_types ? resolve_type(*param.type_annotation, impl_ctx)
                        : k_unknown_type;
      const auto both_known = known(expected_type) && known(written_type);
      const auto mode = passing_mode(param.type_annotation.get());
      const auto expected_mode = passing_mode(expected.type_annotation.get());
      if (both_known ? expected_type == written_type : mode == expected_mode) {
        continue;
      }
      const auto label_name = name.empty()
                                  ? std::format("parameter {}", i + 1)
                                  : std::format("parameter `{}`", name);
      const auto found_text = both_known ? session_.types_.display(written_type)
                                         : std::string(mode);
      const auto expected_text = both_known
                                     ? session_.types_.display(expected_type)
                                     : std::string(expected_mode);
      auto help = std::format(
          "Write `{}` here, as the trait does. Code that calls `{}` through "
          "the trait passes this argument as `{}`, so the method has to "
          "accept exactly that.",
          expected_text, fn.name, expected_text);
      if (mode != expected_mode) {
        help += expected_mode == "owned"
                    ? " The trait hands the value over, so the method owns "
                      "it."
                    : " A reference only borrows the value: the caller "
                      "keeps it, and the method must not drop it.";
      }
      error_with_help(
          param.type_annotation->span,
          std::format("{} of `{}` has type `{}`, but trait `{}` declares "
                      "it as `{}`",
                      label_name, fn.name, found_text, trait_name,
                      expected_text),
          "parameter type differs from the trait", help);
    }

    if (compare_types && fn.return_type != nullptr &&
        declared->return_type != nullptr) {
      const auto expected_type =
          resolve_type(*declared->return_type, trait_ctx);
      const auto written_type = resolve_type(*fn.return_type, impl_ctx);
      if (known(expected_type) && known(written_type) &&
          expected_type != written_type) {
        error_with_help(
            fn.return_type->span,
            std::format("method `{}` returns `{}`, but trait `{}` declares "
                        "it to return `{}`",
                        fn.name, session_.types_.display(written_type),
                        trait_name, session_.types_.display(expected_type)),
            "return type differs from the trait",
            std::format("Write `-> {}` here. Code that calls `{}` through "
                        "the trait reads its result as `{}`.",
                        session_.types_.display(expected_type), fn.name,
                        session_.types_.display(expected_type)));
      }
    }
  }
}

/// Validates an impl's members against its trait's requirements: every
/// non-default trait method must be implemented, every associated type
/// without a default must be defined, every impl method must actually be
/// a trait member with a matching parameter count, and every trait
/// `requires` obligation must already be satisfied by the target type.
auto checker::check_impl_members(const ast::impl_decl &impl,
                                 const ast::trait_decl &trait,
                                 std::string_view trait_name,
                                 const type_entry &target) -> void {
  auto impl_methods = std::unordered_map<std::string, const ast::func_decl *>{};
  auto impl_assoc_types = std::unordered_set<std::string>{};
  for (const auto &item : impl.items) {
    if (item == nullptr || item->has_error) {
      continue;
    }
    if (item->kind == ast::node_kind::func_decl) {
      const auto &fn = dynamic_cast<const ast::func_decl &>(*item);
      impl_methods.emplace(fn.name, &fn);
    } else if (item->kind == ast::node_kind::associated_type_def_node) {
      impl_assoc_types.insert(
          dynamic_cast<const ast::associated_type_def_node &>(*item)
              .value.name);
    }
  }

  auto trait_methods =
      std::unordered_map<std::string, const ast::func_decl *>{};
  auto missing_methods = std::string{};
  auto missing_assoc = std::string{};
  for (const auto &item : trait.items) {
    if (item == nullptr) {
      continue;
    }
    if (item->kind == ast::node_kind::func_decl) {
      const auto &fn = dynamic_cast<const ast::func_decl &>(*item);
      trait_methods.emplace(fn.name, &fn);
      const auto has_default =
          fn.body_expr != nullptr || !fn.body_stmts.empty();
      if (!has_default && !impl_methods.contains(fn.name)) {
        if (!missing_methods.empty()) {
          missing_methods += ", ";
        }
        missing_methods += std::format("`{}`", fn.name);
      }
    } else if (item->kind == ast::node_kind::associated_type_decl_node) {
      const auto &assoc =
          dynamic_cast<const ast::associated_type_decl_node &>(*item);
      if (assoc.value.default_type == nullptr &&
          !impl_assoc_types.contains(assoc.value.name)) {
        if (!missing_assoc.empty()) {
          missing_assoc += ", ";
        }
        missing_assoc += std::format("`{}`", assoc.value.name);
      }
    }
  }

  const auto target_name = target.decl != nullptr ? target.decl->name
                           : !target.name.empty()
                               ? target.name
                               : session_.types_.display(k_unknown_type);

  if (!missing_methods.empty()) {
    error_with_help(
        impl.span,
        std::format("implementation of trait `{}` for `{}` is missing "
                    "method{} {}",
                    trait_name, target_name,
                    missing_methods.find(',') != std::string::npos ? "s" : "",
                    missing_methods),
        "incomplete trait implementation",
        "Every trait method without a default body must be implemented.");
  }
  if (!missing_assoc.empty()) {
    error_with_help(
        impl.span,
        std::format("implementation of trait `{}` for `{}` is missing "
                    "associated type{} {}",
                    trait_name, target_name,
                    missing_assoc.find(',') != std::string::npos ? "s" : "",
                    missing_assoc),
        "incomplete trait implementation",
        "Define the associated type with `type name = ...` inside the "
        "impl.");
  }

  for (const auto &[name, fn] : impl_methods) {
    const auto it = trait_methods.find(name);
    if (it == trait_methods.end()) {
      error_with_help(
          fn->span,
          std::format("`{}` is not a member of trait `{}`", name, trait_name),
          "unexpected method in trait implementation",
          std::format("Move `{}` into an inherent `impl {}:` block, or add "
                      "it to the trait.",
                      name, target_name));
      continue;
    }
    if (fn->params.size() != it->second->params.size()) {
      auto diag = diagnostic(
          diagnostic_level::error,
          std::format("method `{}` takes {} parameter{}, but trait `{}` "
                      "declares it with {}",
                      name, fn->params.size(),
                      fn->params.size() == 1 ? "" : "s", trait_name,
                      it->second->params.size()),
          file_.file_id_);
      diag.with_label(fn->span, "signature differs from the trait");
      emit_diag(diag);
      mark_error();
    }
  }

  // `requires` obligations: implementing this trait demands the others.
  // Constructor targets (`impl monad for option`) participate too — their
  // coherence records are keyed by constructor name, which is exactly what
  // `type_has_trait` consults.
  if (trait.requires_bound.has_value() &&
      (target.decl != nullptr || target.kind == type_kind::ctor_ref_kind)) {
    for (const auto &term : trait.requires_bound->terms) {
      if (term.type == nullptr ||
          term.type->kind != ast::node_kind::named_type) {
        continue;
      }
      const auto &named = dynamic_cast<const ast::named_type &>(*term.type);
      if (named.path.empty()) {
        continue;
      }
      const auto &required = named.path.back();
      if (type_has_trait(target, required)) {
        continue;
      }
      const auto derived = session_.derived_impl_origin_spans_.contains(&impl);
      error_with_help(
          impl_report_span(impl),
          std::format("trait `{}` requires `{}`, but `{}` does not "
                      "implement it",
                      trait_name, required, target_name),
          derived ? "this `deriving` clause implements `" +
                        std::string(trait_name) + "`"
                  : "unsatisfied trait requirement",
          derived ? std::format("Add `{}` to this `deriving` clause (`{}` "
                                "is derived from the same field list), or "
                                "write `impl {} for {}` by hand.",
                                required, required, required, target_name)
                  : std::format("Add `impl {} for {}` (or derive it) before "
                                "implementing `{}`.",
                                required, target_name, trait_name));
    }
  }
}

/// Checks a `trait` declaration: binds its type parameters and a
/// self-referential `self` type parameter, resolves its `requires` bound
/// and associated-type defaults, and checks each method declaration
/// (including any default body) as an ordinary function.
auto checker::check_trait_decl(const ast::trait_decl &decl) -> void {
  push_type_params(decl.type_params);
  const auto saved_self = function_.self_type_;
  const auto saved_assoc = function_.self_assoc_types_;
  function_.self_type_ = session_.types_.type_param("self");
  function_.self_assoc_types_.clear();
  record_declared_facts(decl.type_params, nullptr);
  // Inside its own body a trait's `self` implements the trait itself, at
  // its own parameters — which is what lets a default method call its
  // siblings — and everything the trait `requires`.
  {
    auto self_args = std::vector<type_id>{};
    for (const auto &param : decl.type_params) {
      self_args.push_back(session_.types_.type_param(
          param.name, param.higher_kinded_arity, &param));
    }
    auto self_facts = std::vector<type_fact>{};
    add_fact_closure(decl, std::move(self_args), self_facts);
    scoped_facts_.emplace_back(function_.self_type_, std::move(self_facts));
  }

  if (decl.requires_bound.has_value()) {
    for (const auto &term : decl.requires_bound->terms) {
      if (term.type != nullptr) {
        resolve_type(*term.type, current_resolve_ctx());
      }
    }
  }

  // Populate `self.<name>` associated-type references before checking any
  // method signature, since a method may reference an associated type
  // declared later in the same trait body.
  for (const auto &item : decl.items) {
    if (item == nullptr || item->has_error ||
        item->kind != ast::node_kind::associated_type_decl_node) {
      continue;
    }
    const auto &assoc =
        dynamic_cast<const ast::associated_type_decl_node &>(*item);
    const auto resolved =
        assoc.value.default_type != nullptr
            ? resolve_type(*assoc.value.default_type, current_resolve_ctx())
            : k_unknown_type;
    function_.self_assoc_types_.emplace(assoc.value.name, resolved);
  }

  const auto saved_block_type_params = session_.enclosing_block_type_params_;
  session_.enclosing_block_type_params_ = &decl.type_params;
  for (const auto &item : decl.items) {
    if (item == nullptr || item->has_error) {
      continue;
    }
    if (item->kind == ast::node_kind::func_decl) {
      check_function(dynamic_cast<const ast::func_decl &>(*item),
                     /*at_module_scope=*/false);
    }
  }
  session_.enclosing_block_type_params_ = saved_block_type_params;

  scoped_facts_.pop_back();
  function_.self_type_ = saved_self;
  function_.self_assoc_types_ = saved_assoc;
  pop_type_params();
}

/// Checks a `concept` declaration: binds its parameters as generic type
/// parameters, then resolves each constraint's subject and (whether it's
/// a trait/concept bound or a compile-time value expression) its
/// bound-or-expression payload.
auto checker::check_concept_decl(const ast::concept_decl &decl) -> void {
  auto param_scope = type_scope{};
  for (const auto &param : decl.params) {
    if (!param.name.empty()) {
      // A concept parameter is its own node kind with no `ast::type_param`
      // to key on, so these keep the by-name identity.
      param_scope.emplace(
          param.name,
          session_.types_.type_param(param.name, param.higher_kinded_arity));
    }
  }
  function_.type_params_.push_back(std::move(param_scope));

  for (const auto &constraint : decl.constraints) {
    if (constraint.subject != nullptr) {
      resolve_type(*constraint.subject, current_resolve_ctx());
    }
    if (constraint.bound_or_expr != nullptr &&
        !constraint.bound_or_expr->has_error) {
      if (const auto *type = dynamic_cast<const ast::type_expr *>(
              constraint.bound_or_expr.get())) {
        resolve_type(*type, current_resolve_ctx());
      } else if (const auto *expr = dynamic_cast<const ast::expr *>(
                     constraint.bound_or_expr.get())) {
        push_scope();
        infer_expr(*expr, k_unknown_type);
        pop_scope();
      }
    }
  }

  pop_type_params();
}

/// Checks a `signature` declaration for well-formedness: each abstract
/// `type` member is bound as an opaque type parameter (so later members may
/// reference it, e.g. `def connect(url: str) -> conn`), then every `def`
/// member's signature and every required `static`'s type is resolved. A
/// signature is never a type and never enters `type_table`; it only
/// describes the shape a module must have to satisfy it
/// (`spec/module-values-design.md` §3).
auto checker::check_signature_decl(const ast::signature_decl &decl) -> void {
  auto param_scope = type_scope{};
  for (const auto &item : decl.items) {
    if (item == nullptr || item->has_error ||
        item->kind != ast::node_kind::associated_type_decl_node) {
      continue;
    }
    const auto &assoc =
        dynamic_cast<const ast::associated_type_decl_node &>(*item);
    if (!assoc.value.name.empty()) {
      param_scope.emplace(assoc.value.name,
                          session_.types_.type_param(assoc.value.name));
    }
  }
  function_.type_params_.push_back(std::move(param_scope));

  for (const auto &item : decl.items) {
    if (item == nullptr || item->has_error) {
      continue;
    }
    if (item->kind == ast::node_kind::func_decl) {
      check_function(dynamic_cast<const ast::func_decl &>(*item),
                     /*at_module_scope=*/false);
    } else if (item->kind == ast::node_kind::static_decl) {
      const auto &stat = dynamic_cast<const ast::static_decl &>(*item);
      if (stat.type_annotation != nullptr) {
        resolve_type(*stat.type_annotation, current_resolve_ctx());
      }
    }
  }

  pop_type_params();
}

/// Resolves the `signature` a module-parameter bound names, or `nullptr` if
/// the bound is not a single-signature bound. `DB: backend` stores its bound
/// as a `bound_type` wrapping one term whose `named_type` path names the
/// signature.
auto checker::resolve_bound_signature(const ast::type_expr &bound_expr) const
    -> const ast::signature_decl * {
  const ast::named_type *named = nullptr;
  if (const auto *bt = dynamic_cast<const ast::bound_type *>(&bound_expr)) {
    if (bt->value.terms.size() == 1 && bt->value.terms.front().type) {
      named = dynamic_cast<const ast::named_type *>(
          bt->value.terms.front().type.get());
    }
  } else if (const auto *nt =
                 dynamic_cast<const ast::named_type *>(&bound_expr)) {
    named = nt;
  }
  if (named == nullptr || named->path.empty()) {
    return nullptr;
  }
  const auto &name = named->path.back();
  // A signature named in a bound is looked up in the current module first,
  // then anywhere in the session (imports make it visible unqualified).
  if (file_.module_ != nullptr) {
    if (const auto it = file_.module_->signatures.find(name);
        it != file_.module_->signatures.end()) {
      return it->second.decl;
    }
  }
  for (const auto &[mod_name, members] : index_.modules) {
    if (const auto it = members.signatures.find(name);
        it != members.signatures.end()) {
      return it->second.decl;
    }
  }
  return nullptr;
}

/// Resolves the functor a `use m[args]` path names, or `nullopt` if no
/// parameterized module by that name is in scope.
auto checker::resolve_functor(const std::vector<std::string> &path) const
    -> std::optional<resolved_functor> {
  if (path.empty()) {
    return std::nullopt;
  }
  const auto &name = path.back();
  if (path.size() >= 2) {
    const auto owner_path =
        std::vector<std::string>(path.begin(), path.end() - 1);
    if (const auto *owner = find_session_module_of_path(owner_path)) {
      if (const auto it = owner->functors.find(name);
          it != owner->functors.end()) {
        return resolved_functor{.ref = &it->second,
                                .owner_module = owner->module_name};
      }
    }
  }
  if (file_.module_ != nullptr) {
    if (const auto it = file_.module_->functors.find(name);
        it != file_.module_->functors.end()) {
      return resolved_functor{.ref = &it->second,
                              .owner_module = file_.module_->module_name};
    }
  }
  for (const auto &[mod_name, members] : index_.modules) {
    if (const auto it = members.functors.find(name);
        it != members.functors.end()) {
      return resolved_functor{.ref = &it->second,
                              .owner_module = members.module_name};
    }
  }
  return std::nullopt;
}

/// Resolves a module named by a functor argument (a `named_type` path),
/// trying the path as written, then relative to the current module.
auto checker::resolve_arg_module(const ast::named_type &nt) const
    -> const module_members * {
  if (nt.path.empty()) {
    return nullptr;
  }
  const auto joined = join_strings(nt.path, ".");
  if (const auto *m = index_.find_module(joined)) {
    return m;
  }
  if (!file_.module_name_.empty()) {
    if (const auto *m = index_.find_module(
            append_module_name(file_.module_name_, joined))) {
      return m;
    }
  }
  // An imported module: `use pkg.postgres` binds `postgres` to that module.
  if (const auto *binding = find_import(joined)) {
    if (const auto *m = index_.find_module(join_strings(binding->path, "."))) {
      return m;
    }
  }
  return nullptr;
}

/// Whether two resolved types are compatible for structural satisfaction:
/// equal by interned id, or one is `k_unknown`/`k_error` so we don't cascade
/// a knowledge gap into a spurious mismatch.
auto checker::types_satisfy(type_id want, type_id got) const -> bool {
  if (want == k_unknown_type || got == k_unknown_type || want == k_error_type ||
      got == k_error_type) {
    return true;
  }
  return want == got;
}

/// Structural signature satisfaction: whether `mod` provides every member
/// `sig` requires, each `pub`. Collects *every* failure into `failures`
/// (concept-quality diagnostics list them all) and returns whether the set
/// is empty. Checks member existence, visibility, function arity, and — under
/// the abstract-type binding — deep parameter/return-type equality
/// (`spec/module-values-design.md` §3). Each abstract signature type
/// (`type conn`) binds to the module's concrete same-named type simply by
/// resolving the signature's types in the module's own resolve context, where
/// `conn` names the module's `pub type conn`.
auto checker::module_satisfies_signature(const module_members &mod,
                                         const ast::signature_decl &sig,
                                         std::vector<std::string> &failures)
    -> bool {
  // Resolve a signature type expression against the argument module, quietly
  // (the module and signature were already checked at their own sites, so no
  // fresh diagnostics belong here). Abstract types resolve to the module's
  // concrete same-named types by virtue of the module context.
  const auto resolve_in_module = [&](const ast::type_expr &t) -> type_id {
    return resolve_type(t, resolve_ctx{.module = &mod,
                                       .param_bindings = nullptr,
                                       .use_type_param_stack = false,
                                       .quiet = true});
  };
  for (const auto &item : sig.items) {
    if (item == nullptr || item->has_error) {
      continue;
    }
    switch (item->kind) {
    case ast::node_kind::associated_type_decl_node: {
      const auto &assoc =
          dynamic_cast<const ast::associated_type_decl_node &>(*item);
      const auto it = mod.types.find(assoc.value.name);
      if (it == mod.types.end()) {
        failures.push_back(std::format("missing type `{}`", assoc.value.name));
      } else if (it->second.decl->visibility != ast::visibility::pub) {
        failures.push_back(std::format("type `{}` is provided but not `pub`",
                                       assoc.value.name));
      }
      break;
    }
    case ast::node_kind::func_decl: {
      const auto &fn = dynamic_cast<const ast::func_decl &>(*item);
      const auto it = mod.functions.find(fn.name);
      if (it == mod.functions.end()) {
        failures.push_back(std::format("missing function `{}`", fn.name));
      } else if (it->second.decl->visibility != ast::visibility::pub) {
        failures.push_back(
            std::format("function `{}` is provided but not `pub`", fn.name));
      } else if (it->second.decl->params.size() != fn.params.size()) {
        failures.push_back(std::format(
            "function `{}` takes {} parameter(s), but the signature requires "
            "{}",
            fn.name, it->second.decl->params.size(), fn.params.size()));
      } else {
        // Same arity and `pub`: check each parameter and the return type
        // match under the abstract-type binding.
        const auto &have = *it->second.decl;
        for (size_t p = 0; p < fn.params.size(); ++p) {
          if (fn.params[p].type_annotation == nullptr ||
              have.params[p].type_annotation == nullptr) {
            continue;
          }
          const auto want = resolve_in_module(*fn.params[p].type_annotation);
          const auto got = resolve_in_module(*have.params[p].type_annotation);
          if (!types_satisfy(want, got)) {
            failures.push_back(std::format(
                "function `{}` parameter {} has type `{}`, but the signature "
                "requires `{}`",
                fn.name, p + 1, session_.types_.display(got),
                session_.types_.display(want)));
          }
        }
        if (fn.return_type != nullptr && have.return_type != nullptr) {
          const auto want = resolve_in_module(*fn.return_type);
          const auto got = resolve_in_module(*have.return_type);
          if (!types_satisfy(want, got)) {
            failures.push_back(std::format(
                "function `{}` returns `{}`, but the signature requires `{}`",
                fn.name, session_.types_.display(got),
                session_.types_.display(want)));
          }
        }
      }
      break;
    }
    case ast::node_kind::static_decl: {
      const auto &stat = dynamic_cast<const ast::static_decl &>(*item);
      const auto it = mod.statics.find(stat.name);
      if (it == mod.statics.end()) {
        failures.push_back(
            std::format("missing constant `static {}`", stat.name));
      } else if (it->second.decl->visibility != ast::visibility::pub) {
        failures.push_back(
            std::format("constant `{}` is provided but not `pub`", stat.name));
      } else if (stat.type_annotation != nullptr &&
                 it->second.decl->type_annotation != nullptr) {
        const auto want = resolve_in_module(*stat.type_annotation);
        const auto got = resolve_in_module(*it->second.decl->type_annotation);
        if (!types_satisfy(want, got)) {
          failures.push_back(std::format(
              "constant `{}` has type `{}`, but the signature requires `{}`",
              stat.name, session_.types_.display(got),
              session_.types_.display(want)));
        }
      }
      break;
    }
    default:
      break;
    }
  }
  return failures.empty();
}

/// Validates a functor instantiation `use m[args]`: resolves the functor,
/// checks argument arity, and — for each module parameter bounded by a
/// signature — checks its argument module `satisfies` the bound with
/// concept-quality diagnostics. Materializing the instantiated module (so
/// its members become usable through the alias) is the next phase; until
/// then a satisfied instantiation reports that its elaboration is pending.
auto checker::check_functor_instantiation(const ast::use_decl &decl) -> void {
  const auto functor = resolve_functor(decl.path);
  if (!functor.has_value()) {
    auto diag =
        diagnostic(diagnostic_level::error,
                   std::format("no parameterized module named `{}` is in scope",
                               join_strings(decl.path, ".")),
                   file_.file_id_);
    diag.with_label(decl.span, "instantiated here");
    diag.with_help(
        "`use path[args]` instantiates a parameterized module (a `module "
        "name[...]:` declaration). Check the name and that the module is "
        "imported.");
    emit_diag(diag);
    mark_file_error();
    return;
  }

  const auto &fdecl = *functor->ref->decl;
  if (decl.instantiation_args.size() != fdecl.type_params.size()) {
    auto diag = diagnostic(
        diagnostic_level::error,
        std::format("parameterized module `{}` takes {} argument(s), but {} "
                    "were supplied",
                    fdecl.name, fdecl.type_params.size(),
                    decl.instantiation_args.size()),
        file_.file_id_);
    diag.with_label(decl.span, "in this instantiation");
    emit_diag(diag);
    mark_file_error();
    return;
  }

  // Resolve each module parameter's argument and check satisfaction. Each
  // binding maps the parameter name to the argument module, which becomes an
  // import alias inside the instantiated body.
  std::vector<functor_arg_binding> module_bindings;
  std::vector<std::string> arg_key_parts;
  bool all_ok = true;
  for (size_t i = 0; i < fdecl.type_params.size(); ++i) {
    const auto &param = fdecl.type_params[i];
    const auto &arg = decl.instantiation_args[i];
    if (param.bound_or_type == nullptr) {
      arg_key_parts.emplace_back("?");
      continue; // an unbounded type parameter: no signature obligation
    }
    // `DB: backend` is syntactically indistinguishable from a value
    // parameter (`n: usize`) — the parser cannot tell a signature bound from
    // a value type. It is a module parameter exactly when its bound names a
    // signature, so resolution (not `is_value_param`) is what decides.
    const auto *sig = resolve_bound_signature(*param.bound_or_type);
    if (sig == nullptr) {
      arg_key_parts.emplace_back("?");
      continue; // an ordinary value/type/trait/concept parameter
    }
    const auto *arg_named =
        arg.value != nullptr
            ? dynamic_cast<const ast::named_type *>(arg.value.get())
            : nullptr;
    const module_members *arg_mod =
        arg_named != nullptr ? resolve_arg_module(*arg_named) : nullptr;
    if (arg_mod == nullptr) {
      auto diag = diagnostic(
          diagnostic_level::error,
          std::format("argument for module parameter `{}` must be a module "
                      "satisfying signature `{}`",
                      param.name, sig->name),
          file_.file_id_);
      diag.with_label(arg.span, "not a module");
      emit_diag(diag);
      all_ok = false;
      arg_key_parts.emplace_back("?");
      continue;
    }
    std::vector<std::string> failures;
    if (!module_satisfies_signature(*arg_mod, *sig, failures)) {
      auto diag =
          diagnostic(diagnostic_level::error,
                     std::format("module `{}` does not satisfy signature `{}`",
                                 arg_mod->module_name, sig->name),
                     file_.file_id_);
      diag.with_label(arg.span, "used here");
      for (const auto &failure : failures) {
        diag.children.emplace_back(diagnostic_level::note, failure,
                                   file_.file_id_);
      }
      diag.with_help(std::format(
          "A module satisfies `{}` structurally: it must provide each "
          "required `type`, `def`, and `static` as a `pub` member. Add or "
          "expose the members listed above.",
          sig->name));
      emit_diag(diag);
      all_ok = false;
      arg_key_parts.emplace_back("?");
      continue;
    }
    module_bindings.push_back(
        {.param_name = param.name, .arg_module_name = arg_mod->module_name});
    arg_key_parts.push_back(arg_mod->module_name);
  }

  if (!all_ok) {
    mark_file_error();
    return;
  }

  // Canonical instantiation key: functor's fully-qualified name plus each
  // argument's canonical module name. Memoized session-wide so two imports
  // of `audited[postgres]` share one module identity (applicative functor).
  const auto instantiation_key = std::format(
      "{}[{}]", append_module_name(functor->owner_module, fdecl.name),
      join_strings(arg_key_parts, ","));

  const auto alias_name = functor_alias_name(decl, fdecl.name);

  auto synth_it = functor_.functor_instances_.find(instantiation_key);
  if (synth_it == functor_.functor_instances_.end()) {
    const auto synth_name = materialize_functor(
        fdecl, *functor, module_bindings, instantiation_key, decl.span);
    if (!synth_name.has_value()) {
      // A body construct beyond v1 materialization; a diagnostic was already
      // emitted by `materialize_functor`.
      mark_file_error();
      return;
    }
    synth_it =
        functor_.functor_instances_.emplace(instantiation_key, *synth_name)
            .first;
  }

  // Bind the alias in the importing file to the instantiated module so
  // `db.query(...)` resolves to its members.
  index_.imports[file_.file_id_].push_back(import_binding{
      .local_name = alias_name,
      .path = split_module_name(synth_it->second),
      .leaf_name = {},
      .is_wildcard = false,
      .span = decl.span,
  });
}

/// The local name a functor instantiation is bound to: the `as` alias if
/// present, else the functor's own name.
auto checker::functor_alias_name(const ast::use_decl &decl,
                                 const std::string &functor_name)
    -> std::string {
  if (decl.selector.has_value() &&
      decl.selector->kind == ast::use_selector_kind::single &&
      !decl.selector->items.empty() &&
      decl.selector->items.front().alias.has_value()) {
    return *decl.selector->items.front().alias;
  }
  return functor_name;
}

/// Turns an instantiation key into a dotted-path-free module name usable as
/// a `program_index` key and in `import_binding::path` (which is split/
/// joined on `.`). Non-identifier characters become `_`.
auto checker::sanitize_module_name(std::string_view key) -> std::string {
  std::string out;
  out.reserve(key.size());
  for (const char c : key) {
    out.push_back((std::isalnum(static_cast<unsigned char>(c)) != 0) ? c : '_');
  }
  return out;
}

/// The bare target-type name of an `impl`/`extend` block's `for` type
/// (`impl show for handle` → `handle`), used both as the coherence/method-
/// table key and as the `functor_instance::impl_target` HIR lowering name.
auto checker::impl_target_name(const ast::type_expr *for_type) -> std::string {
  if (const auto *named = dynamic_cast<const ast::named_type *>(for_type);
      named != nullptr && !named->path.empty()) {
    return named->path.back();
  }
  return {};
}

/// Materializes a functor instantiation: clones each supported member of the
/// body (giving the instance distinct node identity for per-instantiation
/// type checking) and registers them under a fresh synthetic module so its
/// `impl`/`extend` members join the method table and coherence check. Body
/// *checking* is deferred (a `pending_functor_body` is recorded) until after
/// `build_method_table`/`validate_impl_coherence`. Returns the synthetic
/// module name, or `nullopt` if the body uses a construct materialization
/// does not support (a diagnostic is emitted). See
/// `spec/module-values-design.md` §4.
auto checker::materialize_functor(
    const ast::sub_module_decl &fdecl, const resolved_functor &functor,
    const std::vector<functor_arg_binding> &module_bindings,
    const std::string &instantiation_key, source_span use_span)
    -> std::optional<std::string> {
  auto synth_name = sanitize_module_name(instantiation_key);

  // Clone each member of the functor body. Materialization covers `def`,
  // `type`, `static` (binding), `impl`, `extend`, and `trait` members;
  // anything else (a non-binding `static` form, an inline submodule) falls
  // back with a clear diagnostic rather than a partial module. A clone gives
  // each instantiation distinct node identity so `session_.node_types_` and
  // diagnostics stay per-instantiation, while projections through a module
  // parameter (`DB.conn`, `DB.query(...)`) resolve via the import aliases
  // bound at check time.
  const auto reject_member = [&](source_span span,
                                 std::string_view what) -> void {
    auto diag =
        diagnostic(diagnostic_level::error,
                   std::format("instantiating `{}` is not supported yet: {}",
                               fdecl.name, what),
                   file_.file_id_);
    diag.with_label(use_span, "in this instantiation");
    diag.with_label(span, "unsupported functor-body member");
    diag.with_help(
        "materialization of a parameterized module supports `def`, `type`, "
        "`static` (binding), `impl`, `extend`, and `trait` members.");
    emit_diag(diag);
  };

  std::vector<const ast::func_decl *> cloned_funcs;
  std::vector<const ast::type_decl *> cloned_types;
  std::vector<const ast::static_decl *> cloned_statics;
  std::vector<const ast::impl_decl *> cloned_impls;
  std::vector<const ast::extend_decl *> cloned_extends;
  std::vector<const ast::trait_decl *> cloned_traits;
  for (const auto &item : fdecl.items) {
    if (item == nullptr || item->has_error) {
      continue;
    }
    switch (item->kind) {
    case ast::node_kind::use_decl:
    case ast::node_kind::module_decl:
      continue;
    case ast::node_kind::func_decl: {
      const auto &fn = dynamic_cast<const ast::func_decl &>(*item);
      auto cloned = ast::clone_func_decl(fn);
      if (!cloned.has_value()) {
        reject_member(fn.span, cloned.error().message);
        return std::nullopt;
      }
      auto *raw = cloned->get();
      functor_.synthetic_nodes_.push_back(
          ast::ptr<ast::node>(std::move(*cloned)));
      cloned_funcs.push_back(raw);
      break;
    }
    case ast::node_kind::type_decl: {
      const auto &td = dynamic_cast<const ast::type_decl &>(*item);
      auto cloned = ast::clone_type_decl(td);
      if (!cloned.has_value()) {
        reject_member(td.span, cloned.error().message);
        return std::nullopt;
      }
      auto *raw = cloned->get();
      functor_.synthetic_nodes_.push_back(
          ast::ptr<ast::node>(std::move(*cloned)));
      cloned_types.push_back(raw);
      break;
    }
    case ast::node_kind::static_decl: {
      const auto &sd = dynamic_cast<const ast::static_decl &>(*item);
      if (sd.decl_kind != ast::static_decl_kind::binding) {
        reject_member(sd.span, "its body contains a non-binding `static` form");
        return std::nullopt;
      }
      auto cloned = ast::clone_static_decl(sd);
      if (!cloned.has_value()) {
        reject_member(sd.span, cloned.error().message);
        return std::nullopt;
      }
      auto *raw = cloned->get();
      functor_.synthetic_nodes_.push_back(
          ast::ptr<ast::node>(std::move(*cloned)));
      cloned_statics.push_back(raw);
      break;
    }
    case ast::node_kind::impl_decl: {
      const auto &id = dynamic_cast<const ast::impl_decl &>(*item);
      auto cloned = ast::clone_impl_decl(id);
      if (!cloned.has_value()) {
        reject_member(id.span, cloned.error().message);
        return std::nullopt;
      }
      auto *raw = cloned->get();
      functor_.synthetic_nodes_.push_back(
          ast::ptr<ast::node>(std::move(*cloned)));
      cloned_impls.push_back(raw);
      break;
    }
    case ast::node_kind::extend_decl: {
      const auto &ed = dynamic_cast<const ast::extend_decl &>(*item);
      auto cloned = ast::clone_extend_decl(ed);
      if (!cloned.has_value()) {
        reject_member(ed.span, cloned.error().message);
        return std::nullopt;
      }
      auto *raw = cloned->get();
      functor_.synthetic_nodes_.push_back(
          ast::ptr<ast::node>(std::move(*cloned)));
      cloned_extends.push_back(raw);
      break;
    }
    case ast::node_kind::trait_decl: {
      const auto &tr = dynamic_cast<const ast::trait_decl &>(*item);
      auto cloned = ast::clone_trait_decl(tr);
      if (!cloned.has_value()) {
        reject_member(tr.span, cloned.error().message);
        return std::nullopt;
      }
      auto *raw = cloned->get();
      functor_.synthetic_nodes_.push_back(
          ast::ptr<ast::node>(std::move(*cloned)));
      cloned_traits.push_back(raw);
      break;
    }
    default:
      reject_member(item->span,
                    "its body contains a member that is not a `def`, "
                    "`type`, `static`, `impl`, `extend`, or `trait`");
      return std::nullopt;
    }
  }

  // Register the synthetic module and its members in the program index.
  // Types, traits, and statics are registered before functions so a
  // function's return type (`-> row`), a static reference, or a trait bound
  // resolves against them.
  auto &synth = index_.modules[synth_name];
  synth.module_name = synth_name;
  for (const auto *tr : cloned_traits) {
    synth.traits.insert_or_assign(
        tr->name, trait_decl_ref{.decl = tr, .file_id = functor.ref->file_id});
  }
  for (const auto *td : cloned_types) {
    synth.types.insert_or_assign(
        td->name, type_decl_ref{.decl = td, .file_id = functor.ref->file_id});
    if (td->definition != nullptr &&
        td->definition->kind == ast::node_kind::sum_type_def) {
      const auto &sum =
          dynamic_cast<const ast::sum_type_def &>(*td->definition);
      for (const auto &variant : sum.body.variants) {
        if (!variant.name.empty()) {
          synth.variants.insert_or_assign(
              variant.name, variant_ref{.sum_decl = td, .variant = &variant});
        }
      }
    }
  }
  for (const auto *sd : cloned_statics) {
    synth.statics.insert_or_assign(
        sd->name, static_decl_ref{.decl = sd, .file_id = functor.ref->file_id});
  }
  for (const auto *fn : cloned_funcs) {
    synth.functions.insert_or_assign(
        fn->name, func_decl_ref{.decl = fn, .file_id = functor.ref->file_id});
    // Record the clone for HIR lowering: `hir::lower_functor_modules` builds
    // a standalone `hir_module` named `synth_name` from every clone tagged
    // with it, so a `db.f(...)` call site's cross-module dispatch finds it.
    // Only `def`s lower to runtime code; `type` members are compile-time and
    // a scalar `static` is inlined at each reference (`static_const_values`).
    functor_.functor_instance_decls_.push_back(
        functor_instance{.decl = fn, .owner_module = synth_name});
  }
  // Register `impl`/`extend` blocks so `build_method_table` and
  // `validate_impl_coherence` (which run right after this pass) see them, and
  // record each method as an `impl_target::method` HIR instance so it lowers
  // to runtime code within the synthetic module.
  for (const auto *id : cloned_impls) {
    synth.impls.push_back(impl_ref{.decl = id,
                                   .module_name = synth_name,
                                   .file_id = functor.ref->file_id});
    const auto target = impl_target_name(id->for_type.get());
    for (const auto &member : id->items) {
      if (member != nullptr && member->kind == ast::node_kind::func_decl) {
        functor_.functor_instance_decls_.push_back(functor_instance{
            .decl = &dynamic_cast<const ast::func_decl &>(*member),
            .owner_module = synth_name,
            .impl_target = target});
      }
    }
  }
  for (const auto *ed : cloned_extends) {
    synth.extends.push_back(extend_ref{.decl = ed,
                                       .module_name = synth_name,
                                       .file_id = functor.ref->file_id});
    const auto target = impl_target_name(ed->for_type.get());
    for (const auto &member : ed->items) {
      if (member != nullptr && member->kind == ast::node_kind::func_decl) {
        functor_.functor_instance_decls_.push_back(functor_instance{
            .decl = &dynamic_cast<const ast::func_decl &>(*member),
            .owner_module = synth_name,
            .impl_target = target});
      }
    }
  }

  // Defer body checking until after the method table and coherence build:
  // a method call inside the body must resolve against a complete table.
  functor_.pending_functor_bodies_.push_back(
      pending_functor_body{.synth_name = synth_name,
                           .owner_module = functor.owner_module,
                           .functor_file = functor.ref->file_id,
                           .module_bindings = module_bindings,
                           .types = std::move(cloned_types),
                           .statics = std::move(cloned_statics),
                           .funcs = std::move(cloned_funcs),
                           .impls = std::move(cloned_impls),
                           .extends = std::move(cloned_extends),
                           .traits = std::move(cloned_traits)});
  return synth_name;
}

/// Checks every deferred functor-instantiation body (recorded by
/// `materialize_functor`), after the method table and coherence build. Each
/// module parameter is temporarily bound as an import alias to its argument
/// module in the functor's own file so the cloned bodies' `DB.conn` /
/// `DB.query(...)` projections resolve; the binding is restored afterward.
auto checker::check_pending_functor_bodies() -> void {
  const auto *saved_module = file_.module_;
  const auto saved_module_name = file_.module_name_;
  const auto saved_file_id = file_.file_id_;
  for (const auto &pending : functor_.pending_functor_bodies_) {
    const auto synth_it = index_.modules.find(pending.synth_name);
    if (synth_it == index_.modules.end()) {
      continue;
    }
    auto *synth = &synth_it->second;
    auto &functor_imports = index_.imports[pending.functor_file];
    const auto saved_import_count = functor_imports.size();
    // Give the cloned body its original lexical scope: a `use owner.*`
    // wildcard so a name declared alongside the functor (a sibling `trait`,
    // `type`, ...) resolves while checking the instantiation in its own
    // synthetic module. Then bind each module parameter as an import alias
    // to its argument module so `DB.conn` / `DB.query(...)` projections
    // resolve. Both are restored afterward.
    if (!pending.owner_module.empty()) {
      functor_imports.push_back(
          import_binding{.local_name = {},
                         .path = split_module_name(pending.owner_module),
                         .leaf_name = {},
                         .is_wildcard = true,
                         .span = source_span::dummy()});
    }
    for (const auto &binding : pending.module_bindings) {
      functor_imports.push_back(import_binding{
          .local_name = binding.param_name,
          .path = split_module_name(binding.arg_module_name),
          .leaf_name = {},
          .is_wildcard = false,
          .span = source_span::dummy(),
      });
    }

    file_.module_ = synth;
    file_.module_name_ = pending.synth_name;
    session_.functor_owner_module_ = pending.owner_module;
    file_.file_id_ = pending.functor_file;
    for (const auto *tr : pending.traits) {
      check_trait_decl(*tr);
    }
    for (const auto *td : pending.types) {
      check_type_decl(*td);
    }
    for (const auto *sd : pending.statics) {
      check_static_decl(*sd);
    }
    for (const auto *fn : pending.funcs) {
      check_function(*fn, /*at_module_scope=*/true);
    }
    for (const auto *id : pending.impls) {
      check_impl_decl(*id);
    }
    for (const auto *ed : pending.extends) {
      check_extend_decl(*ed);
    }

    functor_imports.resize(saved_import_count);
  }
  file_.module_ = saved_module;
  file_.module_name_ = saved_module_name;
  session_.functor_owner_module_.clear();
  file_.file_id_ = saved_file_id;
}

/// Checks one module/trait/impl-scope item, dispatching to the
/// appropriate check_* function for its declaration kind; a nested inline
/// submodule recurses with the module context switched to the submodule,
/// and anything else (a bare statement reached via unusual nesting) falls
/// through to `check_body_node`.
auto checker::check_item(const ast::node &item, bool at_module_scope) -> void {
  if (item.has_error) {
    return;
  }
  switch (item.kind) {
  case ast::node_kind::func_decl:
    check_function(dynamic_cast<const ast::func_decl &>(item), at_module_scope);
    return;
  case ast::node_kind::type_decl:
    check_type_decl(dynamic_cast<const ast::type_decl &>(item));
    return;
  case ast::node_kind::trait_decl:
    check_trait_decl(dynamic_cast<const ast::trait_decl &>(item));
    return;
  case ast::node_kind::impl_decl:
    check_impl_decl(dynamic_cast<const ast::impl_decl &>(item));
    return;
  case ast::node_kind::extend_decl:
    check_extend_decl(dynamic_cast<const ast::extend_decl &>(item));
    return;
  case ast::node_kind::concept_decl:
    check_concept_decl(dynamic_cast<const ast::concept_decl &>(item));
    return;
  case ast::node_kind::signature_decl:
    check_signature_decl(dynamic_cast<const ast::signature_decl &>(item));
    return;
  case ast::node_kind::static_decl:
    check_static_decl(dynamic_cast<const ast::static_decl &>(item));
    return;
  case ast::node_kind::sub_module_decl: {
    const auto &decl = dynamic_cast<const ast::sub_module_decl &>(item);
    if (decl.is_functor()) {
      // A functor's body is not elaborated as written — it is type-checked
      // per instantiation, after substituting the module arguments (see
      // `spec/module-values-design.md` §4). Checking it here, with the
      // parameters unbound, would report spurious `DB.conn`-style errors.
      return;
    }
    if (decl.items.empty()) {
      return;
    }
    const auto saved_module_name = file_.module_name_;
    const auto *saved_module = file_.module_;
    file_.module_name_ = append_module_name(file_.module_name_, decl.name);
    file_.module_ = index_.find_module(file_.module_name_);
    push_scope();
    for (const auto &child : decl.items) {
      if (child == nullptr || child->has_error) {
        continue;
      }
      if (const auto it = session_.item_splice_impls_.find(child.get());
          it != session_.item_splice_impls_.end()) {
        check_item(*it->second, /*at_module_scope=*/true);
        continue;
      }
      if (const auto *expr = dynamic_cast<const ast::expr *>(child.get());
          expr != nullptr ||
          dynamic_cast<const ast::stmt *>(child.get()) != nullptr) {
        check_body_node(*child, k_unknown_type);
      } else {
        check_item(*child, /*at_module_scope=*/true);
      }
      if (const auto it = session_.derived_trait_impls_.find(child.get());
          it != session_.derived_trait_impls_.end()) {
        for (const auto *impl : it->second) {
          check_item(*impl, /*at_module_scope=*/true);
        }
      }
    }
    pop_scope();
    file_.module_name_ = saved_module_name;
    file_.module_ = saved_module;
    return;
  }
  case ast::node_kind::use_decl:
  // Functor instantiations (`use m[args]`) are materialized in a pre-pass
  // (`discover_functor_instantiations`) before the method-table/coherence
  // build, not here — see `run_impl`. An ordinary `use`, a `dep`, and a
  // `module` header all need no per-item checking.
  case ast::node_kind::dep_decl:
  case ast::node_kind::module_decl:
    return;
  default:
    check_body_node(item, k_unknown_type);
    return;
  }
}

// ==========================================================================
//  Impl coherence (session-wide)
// ==========================================================================

/// Session-wide coherence check: for every trait impl in every module,
/// records the (trait, target-type) pair in `impl_trait_index_` and
/// reports a duplicate-implementation error (with both locations) if the
/// pair was already recorded — enforcing Cinder's "at most one impl per
/// (trait, type)" rule. Inherent impls (no trait) are skipped since they
/// cannot conflict by construction.
auto checker::validate_impl_coherence() -> void {
  for (const auto &[module_name, members] : index_.modules) {
    for (const auto &impl : members.impls) {
      if (impl.decl->has_error) {
        continue;
      }
      const auto trait_name = trait_name_of_impl(*impl.decl);
      if (trait_name.empty()) {
        continue; // inherent impls never conflict by construction
      }
      const auto target = strip_refs(resolve_impl_target(impl));
      const auto &target_entry = session_.types_.entry(target);
      if (session_.types_.is_unknown(target)) {
        continue;
      }
      const auto type_key = type_key_of(target_entry);
      auto &recorded_traits = traits_by_type_key_[type_key];
      if (!std::ranges::contains(recorded_traits, trait_name)) {
        recorded_traits.push_back(trait_name);
      }
      // Keyed by the trait *as instantiated*: `index[usize]` and
      // `index[range[usize]]` are two different capabilities a collection
      // can offer (an element and a sub-view), not two impls of one
      // trait. A trait with no arguments keys under its bare name, so
      // nothing about the `show`/`drop` cases changes.
      const auto key = std::format(
          "{}:{}", resolve_impl_trait_key(impl, trait_name), type_key);
      const auto bare_key = std::format("{}:{}", trait_name, type_key);
      const auto location =
          source_location{.file_id = impl.file_id, .span = impl.decl->span};
      // Both keys are filed first-wins: they answer "does this type
      // implement the trait at all" (`type_has_trait`, and the `requires`
      // check behind it), which two impls of one parameterized trait both
      // satisfy. Conflict detection is a separate question, decided below
      // against every impl already recorded rather than against whichever
      // one happened to claim the key.
      impl_trait_index_.emplace(
          key, recorded_impl{.location = location, .target = target});
      if (key != bare_key) {
        impl_trait_index_.emplace(
            bare_key, recorded_impl{.location = location, .target = target});
      }

      const auto trait_arg = resolve_impl_trait_argument(impl);
      const auto *trait_decl = resolve_trait_decl_from(
          trait_name, index_.find_module(impl.module_name));
      auto &recorded = impl_coherence_records_[bare_key];
      const auto found = std::ranges::find_if(
          recorded, [&](const coherence_record &seen) -> bool {
            return seen.trait_decl == trait_decl &&
                   impl_targets_overlap(seen.target, target) &&
                   impl_targets_overlap(seen.trait_arg, trait_arg);
          });
      const auto clash = found == recorded.end()
                             ? std::optional<coherence_record>{}
                             : std::optional<coherence_record>{*found};
      recorded.push_back(coherence_record{.location = location,
                                          .trait_decl = trait_decl,
                                          .target = target,
                                          .trait_arg = trait_arg});
      if (!clash.has_value()) {
        // Either the first impl for this pair, or two impls for two
        // different instantiations of one generic declaration — legal.
        continue;
      }
      auto duplicate = diagnostic(
          diagnostic_level::error,
          std::format("duplicate implementation of trait `{}` for `{}`",
                      trait_name, session_.types_.display(target)),
          impl.file_id);
      duplicate.with_label(impl.decl->span, "conflicting implementation");
      duplicate.children.push_back(
          diagnostic(diagnostic_level::note,
                     std::format("`{}` was first implemented for `{}` here",
                                 trait_name,
                                 session_.types_.display(clash->target)),
                     clash->location.file_id)
              .with_label(clash->location.span, "previous implementation"));
      duplicate.with_help(
          "A program contains at most one implementation of a trait for a "
          "type; remove or merge one of these.");
      emit_diag(duplicate);
      if (static_cast<size_t>(impl.file_id) < file_has_errors_.size()) {
        file_has_errors_[impl.file_id] = true;
      }
    }
  }
}

} // namespace cinder::semantic::detail
