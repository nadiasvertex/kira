#include "checker.h"

namespace cinder::semantic {
namespace detail {

checker::checker(program_index &index, diagnostic_bag &diag,
                 std::vector<bool> &file_has_errors)
    : index_(index), diag_(diag), file_has_errors_(file_has_errors),
      comptime_eval_(diag, 0) {
  comptime_eval_.set_variant_resolver(
      [this](const ast::node &node)
          -> std::optional<std::pair<std::string, std::string>> {
        return resolve_variant_tag(node);
      });
  comptime_eval_.set_path_resolver(
      [this](const ast::module_path_expr &path,
             bool root_is_local) -> comptime::evaluator::dotted_path_reading {
        return read_dotted_path_for_comptime(path, root_is_local);
      });
  comptime_eval_.set_name_resolver(
      [this](
          const std::string &name) -> comptime::evaluator::bare_name_reading {
        return read_bare_name_for_comptime(name);
      });
  comptime_eval_.set_method_resolver(
      [this](const ast::expr &receiver_expr, const comptime::value &receiver,
             const std::string &name)
          -> std::expected<const ast::func_decl *, std::string> {
        return find_comptime_method(receiver_expr, receiver, name);
      });
  comptime_eval_.set_generic_call_resolver(
      [this](const ast::call_expr &call)
          -> std::optional<
              std::vector<std::pair<std::string, comptime::value>>> {
        return comptime_call_type_args(call);
      });
  comptime_eval_.set_call_context(
      [this](const ast::func_decl &fn,
             const std::function<comptime::value()> &body)
          -> cinder::comptime::value {
        const auto owner = function_owner(fn);
        if (!owner.has_value()) {
          return body();
        }
        return in_module_context(*owner->first, owner->second, body);
      });
  // A default is a candidate the queue offers once everything else has
  // stalled, not a rule inside the solver. The candidate travels in the
  // obligation's payload, so this resolver does not know what `int32` is
  // for or which leaf asked — which is what keeps "an unconstrained
  // integer literal is `int32`" a statement about the language rather than
  // a branch in the unifier.
  leaf_queue_.set_resolver(
      infer::obligation_kind::defaulting,
      [this](const infer::obligation &goal) -> infer::obligation_report {
        // The literal's leaf may have been merged with another that is
        // just as open (an argument with the parameter of the call it is
        // passed to); the default then answers for both.
        const auto leaf = leaf_ctxt_.shallow_resolve(goal.watches.front());
        if (!leaf_ctxt_.is_meta(leaf)) {
          // Something real arrived while this was waiting. The default
          // loses, which is the only reason it is last.
          return {.outcome = infer::obligation_outcome::discharged};
        }
        const auto candidate = static_cast<type_id>(goal.payload);
        // A candidate that will not bind has simply lost — it is a *last
        // resort*, not a requirement, so "this default does not apply
        // here" is a discharge and not a failure. Reporting it as a
        // failure aborts the whole flush, and every later literal that
        // would have taken `int32` is left open by the one that could
        // not: one leaf's refusal became every leaf's.
        (void)leaf_ctxt_.bind(leaf, candidate, infer::k_no_cause);
        return {.outcome = infer::obligation_outcome::discharged};
      });
}

/// Hands the session's interned types and per-expression type map to the
/// caller, once `run` has finished. Moves both out of the checker, so call
/// this at most once, after `run` returns.
/// Everything the checker synthesized that must outlive it: the compile-
/// time evaluator's quoted fragments, plus the desugared body of every
/// `try_from`. Both are pointed at by `spliced_fragments`, which lowering
/// reads long after the checker is gone.
auto checker::take_synthesized_fragments() -> ast::ptr_vec<ast::node> {
  auto fragments = comptime_eval_.take_synthesized_fragments();
  for (auto &fragment : session_.proof_fragments_) {
    fragments.push_back(std::move(fragment));
  }
  session_.proof_fragments_.clear();
  return fragments;
}

/// Supplies the module graph for validating module-rooted dotted paths
/// (see `validate_module_path`).
auto checker::set_module_graph(const module_session_index &session_index,
                               const semantic_resolution_index &semantic_index)
    -> void {
  session_.session_index_ = &session_index;
  session_.semantic_index_ = &semantic_index;
}

auto checker::take_checked_types() -> checked_types {
  const auto fmt_types = resolve_fmt_runtime_types();
  resolve_drop_plans();
  // Drop resolution runs after `run_impl` has finished, so it is the one
  // requester with no walk left to flush behind it. Without this, every
  // `list[T]::drop` and the `mem::free[T]` each one calls was cloned and
  // named and then never checked — a function the backends are told to
  // compile and nothing ever compiled, which is the exact defect the phase
  // 0 snapshot was built to catch. It caught it.
  //
  // Before the sweeps below, not after, because checking those bodies is
  // itself a walk: it records types of its own, and any leaf it mints has
  // to be settled by the same pass that settles every other.
  flush_deferred();
  // Refinements have done their work by now — every obligation is raised
  // and discharged during checking, and a predicate has no runtime
  // existence. Erase them here, once, at the boundary, so no downstream
  // pass (HIR, layout, either backend) ever has to know they existed. This
  // is the same trick `resolve_opaque` plays for existential types, applied
  // one layer earlier because a refinement can hide *inside* a type
  // (`option[index[n]]`) where an unwrap at the use site wouldn't reach it.
  // A leaf unknown is recorded at the node that minted it, which is before
  // anything has said what it is — so the recorded type is the variable and
  // not the answer, even long after the answer arrived. Substitute the
  // solutions in here, at the boundary, for the same reason refinements are
  // erased here: nothing downstream should have to know an inference
  // variable ever existed. Without it `var out = []` in an annotated
  // function records `list[_]` and hands a type containing a metavariable
  // to lowering, which survives only as long as nothing lowers it.
  //
  // Guarded on there having been any leaves, since zonking rebuilds a type
  // through the table's constructors and a session with none must not pay
  // that on every node it typed.
  for (auto &[node, type] : session_.node_types_) {
    type = session_.types_.erase_refinements(settle(type));
  }
  auto open_param_templates = std::unordered_set<const ast::func_decl *>{};
  for (const auto &[decl, open] : open_param_decls_) {
    if (open) {
      open_param_templates.insert(decl);
    }
  }
  for (auto &[field, type] : session_.struct_pattern_field_types_) {
    type = session_.types_.erase_refinements(settle(type));
  }
  for (auto &[literal, conversion] : session_.array_literal_conversions_) {
    conversion.array_type =
        session_.types_.erase_refinements(settle(conversion.array_type));
  }
  for (auto &[field, type] : session_.struct_literal_field_types_) {
    type = session_.types_.erase_refinements(settle(type));
  }
  for (auto &[field, type] : session_.struct_literal_field_expected_) {
    type = session_.types_.erase_refinements(settle(type));
  }
  for (auto &[node, dispatch] : session_.for_iterator_dispatches_) {
    dispatch.element_type =
        session_.types_.erase_refinements(settle(dispatch.element_type));
    dispatch.adapter_result_type =
        session_.types_.erase_refinements(settle(dispatch.adapter_result_type));
  }
  for (auto &[node, dispatch] : session_.comprehension_iterator_dispatches_) {
    dispatch.element_type =
        session_.types_.erase_refinements(settle(dispatch.element_type));
    dispatch.adapter_result_type =
        session_.types_.erase_refinements(settle(dispatch.adapter_result_type));
  }
  for (auto &[path, segment_types] : session_.value_path_types_) {
    for (auto &type : segment_types) {
      type = session_.types_.erase_refinements(settle(type));
    }
  }
  const auto settle_steps = [this](std::vector<deref_step> &steps) -> void {
    for (auto &step : steps) {
      step.callee_type =
          session_.types_.erase_refinements(settle(step.callee_type));
      step.result = session_.types_.erase_refinements(settle(step.result));
    }
  };
  for (auto &[receiver, steps] : session_.deref_adjustments_) {
    settle_steps(steps);
  }
  for (auto &[path, segments] : session_.path_deref_adjustments_) {
    for (auto &steps : segments) {
      settle_steps(steps);
    }
  }
  for (auto &[node, dispatch] : session_.interp_dispatches_) {
    dispatch.value_type =
        session_.types_.erase_refinements(settle(dispatch.value_type));
  }
  // Settling interns the solved types — a generic lambda's copy is typed
  // `fn(int64) -> int64` only now — after `resolve_drop_plans` ran. A
  // closure's plan does not depend on its signature, so every `fn` type
  // gets one here too.
  for (std::size_t raw = 0; raw < session_.types_.count(); ++raw) {
    const auto id = static_cast<type_id>(raw);
    auto scratch = std::unordered_set<type_id>{};
    if (session_.types_.entry(id).kind == type_kind::fn_kind &&
        !session_.drop_plans_.contains(id) && !mentions_type_var(id, scratch)) {
      session_.drop_plans_.emplace(
          id, drop_plan{.closure_glue = session_.closure_glue_type_});
    }
  }
  // Precompute which interned types carry a view, before `session_.types_` is
  // moved out below — the borrow checker reads this to track view-borrow
  // lifetimes.
  const auto type_count = session_.types_.count();
  auto borrow_bearing = compute_borrow_bearing_types(type_count);
  auto own_borrow_bearing =
      compute_borrow_bearing_types(type_count, /*params_bear=*/false);
  auto copy_sum_types = std::unordered_set<type_id>{};
  for (std::size_t raw = 0; raw < session_.types_.count(); ++raw) {
    const auto id = static_cast<type_id>(raw);
    if (is_sum_like(id) && value_is_copy(id)) {
      copy_sum_types.insert(id);
    }
  }
  auto result = checked_types{
      .types = std::move(session_.types_),
      .node_types = std::move(session_.node_types_),
      .node_files = std::move(session_.node_files_),
      .call_argument_mappings = std::move(session_.call_argument_mappings_),
      .resolved_callees = std::move(session_.resolved_callees_),
      .for_iterator_dispatches = std::move(session_.for_iterator_dispatches_),
      .comprehension_iterator_dispatches =
          std::move(session_.comprehension_iterator_dispatches_),
      .inferred_return_types = std::move(inferred_returns_),
      .spliced_fragments = std::move(session_.spliced_fragments_),
      .static_global_refs = std::move(session_.static_global_refs_),
      .value_path_types = std::move(session_.value_path_types_),
      .deref_adjustments = std::move(session_.deref_adjustments_),
      .path_deref_adjustments = std::move(session_.path_deref_adjustments_)};
  result.lowering = lowering_results{
      .struct_pattern_field_types =
          std::move(session_.struct_pattern_field_types_),
      .struct_literal_field_types =
          std::move(session_.struct_literal_field_types_),
      .struct_literal_field_expected =
          std::move(session_.struct_literal_field_expected_),
      .resolved_fn_values = std::move(session_.resolved_fn_values_),
      .operator_dispatches = std::move(session_.operator_dispatches_),
      .ord_dispatch_result_types =
          std::move(session_.ord_dispatch_result_types_),
      .index_dispatches = std::move(session_.index_dispatches_),
      .index_set_dispatches = std::move(session_.index_set_dispatches_),
      .index_mut_dispatches = std::move(session_.index_mut_dispatches_),
      .index_ref_dispatches = std::move(session_.index_ref_dispatches_),
      .array_literal_conversions =
          std::move(session_.array_literal_conversions_),
      .interp_dispatches = std::move(session_.interp_dispatches_),
      .type_param_reflections = std::move(session_.type_param_reflections_),
      .comprehension_dispatches = std::move(session_.comprehension_dispatches_),
      .runtime_fill_dispatches = std::move(session_.runtime_fill_dispatches_),
      .static_for_elements = std::move(cache_.static_for_elements_),
      .static_for_dispatches = std::move(cache_.static_for_dispatches_),
      .static_for_iterations = std::move(cache_.static_for_iterations_),
      .try_conversions = std::move(session_.try_conversions_),
      .try_conversion_types = std::move(session_.try_conversion_types_),
      .fmt_types = fmt_types,
      .synthesized_decls = std::move(session_.synthesized_decls_),
      .synthesized_trait_defaults =
          std::move(session_.synthesized_trait_defaults_),
      .const_generic_instances = std::move(session_.const_generic_instances_),
      .open_param_templates = std::move(open_param_templates),
      .synthesized_lambdas = std::move(synthesized_lambdas_),
      .comptime_only_functions = std::move(session_.comptime_only_functions_),
      .synthesized_functor_nodes = std::move(functor_.synthetic_nodes_),
      .functor_instances = std::move(functor_.functor_instance_decls_),
      .synthesized_types = std::move(session_.synthesized_types_),
      .synthesized_fragments = take_synthesized_fragments(),
      .synthesized_item_splices = std::move(session_.synthesized_item_splices_),
      .synthesized_const_literals =
          std::move(session_.synthesized_const_literals_),
      .static_const_values = std::move(session_.static_const_values_),
      .folded_comptime_calls = std::move(session_.folded_comptime_calls_),
      .layout_queries = std::move(session_.layout_queries_),
      .ptr_casts = std::move(session_.ptr_casts_),
      .slice_from_raw_parts_calls =
          std::move(session_.slice_from_raw_parts_calls_),
      .stack_buffers = std::move(session_.stack_buffers_),
      .static_global_defs = std::move(session_.static_global_defs_),
      .static_if_taken_branch = std::move(session_.static_if_taken_branch_),
      .static_struct_values = std::move(session_.static_struct_values_),
      .synthesized_static_structs =
          std::move(session_.synthesized_static_structs_),
      .unreadable_static_refs = std::move(session_.unreadable_static_refs_),
      .static_global_owners = std::move(session_.static_global_owners_),
      .proven_in_bounds = std::move(session_.proven_in_bounds_),
      .elided_contracts = std::move(session_.elided_contracts_)};
  result.ownership = ownership_results{
      .generic_lambda_instances = std::move(generic_lambda_instances_),
      .generic_lambda_refs = std::move(generic_lambda_refs_),
      .generic_lambda_uses = std::move(generic_lambda_uses_),
      .implicit_signatures = std::move(implicit_signatures_),
      .instance_templates = std::move(instance_templates_),
      .borrow_bearing_types = std::move(borrow_bearing),
      .own_borrow_bearing_types = std::move(own_borrow_bearing),
      .copy_type_params = std::move(copy_type_params_),
      .copy_sum_types = std::move(copy_sum_types)};
  result.drop = drop_results{
      .drop_plans = std::move(session_.drop_plans_)};
  return result;
}

/// Resolves `std.fmt`'s runtime-support types (`format_spec`, `align_mode`,
/// `sign_mode`) into `type_id`s for `hir::lower` — see
/// `fmt_runtime_types`'s doc comment for why this can't
/// just be a lookup lowering does itself. A no-op (all-zero) result if
/// `std.fmt` isn't part of this session at all.
auto checker::resolve_fmt_runtime_types() -> fmt_runtime_types {
  auto result = fmt_runtime_types{};
  const auto *fmt_module = index_.find_module("std.fmt");
  if (fmt_module == nullptr) {
    return result;
  }
  const auto resolve = [&](std::string_view name) -> type_id {
    const auto it = fmt_module->types.find(std::string(name));
    if (it == fmt_module->types.end() || it->second.decl == nullptr) {
      return 0;
    }
    return session_.types_.user_type(*it->second.decl, "std.fmt", {});
  };
  result.format_spec = resolve("format_spec");
  result.align_mode = resolve("align_mode");
  result.sign_mode = resolve("sign_mode");
  result.str_type = session_.types_.builtin("str");
  result.int64_type = session_.types_.builtin("int64");
  result.uint64_type = session_.types_.builtin("uint64");
  result.uint32_type = session_.types_.builtin("uint32");
  result.uint8_type = session_.types_.builtin("uint8");
  result.float64_type = session_.types_.builtin("float64");
  if (result.align_mode != 0) {
    result.option_align_mode =
        session_.types_.builtin_generic("option", {result.align_mode});
  }
  result.option_usize =
      session_.types_.builtin_generic("option", {usize_type()});
  return result;
}

/// The interned id for the builtin `usize` type — a thin wrapper so
/// `resolve_fmt_runtime_types` reads the same way regardless of whether
/// `usize` happens to already be interned (`type_table::builtin` interns
/// lazily either way, so this is just a readable spelling).
auto checker::usize_type() -> type_id {
  return session_.types_.builtin("usize");
}

// ==========================================================================
//  Per-file entry
// ==========================================================================

/// Recomputes `file_.file_has_external_wildcard_` for the file currently being
/// checked: true when it has a `use pkg.*` wildcard import whose root the
/// session does not own, meaning some undefined-looking name might
/// actually come from that external module.
auto checker::compute_external_wildcard() -> void {
  file_.file_has_external_wildcard_ = false;
  const auto *imports = imports_for_current_file();
  if (imports == nullptr) {
    return;
  }
  for (const auto &binding : *imports) {
    if (binding.is_wildcard && !session_owns_path_root(binding.path)) {
      file_.file_has_external_wildcard_ = true;
      return;
    }
  }
}

/// Checks one file's items end to end: enforces the "`main` xor top-level
/// statements" rule, recomputes the external-wildcard flag, and checks
/// every top-level item (statements via `check_body_node`, declarations
/// via `check_item`) in a fresh top-level scope.
auto checker::check_file(const ast::file &file) -> void {
  compute_external_wildcard();
  function_.reported_undefined_.clear();

  // A file may declare `main` or use top-level statements, not both.
  const ast::func_decl *main_decl = nullptr;
  const ast::node *first_statement = nullptr;
  for (const auto &item : file.items) {
    if (item == nullptr || item->has_error) {
      continue;
    }
    if (item->kind == ast::node_kind::func_decl) {
      const auto &fn = dynamic_cast<const ast::func_decl &>(*item);
      if (fn.name == "main") {
        main_decl = &fn;
      }
    } else if (session_.item_splice_impls_.contains(item.get())) {
      // Resolved to an injected `impl` block by `resolve_item_splices` —
      // an item, not a top-level script statement.
    } else if (first_statement == nullptr &&
               (dynamic_cast<const ast::stmt *>(item.get()) != nullptr ||
                dynamic_cast<const ast::expr *>(item.get()) != nullptr)) {
      first_statement = item.get();
    }
  }
  if (main_decl != nullptr && first_statement != nullptr) {
    auto diag = diagnostic(
        diagnostic_level::error,
        "a file may declare `main` explicitly or use top-level statements, "
        "but not both",
        file_.file_id_);
    diag.with_label(first_statement->span,
                    "top-level statement in a file that declares `main`");
    diag.children.push_back(
        diagnostic(diagnostic_level::note, "`main` is declared here",
                   file_.file_id_)
            .with_label(main_decl->span, "explicit `main`"));
    diag.with_help("Move these statements into `main`, or delete the "
                   "explicit `main` and keep the script form.");
    emit_diag(diag);
    mark_error();
  }

  push_scope();
  // Functions with an unannotated parameter first: that parameter is a
  // leaf the body solves, and a caller checked before the body would meet
  // it open and, at any point needing a type, default it before the
  // body's own uses were read. Among themselves they need no order — a
  // call ties the caller's argument to the callee's leaf, and whichever
  // body is read later solves both.
  auto checked_early = std::unordered_set<const ast::node *>{};
  for (const auto &item : file.items) {
    if (item != nullptr && !item->has_error &&
        item->kind == ast::node_kind::func_decl &&
        has_unannotated_params(dynamic_cast<const ast::func_decl &>(*item))) {
      checked_early.insert(item.get());
      check_item(*item, /*at_module_scope=*/true);
    }
  }
  for (const auto &item : file.items) {
    if (item == nullptr || item->has_error ||
        checked_early.contains(item.get())) {
      continue;
    }
    if (const auto it = session_.item_splice_impls_.find(item.get());
        it != session_.item_splice_impls_.end()) {
      // Already resolved by `resolve_item_splices` (before method-table/
      // coherence build) — check the injected impl itself, in place of
      // the splice node. Do not fall through to `check_body_node` below:
      // `splice_stmt` derives from `stmt`, so it would otherwise match
      // that branch and re-evaluate the operand a second time.
      check_item(*it->second, /*at_module_scope=*/true);
      continue;
    }
    if (dynamic_cast<const ast::stmt *>(item.get()) != nullptr ||
        dynamic_cast<const ast::expr *>(item.get()) != nullptr) {
      check_body_node(*item, k_unknown_type);
    } else {
      check_item(*item, /*at_module_scope=*/true);
    }
    if (const auto it = session_.derived_trait_impls_.find(item.get());
        it != session_.derived_trait_impls_.end()) {
      for (const auto *impl : it->second) {
        check_item(*impl, /*at_module_scope=*/true);
      }
    }
  }
  pop_scope();
}

/// Whether `name` reflects a module — matching a module's fully-qualified
/// name or its leaf segment, the two keys `register_comptime_modules`
/// registers. Lets `infer_field_call` type `M.functions()` and hand it to
/// compile-time evaluation instead of rejecting `M` as an undefined value.
auto checker::names_reflectable_module(std::string_view name) const -> bool {
  return std::ranges::any_of(index_.modules, [&](const auto &entry) -> auto {
    return entry.first == name || split_module_name(entry.first).back() == name;
  });
}

/// Splices `~derive_<trait>[TypeName]()` in behind the scenes, once per
/// entry in `k_real_derive_traits` present in `decl.deriving`, for a
/// concrete (non-generic), struct-shaped `type ... deriving <trait>` —
/// M7's replacement for the old hardcoded `derived_method_result` type-
/// only rule (each derived trait stays listed there too, purely for the
/// `type_has_trait` bookkeeping every `deriving`d trait needs; the *real*
/// runtime method now comes from here for `show`/`eq`/`debug`). Reuses
/// `resolve_item_splices`'s own tail exactly (evaluate, extract the
/// wrapped `impl_decl`, push into `index_.modules[...].impls`) via a
/// synthesized call — there's no literal `splice_stmt` AST node for this
/// sugar to key off, so nothing is recorded in `session_.item_splice_impls_`
/// (nothing will ever look one up for a plain `type_decl` item). A
/// generic type or a non-struct (sum/refinement) definition is left
/// entirely on the old type-only path unchanged: every `derive_<trait>
/// [T]()` body only supports a concrete, struct-shaped `T` (`T.fields()`
/// — see `reflect.cpp`), and the point of this milestone is a real,
/// working derivation, not a diagnostic regression for the shapes it
/// doesn't cover yet.
/// The span to report an impl-level diagnostic at: the impl's own span for
/// a hand-written `impl`, and the originating `deriving` clause's span for
/// one `resolve_deriving_traits` synthesized — see
/// `session_.derived_impl_origin_spans_` for why the synthesized impl's own
/// span is not usable.
auto checker::impl_report_span(const ast::impl_decl &impl) const
    -> source_span {
  const auto it = session_.derived_impl_origin_spans_.find(&impl);
  return it != session_.derived_impl_origin_spans_.end() ? it->second
                                                         : impl.span;
}

/// Synthesizes and evaluates `derive_<trait>[<decl.name>]()`, handing back
/// the `impl` block it produced — the one piece shared by the two paths
/// that derive: `resolve_deriving_traits` (concrete types, in a pre-pass)
/// and `ensure_derived_instance_impls` (generic types, once per concrete
/// instantiation).
///
/// `nullptr` means "no derivation available here", never "an error was
/// reported": the caller is expected to fall back to the type-check-only
/// `derived_method_result` path. In particular, a session that didn't
/// inject `std.derive` at all (several narrow test fixture sets don't) hits
/// the `has_pending_function` guard and degrades quietly, rather than
/// attempting a call `evaluate` can only fail with a leaked diagnostic.
///
/// Every call materializes a *fresh* fragment (`comptime::evaluator::
/// materialize_quote`), which is what makes per-instantiation derivation
/// work at all: `box[int32]` and `box[str]` each get their own `impl`
/// node, identical in spelling and distinct in identity, so each can be
/// registered against its own target type.
///
/// `instance_id` is the *concrete* type being derived for (`box[int32]`,
/// or the type's own id for a non-generic type) — used to precompute each
/// field's fully-substituted, canonically-rendered type name
/// (`struct_field_type` + `type_table::display`) and hand it to the
/// evaluator (`push_field_type_context`) for the duration of this call, so
/// `f.type_name` inside the derive body reports the instantiation's real
/// field types instead of the declaration's raw, un-substituted syntax
/// (`spec/todo.md` item 5).
auto checker::evaluate_derive_call(const ast::type_decl &decl,
                                   std::string_view trait_name,
                                   type_id instance_id)
    -> const ast::impl_decl * {
  if (decl.definition == nullptr) {
    return nullptr;
  }
  const bool is_sum_shaped =
      decl.definition->kind == ast::node_kind::sum_type_def;
  auto derive_fn_name = is_sum_shaped ? std::format("derive_{}_sum", trait_name)
                                      : std::format("derive_{}", trait_name);
  const auto *derive_fn = comptime_eval_.pending_function(derive_fn_name);
  if (derive_fn == nullptr) {
    return nullptr;
  }
  // The synthesized call names `derive_<trait>` bare, so it is evaluated
  // in the module declaring it (`std.derive`), not the one being checked,
  // which need not import it.
  const auto derive_owner = function_owner(*derive_fn);
  if (!derive_owner.has_value()) {
    return nullptr;
  }
  auto callee_ident = ast::make<ast::ident_expr>();
  callee_ident->span = decl.span;
  callee_ident->name = std::move(derive_fn_name);
  auto type_ident = ast::make<ast::ident_expr>();
  type_ident->span = decl.span;
  type_ident->name = decl.name;
  auto index = ast::make<ast::index_expr>();
  index->span = decl.span;
  index->object = std::move(callee_ident);
  index->index = std::move(type_ident);
  auto call = ast::make<ast::call_expr>();
  call->span = decl.span;
  call->callee = std::move(index);

  auto field_type_names =
      std::unordered_map<const ast::struct_field *, std::string>{};
  if (!is_sum_shaped) {
    const auto &struct_def =
        dynamic_cast<const ast::struct_type_def &>(*decl.definition);
    const auto &instance = session_.types_.entry(instance_id);
    for (const auto &field : struct_def.body.fields) {
      if (const auto field_type_id = struct_field_type(instance, field.name);
          field_type_id.has_value()) {
        field_type_names.emplace(&field,
                                 session_.types_.display(*field_type_id));
      }
    }
  }
  comptime_eval_.push_field_type_context(std::move(field_type_names));
  const auto fragment_value = in_module_context(
      *derive_owner->first, derive_owner->second,
      [&] -> comptime::value { return comptime_eval_.evaluate(*call); });
  comptime_eval_.pop_field_type_context();
  if (fragment_value.is_error() ||
      fragment_value.kind != comptime::value_kind::def_expr_fragment ||
      fragment_value.fragment == nullptr) {
    return nullptr;
  }
  return dynamic_cast<const ast::impl_decl *>(fragment_value.fragment);
}

/// Whether a `type_decl` is one `derive_<trait>`/`derive_<trait>_sum` knows
/// how to reflect over: a struct or a sum, never a refinement or an alias.
auto checker::derivable_shape(const ast::type_decl &decl) -> bool {
  return decl.definition != nullptr &&
         (decl.definition->kind == ast::node_kind::struct_type_def ||
          decl.definition->kind == ast::node_kind::sum_type_def);
}

/// The `deriving`-able trait a method name belongs to, or empty for a name
/// no derivation provides. The inverse of the mapping
/// `derived_method_result` applies in the other direction; `cmp` is the one
/// entry whose method name and trait name differ.
auto checker::derive_trait_for_method(std::string_view name)
    -> std::string_view {
  if (name == "cmp") {
    return "ord";
  }
  if (name == "show" || name == "debug" || name == "eq" || name == "hash") {
    return name;
  }
  return {};
}

/// `deriving` on a *generic* type, resolved once per concrete
/// instantiation (`spec/todo.md` item 5).
///
/// A generic `type box[T] = { value: T } deriving show` cannot be derived
/// where a concrete one is — `resolve_deriving_traits` runs as a pre-pass,
/// before any instantiation is known, and every `derive_<trait>[T]()` body
/// reflects over a single `T` via `T.fields()`/`T.variants()`. So `box`
/// used to stay on the type-check-only `derived_method_result` path:
/// `b.show()` type-checked and then failed lowering with "no concrete
/// checked type is available for this node".
///
/// Instead, each *instantiation* derives its own impl, the first time a
/// lookup on it needs one. `box[int32]` and `box[str]` get separate
/// fragments from separate `evaluate_derive_call`s, registered against
/// separate target types. Everything downstream is then the ordinary
/// concrete path: a method whose `impl_target_pattern` is an applied type
/// satisfies `impl_needs_instance`, so `check_impl_generic_method_call`
/// monomorphizes it through `find_or_check_generic_instance` — which
/// checks the body with `function_.self_type_` bound to the instantiation,
/// names it per instance, and records it in `session_.const_generic_instances_`
/// for `hir::lower_module` to emit. Nothing here has to name the applied type
/// syntactically, which is the reason the derived `impl`'s own `for_type`
/// is left exactly as the derivation wrote it (a bare `box`) and
/// `impl_target_pattern` carries the real target instead.
///
/// Returns whether anything was registered, so `find_method` knows whether
/// a second lookup is worth attempting.
auto checker::ensure_derived_instance_impls(type_id instance_id,
                                            std::string_view method_name)
    -> bool {
  const auto trait_name = derive_trait_for_method(method_name);
  if (trait_name.empty() || session_.types_.is_unknown(instance_id)) {
    return false;
  }
  const auto &entry = session_.types_.entry(instance_id);
  const auto *decl = entry.decl;
  // An *applied* generic: a `deriving` type with parameters, instantiated.
  // The bare template (`args` empty) has nothing concrete to reflect over,
  // and a non-generic type was already handled by the pre-pass.
  if (decl == nullptr || decl->type_params.empty() || entry.args.empty() ||
      !derivable_shape(*decl) ||
      !std::ranges::contains(decl->deriving, std::string(trait_name)) ||
      !std::ranges::contains(k_real_derive_traits, trait_name)) {
    return false;
  }
  if (!session_.derived_instances_
           .insert(std::format("{}#{}", instance_id, trait_name))
           .second) {
    // Already derived for this exact (instantiation, trait) pair. The
    // key is the *instantiation*, not the declaration: deriving once per
    // `box` rather than once per `box[int32]` is the bug this whole path
    // exists to avoid.
    return false;
  }
  const auto *owner = index_.find_module(entry.module_name);
  if (owner == nullptr) {
    return false;
  }
  const auto type_it = owner->types.find(decl->name);
  if (type_it == owner->types.end()) {
    return false;
  }
  const auto *impl = evaluate_derive_call(*decl, trait_name, instance_id);
  if (impl == nullptr) {
    return false;
  }
  session_.derived_impl_origin_spans_[impl] = decl->span;
  auto &methods = cache_.method_tables_.declared[decl];
  auto registered = false;
  for (const auto &item : impl->items) {
    if (item == nullptr || item->has_error ||
        item->kind != ast::node_kind::func_decl) {
      continue;
    }
    methods.push_back(method_entry{
        .decl = dynamic_cast<const ast::func_decl *>(item.get()),
        .owner = owner,
        .from_trait = nullptr,
        .file_id = type_it->second.file_id,
        // Non-null and empty, exactly as a hand-written non-generic `impl`
        // over an applied target registers: that combination is what marks
        // a method as needing a per-receiver instance (`impl_needs_
        // instance`), which is how the body ever gets compiled.
        .block_type_params = &impl->type_params,
        // The concrete instantiation, not the declaration's pattern, so
        // `find_method`'s `target_pattern_matches` admits this method for
        // `box[int32]` and not for `box[str]` — even though both share one
        // `cache_.method_tables_.declared` bucket, keyed by the `type_decl`.
        .impl_target_pattern = instance_id,
    });
    registered = true;
  }
  return registered;
}

auto checker::resolve_deriving_traits(const ast::type_decl &decl,
                                      file_id_type owner_file) -> void {
  if (decl.name.empty() || !decl.type_params.empty() ||
      !derivable_shape(decl)) {
    // A *generic* type derives lazily instead, once per concrete
    // instantiation — see `ensure_derived_instance_impls`. It cannot derive
    // here: this pre-pass runs before any instantiation is known, and a
    // single impl over `box[T]` is not what these derivations produce.
    return;
  }
  const auto instance_id = make_user_type(decl, file_.module_name_, {});
  for (const auto trait_name : k_real_derive_traits) {
    if (!std::ranges::contains(decl.deriving, std::string(trait_name))) {
      continue;
    }
    const auto *impl = evaluate_derive_call(decl, trait_name, instance_id);
    if (impl == nullptr) {
      continue;
    }
    index_.modules[file_.module_name_].impls.push_back(
        impl_ref{.decl = impl,
                 .module_name = file_.module_name_,
                 .file_id = owner_file});
    session_.synthesized_item_splices_.push_back(synthesized_item_splice{
        .impl = impl, .owner_module = file_.module_name_});
    session_.derived_trait_impls_[&decl].push_back(impl);
    session_.derived_impl_origin_spans_[impl] = decl.span;
  }
}

/// Walks a file's items (recursing into inline submodules with the module
/// context switched, exactly as `resolve_item_splices` does) and materializes
/// every functor instantiation `use m[args]`, so its members — including any
/// `impl`/`extend` — are registered before the method-table/coherence build.
auto checker::discover_functor_instantiations(
    const std::vector<ast::ptr<ast::node>> &items) -> void {
  for (const auto &item : items) {
    if (item == nullptr || item->has_error) {
      continue;
    }
    if (item->kind == ast::node_kind::use_decl) {
      const auto &use = dynamic_cast<const ast::use_decl &>(*item);
      if (!use.instantiation_args.empty()) {
        check_functor_instantiation(use);
      }
      continue;
    }
    if (item->kind == ast::node_kind::sub_module_decl) {
      const auto &sub = dynamic_cast<const ast::sub_module_decl &>(*item);
      if (sub.items.empty() || sub.is_functor()) {
        continue; // a functor declaration itself is materialized per `use`
      }
      const auto saved_module_name = file_.module_name_;
      const auto *saved_module = file_.module_;
      file_.module_name_ = append_module_name(file_.module_name_, sub.name);
      file_.module_ = index_.find_module(file_.module_name_);
      push_scope();
      discover_functor_instantiations(sub.items);
      pop_scope();
      file_.module_name_ = saved_module_name;
      file_.module_ = saved_module;
    }
  }
}

auto checker::resolve_item_splices(
    const std::vector<ast::ptr<ast::node>> &items, file_id_type owner_file)
    -> void {
  for (const auto &item : items) {
    if (item == nullptr || item->has_error) {
      continue;
    }
    if (item->kind == ast::node_kind::type_decl) {
      resolve_deriving_traits(dynamic_cast<const ast::type_decl &>(*item),
                              owner_file);
      continue;
    }
    if (item->kind == ast::node_kind::sub_module_decl) {
      const auto &sub = dynamic_cast<const ast::sub_module_decl &>(*item);
      if (sub.items.empty()) {
        continue;
      }
      const auto saved_module_name = file_.module_name_;
      const auto *saved_module = file_.module_;
      file_.module_name_ = append_module_name(file_.module_name_, sub.name);
      file_.module_ = index_.find_module(file_.module_name_);
      push_scope();
      resolve_item_splices(sub.items, owner_file);
      pop_scope();
      file_.module_name_ = saved_module_name;
      file_.module_ = saved_module;
      continue;
    }
    if (item->kind != ast::node_kind::splice_stmt) {
      continue;
    }
    const auto &splice = dynamic_cast<const ast::splice_stmt &>(*item);
    if (splice.expr == nullptr) {
      continue;
    }
    const auto operand_type = infer_expr(*splice.expr, k_unknown_type);
    require_quote_value(operand_type, splice.expr->span,
                        "an item-level splice");
    const auto fragment_value = comptime_eval_.evaluate(*splice.expr);
    if (fragment_value.is_error()) {
      continue;
    }
    if (fragment_value.kind != comptime::value_kind::def_expr_fragment ||
        fragment_value.fragment == nullptr) {
      error_with_help(
          splice.expr->span,
          std::format("an item-level splice must resolve to a quoted "
                      "definition (`def_expr`), found {}",
                      quote_fragment_kind_name(fragment_value.kind)),
          "this splice is used in item position",
          "Only a value quoted with definition content (e.g. "
          "`` `(impl show for point: ...)` ``) can be spliced among a "
          "module's top-level items; a quoted expression or statement "
          "belongs inside a function body instead.");
      continue;
    }
    const auto *impl =
        dynamic_cast<const ast::impl_decl *>(fragment_value.fragment);
    if (impl == nullptr) {
      error_with_help(
          splice.expr->span,
          "an item-level splice currently only supports injecting an "
          "`impl` block",
          "this quoted definition is not an `impl`",
          "Splicing a quoted `type`/`trait`/`def` at item position is "
          "not supported yet; wrap the generated code in an `impl` "
          "block instead.");
      continue;
    }
    index_.modules[file_.module_name_].impls.push_back(
        impl_ref{.decl = impl,
                 .module_name = file_.module_name_,
                 .file_id = owner_file});
    session_.item_splice_impls_[item.get()] = impl;
    session_.synthesized_item_splices_.push_back(synthesized_item_splice{
        .impl = impl, .owner_module = file_.module_name_});
  }
}

/// Runs impl coherence once for the whole session, then checks every
/// input file that has a valid module declaration and no already-recorded
/// error, setting up the current-file/current-module context before each.
/// Clears the parser's value-parameter mark on every `[T: Bound]` whose
/// bound names a trait or a concept, in `items` and everything nested in
/// them. The AST is the driver's, owned mutably; this is the one place the
/// checker writes to it, and only this flag, which parsing could not decide.
auto checker::settle_bounded_params(
    const std::vector<ast::ptr<ast::node>> &items) -> void {
  const auto settle = [&](const std::vector<ast::type_param> &params) -> void {
    for (const auto &param : params) {
      if (param.is_value_param && param.bound_or_type != nullptr &&
          names_trait_or_concept(*param.bound_or_type)) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        const_cast<ast::type_param &>(param).is_value_param = false;
      }
    }
  };
  for (const auto &item : items) {
    if (item == nullptr) {
      continue;
    }
    switch (item->kind) {
    case ast::node_kind::func_decl:
      settle(dynamic_cast<const ast::func_decl &>(*item).type_params);
      break;
    case ast::node_kind::impl_decl: {
      const auto &impl = dynamic_cast<const ast::impl_decl &>(*item);
      settle(impl.type_params);
      settle_bounded_params(impl.items);
      break;
    }
    case ast::node_kind::extend_decl: {
      const auto &extend = dynamic_cast<const ast::extend_decl &>(*item);
      settle(extend.type_params);
      settle_bounded_params(extend.items);
      break;
    }
    case ast::node_kind::trait_decl: {
      const auto &trait = dynamic_cast<const ast::trait_decl &>(*item);
      settle(trait.type_params);
      settle_bounded_params(trait.items);
      break;
    }
    case ast::node_kind::type_decl:
      settle(dynamic_cast<const ast::type_decl &>(*item).type_params);
      break;
    case ast::node_kind::sub_module_decl:
      settle_bounded_params(
          dynamic_cast<const ast::sub_module_decl &>(*item).items);
      break;
    default:
      break;
    }
  }
}

auto checker::run_impl(const std::vector<parsed_module> &inputs) -> void {
  for (const auto &input : inputs) {
    if (input.ast_file != nullptr) {
      session_.file_module_aliases_.emplace(
          input.file_id, collect_module_aliases(*input.ast_file));
    }
  }
  // Eagerly, before anything below can take a `const type_entry &`
  // reference into `type_table` and hold it across an expression: `find_
  // method`'s own lazy call to this (guarded by `cache_.methods_built_`, so it
  // only actually runs once) interns a type for every cloned trait-
  // default body it monomorphizes, which can reallocate `type_table`'s
  // backing storage. A caller like `infer_method_call` that resolved
  // `entry` just before calling `find_method` — the very first call
  // anywhere in the session, if this weren't forced early — would have
  // that reference silently dangle for the rest of its own scope once
  // `find_method` returns (see `target_type_name` ending up empty
  // instead of a real struct name, for exactly this reason).
  for (const auto &input : inputs) {
    if (input.ast_file == nullptr || input.ast_file->module_decl == nullptr ||
        input.ast_file->module_decl->has_error ||
        input.ast_file->module_decl->path.empty()) {
      continue;
    }
    if (static_cast<size_t>(input.file_id) < file_has_errors_.size() &&
        file_has_errors_[input.file_id]) {
      continue;
    }
    register_comptime_globals(input.ast_file->items);
  }

  // Resolve item-level splices before the method table/coherence build
  // below, so any injected impl participates exactly like one written
  // directly in source — see `resolve_item_splices`'s doc comment.
  for (const auto &input : inputs) {
    if (input.ast_file == nullptr || input.ast_file->module_decl == nullptr ||
        input.ast_file->module_decl->has_error ||
        input.ast_file->module_decl->path.empty()) {
      continue;
    }
    if (static_cast<size_t>(input.file_id) < file_has_errors_.size() &&
        file_has_errors_[input.file_id]) {
      continue;
    }
    file_.file_id_ = input.file_id;
    file_.module_name_ = join_strings(input.ast_file->module_decl->path, ".");
    file_.module_ = index_.find_module(file_.module_name_);
    file_.file_no_prelude_ = input.ast_file->no_prelude;
    compute_external_wildcard();
    push_scope();
    resolve_item_splices(input.ast_file->items, input.file_id);
    pop_scope();
  }

  // `[T: ord]` and `[n: usize]` parse alike, so the parser marks both as
  // value parameters. What the name after the colon resolves to decides —
  // a trait or a concept makes it a bounded type parameter — and it is
  // decided once, here, before anything reads the flag: a caller can reach
  // a callee's parameters long before the callee itself is checked.
  for (const auto &input : inputs) {
    if (input.ast_file == nullptr || input.ast_file->module_decl == nullptr ||
        input.ast_file->module_decl->has_error ||
        input.ast_file->module_decl->path.empty()) {
      continue;
    }
    file_.file_id_ = input.file_id;
    file_.module_name_ = join_strings(input.ast_file->module_decl->path, ".");
    file_.module_ = index_.find_module(file_.module_name_);
    file_.file_no_prelude_ = input.ast_file->no_prelude;
    compute_external_wildcard();
    settle_bounded_params(input.ast_file->items);
  }

  // Materialize every functor instantiation (`use m[args]`) before the
  // method table and coherence build below, so any `impl`/`extend` a functor
  // body declares participates in both — exactly like an item-level splice.
  // Bodies are only *registered* here; they are *checked* by
  // `check_pending_functor_bodies` after the table is complete.
  for (const auto &input : inputs) {
    if (input.ast_file == nullptr || input.ast_file->module_decl == nullptr ||
        input.ast_file->module_decl->has_error ||
        input.ast_file->module_decl->path.empty()) {
      continue;
    }
    if (static_cast<size_t>(input.file_id) < file_has_errors_.size() &&
        file_has_errors_[input.file_id]) {
      continue;
    }
    file_.file_id_ = input.file_id;
    file_.module_name_ = join_strings(input.ast_file->module_decl->path, ".");
    file_.module_ = index_.find_module(file_.module_name_);
    file_.file_no_prelude_ = input.ast_file->no_prelude;
    compute_external_wildcard();
    push_scope();
    discover_functor_instantiations(input.ast_file->items);
    pop_scope();
  }

  // Register every module's reflectable surface (now including materialized
  // functor instantiations) so compile-time `M.functions()`/`M.types()`/
  // `M.name()` reflection resolves — see `register_comptime_modules`.
  register_comptime_modules();

  build_method_table();
  validate_impl_coherence();
  // Reads `traits_by_type_key_`, so must run after `validate_impl_
  // coherence` above and, like `register_comptime_modules`, before any
  // file's body (and therefore any `static`/comptime call) is checked.
  register_comptime_coherence_info();
  check_pending_functor_bodies();

  for (const auto &input : inputs) {
    if (input.ast_file == nullptr || input.ast_file->module_decl == nullptr ||
        input.ast_file->module_decl->has_error ||
        input.ast_file->module_decl->path.empty()) {
      continue;
    }
    if (static_cast<size_t>(input.file_id) < file_has_errors_.size() &&
        file_has_errors_[input.file_id]) {
      continue;
    }
    file_.file_id_ = input.file_id;
    file_.module_name_ = join_strings(input.ast_file->module_decl->path, ".");
    file_.module_ = index_.find_module(file_.module_name_);
    file_.file_no_prelude_ = input.ast_file->no_prelude;
    check_file(*input.ast_file);
    // Per file rather than once at the end. The instances this file asked
    // for are checked before the next file starts, which keeps a
    // monomorphization diagnostic next to the file that caused it instead
    // of at the end of the session — the ordering users read messages in,
    // and the one the golden corpus records.
    flush_deferred();
  }

  // Anything requested outside a file's own walk — a functor body, an
  // item-level splice — has nothing left to flush it.
  flush_deferred();
  flush_unchecked_templates();
  flush_deferred();
  finish_open_results(/*last=*/true);
  finish_pending_interp_segments();
}

} // namespace detail

