#include "checker.h"

namespace cinder::semantic::detail {

// ==========================================================================
//  Diagnostics helpers
// ==========================================================================

/// Marks the file currently being checked (`file_id_`) as containing an
/// error, so later driver phases skip emitting metadata for it.
auto checker::mark_error() -> void {
  // Held with the diagnostics it belongs to: `finish_param_probes` marks
  // the file if it replays them, and a discarded probe must not fail it.
  if (probe_capture_ != nullptr) {
    return;
  }
  if (static_cast<size_t>(file_id_) < file_has_errors_.size()) {
    file_has_errors_[file_id_] = true;
  }
}

/// Emits `diag`, unless the same complaint about the same source has
/// already been made.
///
/// One piece of source can be checked several times: a const-generic
/// template's body is checked once as itself and again inside every
/// instance made of it (`instantiate_generic_function`), and a trait-default
/// body once per implementing type (`monomorphize_trait_default`). A
/// mistake in that source is *one* mistake — reporting it once per copy
/// would tell the user, three times over, about a line they wrote once.
/// Anything an instance discovers that the template could not (a contract
/// refuted at `n == 3`, an index provably out of bounds) reads differently
/// and still comes through.
auto checker::emit_diag(const diagnostic &diag) -> void {
  if (probe_capture_ != nullptr) {
    probe_capture_->push_back(diag);
    return;
  }
  auto key = std::format("{}|{}|{}", static_cast<int>(diag.level), diag.file_id,
                         diag.message);
  for (const auto &label : diag.labels) {
    key += std::format("|{}:{}", label.span.start, label.span.end);
  }
  if (!emitted_diagnostics_.insert(std::move(key)).second) {
    return;
  }
  if (diag.level == diagnostic_level::error) {
    ++errors_emitted_;
  }
  diag_.emit(with_instantiation_notes(diag));
}

/// Appends the §5.4 instantiation-site notes to an error raised while
/// checking a monomorphized body.
///
/// A generic instance is compiled because some call asked for it, and the
/// user wrote that call — not the template. Without these notes a failure
/// inside `collect`'s body reads as an error in the standard library at a
/// line the user has never seen, and the type the parameter was solved to
/// appears to come from nowhere at all. Innermost frame first, so the
/// nearest call is the one read first.
auto checker::with_instantiation_notes(const diagnostic &diag) -> diagnostic {
  if (instantiation_sites_.empty() || diag.level != diagnostic_level::error) {
    return diag;
  }
  auto annotated = diag;
  for (const auto &frame : std::views::reverse(instantiation_sites_)) {
    if (frame.lambda_use) {
      annotated.children.push_back(
          diagnostic(diagnostic_level::note,
                     std::format("found in the copy of `{}` checked for "
                                 "this use",
                                 frame.instance_name),
                     frame.call_file)
              .with_label(frame.call_span,
                          std::format("this use of `{}` needs that copy",
                                      frame.instance_name)));
      continue;
    }
    annotated.children.push_back(
        diagnostic(
            diagnostic_level::note,
            std::format("instantiated from here, as `{}`", frame.instance_name),
            frame.call_file)
            .with_label(frame.call_span, "this call needs that copy"));
    for (const auto &solution : frame.context_solutions) {
      annotated.children.emplace_back(diagnostic_level::note, solution,
                                      frame.call_file);
    }
  }
  return annotated;
}

/// Emits a single-label error at `span` in the current file.
auto checker::error(source_span span, const std::string &message,
                    const std::string &label) -> void {
  auto diag = diagnostic(diagnostic_level::error, message, file_id_);
  diag.with_label(span, label);
  emit_diag(diag);
  mark_error();
}

/// Emits a single-label error with an attached help suggestion.
auto checker::error_with_help(source_span span, const std::string &message,
                              const std::string &label, const std::string &help)
    -> void {
  auto diag = diagnostic(diagnostic_level::error, message, file_id_);
  diag.with_label(span, label);
  diag.with_help(help);
  emit_diag(diag);
  mark_error();
}

/// Explains, in the user's own arithmetic, *why* two types with the same
/// shape still don't match — because somewhere inside them a compile-time
/// value slot cannot line up. Without this, calling `head` on an empty
/// `vec` reports a bare "expected `vec[T, n + 1]`, found `vec[int32, 0]`"
/// and leaves the reader to work out that the lengths are the problem, and
/// then that no `n: usize` could ever close the gap. Returns `nullopt` when
/// the mismatch isn't about a value slot, which is the common case.
auto checker::value_slot_conflict(type_id expected, type_id found)
    -> std::optional<std::string> {
  const auto &expected_entry = types_.entry(expected);
  const auto &found_entry = types_.entry(found);

  if (is_value_kind(expected_entry.kind) && is_value_kind(found_entry.kind)) {
    if (types_.compatible(expected, found)) {
      return std::nullopt;
    }
    if (expected_entry.kind == type_kind::const_variant_kind ||
        found_entry.kind == type_kind::const_variant_kind) {
      return std::format(
          "This value is in state `{}`, but state `{}` is required here — "
          "and a state is a fact about the value, not a conversion you can "
          "perform. Route the value through whatever operation produces "
          "state `{}`.",
          found_entry.name, expected_entry.name, expected_entry.name);
    }
    auto unknowns = std::vector<std::string>{};
    for (const auto &term : expected_entry.value.terms) {
      unknowns.push_back(poly_var_spelling(term.var));
    }
    for (const auto &term : found_entry.value.terms) {
      unknowns.push_back(poly_var_spelling(term.var));
    }
    if (unknowns.empty()) {
      return std::nullopt; // two constants; the displayed types say it all
    }
    std::ranges::sort(unknowns);
    unknowns.erase(std::ranges::unique(unknowns).begin(), unknowns.end());
    const auto domain = types_.display(is_value_kind(expected_entry.kind) &&
                                               expected_entry.result != 0
                                           ? expected_entry.result
                                           : found_entry.result);
    return std::format(
        "`{}` can never equal `{}` for `{}: {}` — there is no value of `{}` "
        "that would make these lengths agree.",
        expected_entry.name, found_entry.name, join_strings(unknowns, "`, `"),
        domain, join_strings(unknowns, "`, `"));
  }

  if (expected_entry.kind != found_entry.kind) {
    return std::nullopt;
  }
  for (size_t i = 0;
       i < expected_entry.args.size() && i < found_entry.args.size(); ++i) {
    if (auto conflict =
            value_slot_conflict(expected_entry.args[i], found_entry.args[i])) {
      return conflict;
    }
  }
  if (expected_entry.result != found_entry.result) {
    return value_slot_conflict(expected_entry.result, found_entry.result);
  }
  return std::nullopt;
}

/// Emits a "type mismatch" error if `found` is not `compatible` with
/// `expected`; a no-op otherwise. Adds a conversion hint when both sides
/// are numeric, since Cinder never converts numbers implicitly.
///
/// `value`, where a caller has the expression to hand, is what makes this
/// the single chokepoint for *coercion* rather than just for shape: a value
/// flowing into a refined type owes a proof, and this is every place a
/// value flows into a declared type (argument, initializer, assignment,
/// field, element, return). Passing `nullptr` simply forgoes that check —
/// used where there is no expression, such as a pattern's implied type.
/// `id` with every solved leaf unknown substituted in.
///
/// Guarded on there being any, because zonking rebuilds a type through the
/// table's constructors, and a session that has minted no leaves must not
/// pay that — nor risk it — on every type the checker looks at.
auto checker::settle(type_id id) -> type_id {
  return leaf_ctxt_.meta_count() == 0 ? id : leaf_ctxt_.zonk(id);
}

/// Whether every type parameter `id` mentions is one the body currently
/// being checked has in scope.
///
/// `type_params_` is that scope stack, so this is a lookup rather than a
/// guess: the `T` of the function being checked is in it, and the `T` of a
/// callee whose signature is merely being matched against is not.
auto checker::type_params_in_scope(type_id id) -> bool {
  auto seen = std::unordered_set<type_id>{};
  return type_params_in_scope_impl(id, seen);
}

auto checker::type_params_in_scope_impl(type_id id,
                                        std::unordered_set<type_id> &seen)
    -> bool {
  if (!seen.insert(id).second) {
    return true;
  }
  const auto &entry = types_.entry(id);
  if (entry.kind == type_kind::type_param_kind &&
      !std::ranges::any_of(type_params_, [id](const auto &scope) -> bool {
        return std::ranges::any_of(scope, [id](const auto &pair) -> bool {
          return pair.second == id;
        });
      })) {
    return false;
  }
  for (const auto arg : entry.args) {
    if (!type_params_in_scope_impl(arg, seen)) {
      return false;
    }
  }
  return entry.result == id || type_params_in_scope_impl(entry.result, seen);
}

/// Runs the obligation queue, spending the last resort only on what
/// `roots` depend on.
///
/// A point of demand owes a default to the leaves it is about to decide
/// on, not to every literal in the program: a global flush here answered
/// `a` in `let a = 1; println("{b}"); let c: int64 = a` before the
/// annotation that pins it.
///
/// "Depends on" is the leaves `roots` mention, plus whatever a stalled
/// decision watches, when that decision may solve a leaf already in scope:
/// a pending method call on `?a` may be the only thing that can solve the
/// `?b` being asked about, and it cannot run until `?a` is known. A
/// decision that solves nothing in scope is not waited on: an index or
/// loop over `[1, 2]` only names its instance once the elements settle, so
/// demanding an unrelated literal must not default them before `let c:
/// int64 = xs[0]` says what they are. A decision that does not record what
/// it binds is followed whole.
///
/// Followed to a fixpoint, since a decision can bring in a leaf another
/// is waiting on. Recomputed each time a default is considered,
/// since the previous one may have solved or merged what this one is
/// asking about.
///
/// `answered`, if given, says the demand has what it needs: nothing more
/// is defaulted once it holds.
auto checker::flush_for(std::span<const type_id> roots,
                        const std::function<bool()> &answered) -> void {
  const auto in_scope = [&](type_id leaf) -> bool {
    if (answered && answered()) {
      return false;
    }
    auto reps = std::unordered_set<type_id>{};
    const auto add = [&](type_id type) -> bool {
      auto vars = std::vector<type_id>{};
      auto seen = std::unordered_set<type_id>{};
      collect_type_vars(leaf_ctxt_.zonk(type), vars, seen);
      auto grew = false;
      for (const auto var : vars) {
        grew = reps.insert(leaf_ctxt_.find(var)).second || grew;
      }
      return grew;
    };
    const auto touches = [&](type_id type) -> bool {
      auto vars = std::vector<type_id>{};
      auto seen = std::unordered_set<type_id>{};
      collect_type_vars(leaf_ctxt_.zonk(type), vars, seen);
      return std::ranges::any_of(vars, [&](type_id var) -> bool {
        return reps.contains(leaf_ctxt_.find(var));
      });
    };
    for (const auto root : roots) {
      (void)add(root);
    }
    auto decisions = std::vector<infer::obligation_id>{};
    for (const auto id : leaf_queue_.stalled()) {
      if (leaf_queue_.at(id).kind != infer::obligation_kind::defaulting) {
        decisions.push_back(id);
      }
    }
    auto grew = true;
    while (grew) {
      grew = false;
      for (const auto id : decisions) {
        const auto &goal = leaf_queue_.at(id);
        if (goal.binds.has_value() &&
            !std::ranges::any_of(*goal.binds, touches)) {
          continue;
        }
        for (const auto watch : goal.watches) {
          grew = add(watch) || grew;
        }
      }
    }
    return reps.contains(leaf_ctxt_.find(leaf));
  };
  (void)leaf_queue_.flush(in_scope);
}

/// The type `id` has, at a point where it has to be a type.
///
/// Zonk first, so a leaf a real constraint already solved reads as that
/// answer. Only if one is *still* open is the queue run, and defaulting
/// inside it is attempted only once every other obligation has stalled,
/// one candidate at a time — so this is a last resort being spent, not a
/// guess being made.
///
/// Where to call it is the whole design. Not at the leaf, which is what
/// the old inline `int32` did and why it was wrong so often; at each point
/// the checker genuinely cannot proceed without a type — solving a call's
/// generic parameters, resolving a method on a receiver — and, at a call,
/// only *after* the arguments have been checked, since checking them is
/// what usually supplies the answer.
auto checker::demand(type_id id) -> type_id {
  if (leaf_ctxt_.meta_count() == 0) {
    return id;
  }
  const auto settled = leaf_ctxt_.zonk(id);
  auto seen = std::unordered_set<type_id>{};
  if (!mentions_type_var(settled, seen)) {
    return settled;
  }
  return demand_until(settled, nullptr);
}

/// `demand`'s last resort, spent only until `answered` holds.
auto checker::demand_until(type_id settled,
                           const std::function<bool()> &answered) -> type_id {
  // A probed function's body may still be sitting in
  // `unclassified_param_decls_`, its open-parameter verdict not yet
  // recorded: `flush_leaf_literals`/`flush_pending_instances` normally
  // classify it first (via `flush_deferred`), so its deferred obligations
  // (an operator dispatch on the still-open parameter, say) are marked
  // no-op before anything can default them. A `demand()` reached from
  // outside that sequence — e.g. `check_interpolated_string` needing an
  // interpolation segment's type right now — used to flush the queue
  // without that guard, so `1 + g(5)` (for `def g(x): x + 1`) permanently
  // defaulted `x` to `int32` while classifying `g`'s deferred `+`
  // obligation, turning `g` into an ordinary `int32` function instead of
  // the implicit generic its body actually makes it, and lowering then
  // rejected the never-instantiated template as "no declared return type,
  // and none could be inferred from its body" (spec/todo.md item 19).
  //
  // Resolving the calls to what was just classified states each call's
  // result in terms of its arguments (`link_open_param_call`), so a
  // demand on `1 + add_one(5)`'s right operand reaches the `5`.
  classify_param_decls();
  resolve_open_param_calls();
  flush_for(std::span{&settled, 1}, answered);
  return leaf_ctxt_.zonk(settled);
}

/// `demand`, for a point that selects on the head constructor alone.
///
/// Indexing, iterating and destructuring choose a route from *what kind*
/// of value this is — a `list`, a tuple — and never from its elements, so
/// `[1, 2]`'s element leaves are not owed their default here: a later
/// `let y: int64 = xs[0]` is what says what they are (phase 11, defect 3).
/// Only a head that is itself still a variable has no route to choose, and
/// that is owed the last resort, but only until the head is known: the
/// call that answers `let p = pair(1, 2147483647)` with a tuple leaves
/// `p.1`'s element to a later `let s: int64 = p.1`.
auto checker::demand_shape(type_id id) -> type_id {
  if (leaf_ctxt_.meta_count() == 0) {
    return id;
  }
  const auto settled = leaf_ctxt_.zonk(id);
  const auto head_open = [&]() -> bool {
    return types_.entry(base_shape(leaf_ctxt_.zonk(settled))).kind ==
           type_kind::type_var_kind;
  };
  if (head_open()) {
    return demand_until(settled, [&]() -> bool { return !head_open(); });
  }
  return settled;
}

/// Teaches the leaf unknowns what a value flowing into a declared type says
/// about them.
///
/// `type_mismatch` is the single place a value meets a declared type —
/// argument, initializer, assignment, field, element, return — so it is the
/// one place this has to be, and the reason phase 8 needs no second walk.
/// Solving here rather than at each of those sites is also why an empty
/// `[]` can be pinned by a `push` that is a hundred lines away.
///
/// A failure is deliberately ignored: the mismatch about to be reported
/// below is the better message, and the unifier's own wording would be a
/// second diagnostic for one mistake.
auto checker::solve_leaves(type_id expected, type_id found) -> void {
  if (leaf_ctxt_.meta_count() == 0) {
    return;
  }
  // Only when a leaf is actually involved. Running the unifier on every
  // pair would also solve *value* parameters, and those are keyed by name
  // in the store: `at[n: usize]` called at `n = 3` and then at `n = 5`
  // would have the second call measured against the first's answer. Value
  // slots have their own path and are not phase 8's business.
  auto seen = std::unordered_set<type_id>{};
  if (!mentions_type_var(expected, seen)) {
    seen.clear();
    if (!mentions_type_var(found, seen)) {
      return;
    }
  }
  // A declared type mentioning a type parameter that is *not in scope
  // here* is not a declared type at all — it is the callee's pattern, and
  // the very thing the call is trying to solve. Binding a leaf to it
  // captures the parameter, and the call then answers "this value's type
  // is `T`" followed by "`T` is unsolved": the checker blaming the call
  // for a circle it drew itself.
  //
  // Scope is the whole distinction, and it is a fact rather than a mode.
  // Inside `from_iter`'s own body `T` *is* in scope — rigid there, and
  // `return out` against `-> list[T]` is exactly how that leaf is meant to
  // solve. At a call site in another function it is not.
  if (!type_params_in_scope(expected) || !type_params_in_scope(found)) {
    return;
  }
  // An integer literal can only be a number. Handed a concrete type that
  // is not one, it is not evidence about the literal — it is the mistake
  // about to be reported — so the literal takes its default now and the
  // ordinary mismatch says what is wrong (`let s: str = 10`).
  const auto refuses_number = [this](type_id literal, type_id other) -> bool {
    const auto root = leaf_ctxt_.find(literal);
    if (!integer_literal_leaves_.contains(root)) {
      return false;
    }
    const auto settled = settle(other);
    auto vars = std::unordered_set<type_id>{};
    if (mentions_type_var(settled, vars) || types_.is_unknown(settled)) {
      return false;
    }
    const auto shape = strip_refs(settled);
    return !types_.is_numeric(shape) &&
           types_.entry(shape).kind != type_kind::type_param_kind;
  };
  if (refuses_number(expected, found)) {
    (void)demand(expected);
    return;
  }
  if (refuses_number(found, expected)) {
    (void)demand(found);
    return;
  }
  (void)leaf_engine_.unify(expected, found, infer::k_no_cause);
}

/// `compatible`, but teaching the leaf unknowns first.
///
/// `type_mismatch` is the chokepoint for a value meeting a declared type,
/// but several callers ask `compatible` themselves and only reach it when
/// the answer is no — and `list[?a]` is *compatible* with everything, so
/// those paths would never solve `?a`. `return out` in
/// `std.iter::from_iter` is exactly one: it is the only thing in that
/// function that says what `out` holds.
auto checker::agrees(type_id expected, type_id found) -> bool {
  solve_leaves(expected, found);
  return types_.compatible(settle(expected), settle(found));
}

/// Whether `id` is a borrow of something a load has to be spelled for —
/// `&int32`, not `&list[T]`. The one shape where dropping the `&` silently
/// changes what the code computes.
auto checker::needs_explicit_deref(type_id id) -> bool {
  const auto &entry = types_.entry(settle(id));
  return entry.kind == type_kind::ref_kind &&
         !types_.reference_is_transparent(entry.result);
}

/// Reports the missing `*` when that is what a mismatch really is, and
/// says whether it did.
///
/// Called ahead of the generic "expected X, found Y" at each site rather
/// than folded into one, because the sites do not share a diagnostic —
/// a returned value, an argument and an operand each phrase their own —
/// and what they *do* share is this one question.
auto checker::report_deref_if_missing(const ast::expr *value, type_id expected,
                                      type_id found) -> bool {
  if (value == nullptr || !needs_explicit_deref(found)) {
    return false;
  }
  // Only when the `*` is the whole story. A `&int32` where a `str` was
  // wanted is a plain mismatch, and telling the user to dereference it
  // would send them somewhere there is nothing to find.
  const auto referent = types_.entry(settle(found)).result;
  if (!types_.compatible(settle(expected), referent)) {
    return false;
  }
  report_missing_deref(*value, found);
  return true;
}

/// Rejects an operator operand that is a `&int32` (see
/// `needs_explicit_deref`), now or once its type settles: `(x) => x > 2`
/// passed where a `fn(&int32) -> bool` is expected only learns that `x`
/// is a reference after its body was checked. False when it reported.
auto checker::require_explicit_deref(const ast::expr &operand, type_id raw)
    -> bool {
  const auto settled = settle(raw);
  if (needs_explicit_deref(settled)) {
    report_missing_deref(operand, settled);
    return false;
  }
  if (mentions_type_var(settled)) {
    defer_method_call("the operand", settled, std::vector<type_id>{},
                      [this, &operand](type_id now) -> void {
                        (void)require_explicit_deref(operand, now);
                      });
  }
  return true;
}

/// The missing `*`. Named as the fix rather than as the mismatch, because
/// "expected `int32`, found `&int32`" is a true sentence that teaches
/// nothing: the two spellings differ by one character and the user has to
/// be told which one and why.
auto checker::report_missing_deref(const ast::expr &value, type_id found)
    -> void {
  const auto settled = settle(found);
  const auto referent = types_.entry(settled).result;
  error_with_help(
      value.span,
      std::format("expected `{}`, found `{}` — this is a borrow, not a "
                  "number",
                  types_.display(referent), types_.display(settled)),
      std::format("this is a `{}`", types_.display(settled)),
      std::format(
          "A borrow of a list, string, struct or tuple can be used as the "
          "value directly — such a value already *is* an address, so the "
          "`&` costs nothing. A value of type `{}` is not: `&` on one "
          "makes an address, and reading the value back out through it is "
          "a load. Write `*` in front of this to do that load.",
          types_.display(referent)));
}

auto checker::type_mismatch(source_span span, type_id expected, type_id found,
                            std::string_view context, const ast::expr *value)
    -> void {
  solve_leaves(expected, found);
  expected = settle(expected);
  found = settle(found);
  if (types_.compatible(expected, found)) {
    check_narrowing(expected, found, value, span, context);
    return;
  }
  if (report_deref_if_missing(value, expected, found)) {
    return;
  }
  auto diag =
      diagnostic(diagnostic_level::error,
                 std::format("type mismatch: expected `{}`, found `{}`",
                             types_.display(expected), types_.display(found)),
                 file_id_);
  diag.with_label(
      span, std::format("expected `{}` {}", types_.display(expected), context));
  if (auto conflict = value_slot_conflict(expected, found)) {
    diag.with_help(*conflict);
  } else if (types_.is_numeric(expected) && types_.is_numeric(found)) {
    diag.with_help(std::format(" Cinder never converts numbers implicitly; "
                               "write `{}(...)` to convert "
                               "this value explicitly.",
                               types_.display(expected)));
  }
  emit_diag(diag);
  mark_error();
}

/// Marks the file currently being checked as containing an error, guarding
/// the index bound.
auto checker::mark_file_error() -> void {
  if (static_cast<size_t>(file_id_) < file_has_errors_.size()) {
    file_has_errors_[file_id_] = true;
  }
}

} // namespace cinder::semantic::detail