using detail::checker;

/// Out-of-line definition of the public entry point declared on `checker`;
/// forwards to `run_impl`.
auto checker::run(const std::vector<parsed_module> &inputs) -> void {
  run_impl(inputs);
}

/// Builds the session-wide `program_index` once, then runs one `checker`
/// instance over every input file.
auto check_program(const std::vector<parsed_module> &inputs,
                   diagnostic_bag &diag, std::vector<bool> &file_has_errors)
    -> checked_types {
  const auto session_index = build_module_session_index(inputs);
  const auto semantic_index = build_semantic_resolution_index(inputs);
  return check_program(inputs, session_index, semantic_index, diag,
                       file_has_errors);
}

auto check_program(const std::vector<parsed_module> &inputs,
                   const module_session_index &session_index,
                   const semantic_resolution_index &semantic_index,
                   diagnostic_bag &diag, std::vector<bool> &file_has_errors)
    -> checked_types {
  // Not `const`: `checker::resolve_item_splices` mutates this in place to
  // graft splice-synthesized impls in before method-table/coherence build —
  // see `checker::index_`'s doc comment.
  auto index = build_program_index(inputs);
  auto session_checker = checker(index, diag, file_has_errors);
  session_checker.set_module_graph(session_index, semantic_index);
  session_checker.run(inputs);
  return session_checker.take_checked_types();
}

} // namespace cinder::semantic
