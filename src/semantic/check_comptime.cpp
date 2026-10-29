#include "checker.h"
#include "src/parser/ast_traversal.h"

namespace cinder::semantic::detail {

/// Resolves a `static if`'s condition to a real branch selection when
/// possible: `std::nullopt` means "couldn't decide, check both branches",
/// `true`/`false` means "only this branch is real, check just that one".
///
/// Deliberately skips evaluation entirely while any type parameter still
/// in scope is unbound (an abstract `type_param_kind`, as opposed to one
/// `push_type_params` bound to a concrete type from a monomorphization
/// instance): a condition like `T.name() == "int64"` reaches the
/// evaluator's general call path (rather than the checker's own
/// `T.name()` fold, which only fires for a fully-concrete `T`) and would
/// otherwise report a spurious "only direct calls to a named `static def`
/// function" error. The real branch selection for that case happens per
/// call, when `evaluator::evaluate_stmt` runs this same `static_decl` with
/// `T` concretely bound. A generic scope whose parameters are *all*
/// already bound (e.g. this `static if` sits inside a locally-defined
/// generic function currently being checked for one concrete
/// instantiation) must still narrow, so the check looks at each in-scope
/// parameter's actual type rather than just whether any generic scope is
/// active.
/// Whether any type parameter currently in scope is still abstract (an
/// unbound `type_param_kind`, as opposed to one `push_type_params` bound to
/// a concrete type from a monomorphization instance) — i.e. whether the
/// function body being checked right now is the un-instantiated generic
/// template pass rather than a concretely-bound instantiation. Shared by
/// `resolve_static_if_branch` (a `static if` condition mentioning an
/// abstract type parameter can't be folded yet) and `check_static_decl`'s
/// `assertion` case (a `static assert` anywhere in a still-abstract body —
/// nested in an unresolved `static if` branch or not — has exactly the
/// same problem: nothing about it is known to be reachable, or even
/// meaningful, until the real per-call-site instantiation checks it with
/// concrete types bound).
namespace {

/// A binding's place, as the checker's binding sets key it.
auto span_key(source_span span) -> std::pair<std::size_t, std::size_t> {
  return {span.start, span.end};
}

} // namespace

auto checker::inside_unbound_generic_scope() -> bool {
  return std::ranges::any_of(
      function_.type_params_, [this](const auto &scope) -> bool {
        return std::ranges::any_of(scope, [this](const auto &param) -> bool {
          return session_.types_.entry(param.second).kind ==
                 type_kind::type_param_kind;
        });
      });
}

/// The bindings of each iteration of an inline `static for` that its guard
/// keeps, evaluated at compile time. `nullopt` when the iterable or the
/// guard can't be computed; that has been reported, and the file marked as
/// failing — unless `silent`, when nothing is reported.
auto checker::evaluate_static_for_inline(const ast::static_decl &decl,
                                         bool silent)
    -> std::optional<std::vector<static_for_scope>> {
  if (silent) {
    const auto saved = comptime_eval_.set_silent(true);
    const auto saved_errors = file_has_errors_;
    auto result = evaluate_static_for_inline(decl, false);
    file_has_errors_ = saved_errors;
    comptime_eval_.set_silent(saved);
    return result;
  }
  const auto inline_form = decl.decl_kind == ast::static_decl_kind::for_inline;
  if (decl.for_iterable == nullptr || decl.for_iterable->has_error ||
      (inline_form && decl.for_yield == nullptr) ||
      decl.for_patterns.size() != 1 || decl.for_patterns[0] == nullptr) {
    // Already reported by the parser, or (several loop patterns) below.
    if (inline_form && decl.for_patterns.size() > 1) {
      error_with_help(decl.span,
                      "an inline `static for` takes one loop variable",
                      "this loop names several",
                      "Bind one variable and take it apart in the yielded "
                      "expression.");
    }
    mark_error();
    return std::nullopt;
  }
  const auto failed = [this]() -> std::optional<std::vector<static_for_scope>> {
    mark_error();
    return std::nullopt;
  };
  const auto list_value = comptime_eval_.evaluate_iterable(*decl.for_iterable);
  if (list_value.is_error()) {
    return failed();
  }
  auto iterations = std::vector<static_for_scope>{};
  for (const auto &element_value : list_value.elements) {
    auto scope = static_for_scope{};
    if (!comptime_eval_.bind_pattern(*decl.for_patterns[0], element_value,
                                     scope)) {
      return failed();
    }
    if (decl.for_guard != nullptr) {
      comptime_eval_.push_locals(scope);
      const auto guard = comptime_eval_.evaluate(*decl.for_guard);
      comptime_eval_.pop_locals();
      if (guard.is_error() || guard.kind != comptime::value_kind::boolean) {
        if (!guard.is_error()) {
          error(decl.for_guard->span,
                "a `static for` guard must be `true` or `false`",
                "this did not evaluate to a boolean");
        }
        return failed();
      }
      if (!guard.boolean) {
        continue;
      }
    }
    iterations.push_back(std::move(scope));
  }
  return iterations;
}

/// `T` when `type` is a `list[T]`, `k_unknown_type` otherwise.
auto checker::list_element_type(type_id type) -> type_id {
  const auto &entry = session_.types_.entry(strip_refs(settle(type)));
  if (entry.name != "list" || entry.args.size() != 1 ||
      entry.kind != type_kind::struct_kind) {
    return k_unknown_type;
  }
  return entry.args.front();
}

/// The type of a list's elements, read from compile-time values alone — for
/// a `static for` whose yield the checker could not type (`field.name`
/// reads a reflection descriptor, which has no static type).
auto checker::comptime_values_type(const std::vector<comptime::value> &values)
    -> type_id {
  auto result = k_unknown_type;
  for (const auto &value : values) {
    auto type = k_unknown_type;
    switch (value.kind) {
    case comptime::value_kind::string:
      type = session_.types_.builtin("str");
      break;
    case comptime::value_kind::boolean:
      type = session_.types_.builtin("bool");
      break;
    case comptime::value_kind::integer:
      type = session_.types_.builtin("int32");
      break;
    case comptime::value_kind::floating:
      type = session_.types_.builtin("float64");
      break;
    default:
      return k_unknown_type;
    }
    if (result != k_unknown_type && result != type) {
      return k_unknown_type;
    }
    result = type;
  }
  return result;
}

/// Types an inline `static for` in a function body, and unrolls it. A
/// template unrolls once per instance, with the instance's types bound.
auto checker::record_static_for_inline(const ast::static_decl &decl,
                                       type_id expected_tail) -> type_id {
  auto yield_type = k_unknown_type;
  if (const auto it = cache_.static_for_yield_types_.find(&decl);
      it != cache_.static_for_yield_types_.end()) {
    yield_type = it->second;
  }
  // A field descriptor exists only while compiling, so no list can hold
  // one. Refused here, once, before a `list[field_info]` is ever made.
  if (yield_type != k_unknown_type &&
      strip_refs(settle(yield_type)) == field_info_type()) {
    error_with_help(
        decl.for_yield->span,
        "an inline `static for` cannot build a list of `field_info`",
        "this is a field descriptor, which exists only while compiling",
        "Yield one of its fields instead, such as `f.name` or "
        "`f.type_name`.");
    return k_error_type;
  }
  if (inside_unbound_generic_scope()) {
    defer_to_instances(
        decl,
        [this, &decl, yield_type, expected_tail,
         file = file_.file_id_](instance_subst &subst) -> void {
          const auto saved_file = file_.file_id_;
          file_.file_id_ = file;
          const auto &clone = *clone_of(subst, &decl);
          if (auto iterations = evaluate_static_for_inline(clone)) {
            (void)unroll_static_for_inline(
                static_for_unrolling{
                    .decl = clone,
                    .written = decl,
                    .clone = [&subst](const ast::node *node)
                        -> const ast::node * { return clone_of(subst, node); },
                    .substitute = [this, &subst](type_id type) -> type_id {
                      return substitute_type(type, subst);
                    }},
                *iterations, substitute_type(yield_type, subst),
                substitute_type(expected_tail, subst));
          }
          file_.file_id_ = saved_file;
        });
    return yield_type == k_unknown_type ? k_unknown_type
                                        : resolve_list_type(yield_type);
  }
  const auto iterations = cache_.static_for_values_.find(&decl);
  if (iterations == cache_.static_for_values_.end()) {
    return k_unknown_type;
  }
  return unroll_static_for_inline(
      static_for_unrolling{
          .decl = decl,
          .written = decl,
          .clone = [](const ast::node *node) -> const ast::node * {
            return node;
          },
          .substitute = [](type_id type) -> type_id { return type; }},
      iterations->second, yield_type, expected_tail);
}

/// The type checking gave `node` of `unrolling.written`, as it is in
/// `unrolling.decl`.
auto checker::static_for_node_type(const static_for_unrolling &unrolling,
                                   const ast::node *node) -> type_id {
  if (node == nullptr) {
    return k_unknown_type;
  }
  if (const auto found = session_.node_types_.find(unrolling.clone(node));
      found != session_.node_types_.end()) {
    return found->second;
  }
  if (const auto found = session_.node_types_.find(node);
      found != session_.node_types_.end()) {
    return unrolling.substitute(found->second);
  }
  return k_unknown_type;
}

/// A compile-time value written into the program as a literal of `type`
/// (a struct literal of literals for a struct); `nullptr` when it has no
/// such form.
auto checker::materialize_static_value(const comptime::value &value,
                                       source_span span, type_id type)
    -> const ast::expr * {
  if (type == k_unknown_type || type == k_error_type) {
    return nullptr;
  }
  if (value.kind == comptime::value_kind::string) {
    return materialize_string_literal(value.string, span, type);
  }
  if (value.kind == comptime::value_kind::struct_instance) {
    auto built = build_static_struct_expr(value, span, type);
    const auto *raw = built.get();
    if (built != nullptr) {
      session_.synthesized_static_structs_.push_back(std::move(built));
    }
    return raw;
  }
  return materialize_const_literal(value, span, type);
}

/// Unrolls an inline `static for`. When its yield reads nothing from the
/// running program and every value it yields has a literal form, the list
/// is built from those values, computed now (`static_for_elements`).
/// Otherwise the yield is code that runs: one copy of it per iteration,
/// with that iteration's loop bindings in place (`static_for_iterations`).
auto checker::unroll_static_for_inline(
    const static_for_unrolling &unrolling,
    const std::vector<static_for_scope> &iterations, type_id yield_type,
    type_id expected_tail) -> type_id {
  const auto &decl = unrolling.decl;
  const auto &reads = cache_.static_for_reads_[&unrolling.written];
  const auto span = decl.for_yield->span;
  auto element = yield_type;
  if (element == k_unknown_type) {
    element = list_element_type(expected_tail);
  }

  // Computed now, when that is all it can mean.
  const static_for_scope *failed_scope = nullptr;
  if (!reads.runtime) {
    auto values = std::vector<comptime::value>{};
    for (const auto &scope : iterations) {
      comptime_eval_.push_locals(scope);
      auto value = comptime_eval_.try_evaluate(*decl.for_yield);
      comptime_eval_.pop_locals();
      if (!value.has_value()) {
        failed_scope = &scope;
        break;
      }
      values.push_back(std::move(*value));
    }
    const auto value_element =
        element == k_unknown_type && failed_scope == nullptr
            ? comptime_values_type(values)
            : element;
    if (failed_scope == nullptr && value_element != k_unknown_type) {
      auto literals = std::vector<const ast::expr *>{};
      for (const auto &value : values) {
        const auto *literal =
            materialize_static_value(value, span, value_element);
        if (literal == nullptr) {
          break;
        }
        literals.push_back(literal);
      }
      if (literals.size() == values.size()) {
        const auto list_type = resolve_list_type(value_element);
        cache_.static_for_elements_.insert_or_assign(&decl,
                                                     std::move(literals));
        record_new_push_dispatch(decl, list_type,
                                 cache_.static_for_dispatches_);
        return list_type;
      }
    }
  }

  if (element == k_unknown_type) {
    error_with_help(span,
                    "cannot tell what type of list this `static for` builds",
                    "the type of these values is not known",
                    "Say what the list holds where it is used, for example "
                    "with a return type such as `-> list[str]`.");
    return k_error_type;
  }
  // The yield reads nothing from the running program, so the evaluator's
  // own diagnosis of why it could not compute it is the real problem.
  const auto report_evaluation = [&]() -> bool {
    if (failed_scope == nullptr) {
      return false;
    }
    comptime_eval_.push_locals(*failed_scope);
    static_cast<void>(comptime_eval_.evaluate(*decl.for_yield));
    comptime_eval_.pop_locals();
    mark_error();
    return true;
  };

  const auto bindings = collect_pattern_bindings(*decl.for_patterns[0]);
  const auto written_bindings =
      collect_pattern_bindings(*unrolling.written.for_patterns[0]);
  auto unrolled = std::vector<static_for_iteration>{};
  unrolled.reserve(iterations.size());
  for (const auto &scope : iterations) {
    auto iteration = static_for_iteration{};
    auto compile_time_only = std::unordered_set<std::string>{};
    for (std::size_t i = 0; i < bindings.size(); ++i) {
      const auto &binding = bindings[i];
      const auto value = scope.find(binding.name);
      const auto *literal =
          value == scope.end() || i >= written_bindings.size()
              ? nullptr
              : materialize_static_value(
                    value->second, binding.span,
                    static_for_node_type(unrolling, written_bindings[i].node));
      if (literal == nullptr) {
        compile_time_only.insert(binding.name);
      } else {
        iteration.bindings.emplace_back(binding.name, literal);
      }
    }
    if (!compile_time_only.empty() &&
        !fold_static_for_reads(unrolling, reads, scope, compile_time_only,
                               iteration)) {
      if (!report_evaluation()) {
        mark_error();
      }
      return k_error_type;
    }
    unrolled.push_back(std::move(iteration));
  }
  const auto list_type = resolve_list_type(element);
  cache_.static_for_elements_.erase(&decl);
  cache_.static_for_iterations_.insert_or_assign(&decl, std::move(unrolled));
  record_new_push_dispatch(decl, list_type, cache_.static_for_dispatches_);
  return list_type;
}

/// Replaces each read in the yield rooted at a loop binding with no literal
/// form (`f.name`, for a `field_info` `f`) by the literal it evaluates to
/// in this iteration. False, with the problem reported, when such a binding
/// is used whole, or a read of it has no literal form either.
auto checker::fold_static_for_reads(
    const static_for_unrolling &unrolling, const static_for_reads &reads,
    const static_for_scope &scope,
    const std::unordered_set<std::string> &compile_time_only,
    static_for_iteration &iteration) -> bool {
  comptime_eval_.push_locals(scope);
  auto covered = std::unordered_set<const ast::expr *>{};
  auto ok = true;
  for (const auto *read : reads.loop_reads) {
    const auto *node = ast::as<const ast::expr>(unrolling.clone(read));
    const auto *root = node == nullptr ? nullptr : static_for_read_root(*node);
    if (root == nullptr || !compile_time_only.contains(*root)) {
      continue;
    }
    // A read inside one already folded (`f` in `f.name`) is gone with it.
    const auto inside_folded = covered.contains(read);
    if (read->kind == ast::node_kind::field_expr) {
      covered.insert(ast::cast<const ast::field_expr>(*read).object.get());
    }
    if (inside_folded) {
      continue;
    }
    if (read->kind == ast::node_kind::ident_expr) {
      error_with_help(
          node->span,
          std::format("this `static for` uses `{}` in code that runs, but "
                      "its value cannot be built into the program",
                      *root),
          std::format("`{}` is used whole here", *root),
          "Inside an inline `static for`, a loop variable is a constant "
          "written into each iteration's code, so it must be a number, a "
          "boolean, a string, or a struct of those. Use a part of it that "
          "is one, such as `f.name` of a field descriptor `f`.");
      ok = false;
      break;
    }
    const auto value = comptime_eval_.try_evaluate(*node);
    const auto *literal =
        value.has_value()
            ? materialize_static_value(*value, node->span,
                                       static_for_node_type(unrolling, read))
            : nullptr;
    if (literal == nullptr) {
      error_with_help(
          node->span,
          "this `static for` reads a value that cannot be built into the "
          "program",
          "this is computed while compiling",
          "A read of a compile-time loop variable used in code that runs "
          "must be a number, a boolean, a string, or a struct of those.");
      ok = false;
      break;
    }
    iteration.folded.emplace(node, literal);
  }
  comptime_eval_.pop_locals();
  return ok;
}

/// A string literal spelling `text`, typed `type`, owned like
/// `materialize_const_literal`'s.
auto checker::materialize_string_literal(const std::string &text,
                                         source_span span, type_id type)
    -> const ast::literal_expr * {
  auto lit = ast::make<ast::literal_expr>();
  lit->span = span;
  lit->lit_kind = token_kind::string_lit;
  lit->value = encode_string_literal(text);
  record_expr_type(*lit, type);
  const auto *raw = lit.get();
  session_.synthesized_const_literals_.push_back(std::move(lit));
  return raw;
}

/// A `static if` the template could not decide, decided by each instance:
/// the branch it does not take is dead there — never elaborated, and its
/// records dropped.
auto checker::defer_static_if(const ast::static_decl &decl) -> void {
  defer_to_instances(decl, [this, &decl](instance_subst &subst) -> void {
    const auto &clone = *clone_of(subst, &decl);
    if (clone.if_condition == nullptr) {
      return;
    }
    const auto evaluated = comptime_eval_.try_evaluate(*clone.if_condition);
    if (!evaluated.has_value() ||
        evaluated->kind != comptime::value_kind::boolean) {
      return;
    }
    const auto taken = evaluated->is_true();
    session_.static_if_taken_branch_.insert_or_assign(&clone, taken);
    const auto &dead = taken ? decl.else_body : decl.if_body;
    auto range = std::optional<source_span>{};
    for (const auto &item : dead) {
      if (item == nullptr || item->span.empty()) {
        continue;
      }
      range =
          range.has_value()
              ? source_span{.start = std::min(range->start, item->span.start),
                            .end = std::max(range->end, item->span.end)}
              : item->span;
    }
    if (range.has_value()) {
      subst.dead.push_back(*range);
    }
  });
}

auto checker::resolve_static_if_branch(const ast::static_decl &decl)
    -> std::optional<bool> {
  if (decl.if_condition == nullptr || decl.if_condition->has_error) {
    return std::nullopt;
  }
  require_bool(*decl.if_condition, "a `static if` condition");
  if (inside_unbound_generic_scope()) {
    return std::nullopt;
  }
  // `try_evaluate`, not `evaluate`: a condition can easily depend on
  // something the evaluator has no value for at this point in an ordinary
  // (non-comptime) check pass — an ordinary function-local `let` such as
  // `std.limits.min`'s `let n = T.name()`, rather than a `static let`
  // global or a direct `T.name()` call — and that is an expected "couldn't
  // decide yet", not a diagnosable error. `evaluate` would report one
  // anyway (its contract is to diagnose every failure), which would turn
  // every such ordinary fallback into a spurious compiler error.
  const auto evaluated = comptime_eval_.try_evaluate(*decl.if_condition);
  if (evaluated.has_value() &&
      evaluated->kind == comptime::value_kind::boolean) {
    return evaluated->is_true();
  }
  return std::nullopt;
}

/// Whether checking `node` as a body statement is guaranteed to have
/// already produced a `return` — used by `check_body_nodes` to stop
/// checking a block once it knows every later sibling is unreachable.
/// Deliberately narrow: only a `return_stmt`, or a `static if` whose
/// condition `resolve_static_if_branch` can actually resolve (every type
/// parameter in scope concretely bound) *and* whose one selected branch
/// itself definitely returns, counts. An ordinary runtime `if`/`match`, or
/// a `static if` that can't be resolved yet (the un-instantiated template
/// pass), is never treated as return-guaranteeing here — proving that in
/// general needs real control-flow analysis this compiler doesn't have.
/// This exists only to remove a false positive the checker's own
/// per-branch comptime narrowing introduces: a chain of independent
/// `static if cond: return ...` statements (one per possible concrete
/// type, `std.limits.max`'s shape) with a trailing catch-all `static
/// assert`/`return` is, for any one concrete instantiation, only ever
/// reached past whichever single `static if` actually matched — the
/// others' bodies are skipped already (empty `else_body`), but without
/// this, the *trailing* statement was still checked as if every one of
/// those earlier `return`s could fall through.
auto checker::stmt_definitely_returns(const ast::node &node) -> bool {
  switch (node.kind) {
  case ast::node_kind::return_stmt:
    return true;
  case ast::node_kind::static_decl: {
    const auto &decl = ast::cast<const ast::static_decl>(node);
    if (decl.decl_kind != ast::static_decl_kind::conditional_compilation) {
      return false;
    }
    const auto taken_branch = resolve_static_if_branch(decl);
    if (!taken_branch.has_value()) {
      return false;
    }
    const auto &body = *taken_branch ? decl.if_body : decl.else_body;
    for (const auto &item : body | std::views::reverse) {
      if (item != nullptr) {
        return stmt_definitely_returns(*item);
      }
    }
    return false;
  }
  default:
    return false;
  }
}

/// Checks a `static if` whose condition cannot be decided yet — it depends
/// on a type parameter of the body being checked — each branch under what
/// its condition establishes (phase 9, rule 5). A branch no permitted type
/// can take is not checked at all: there is no `T` for which it would run.
/// Returns `nullopt` when the condition says nothing this compiler reads,
/// leaving the caller's ordinary both-branches check to run.
auto checker::check_narrowed_static_if(const ast::static_decl &decl,
                                       type_id expected_tail)
    -> std::optional<std::pair<type_id, type_id>> {
  if (decl.if_condition == nullptr || decl.if_condition->has_error) {
    return std::nullopt;
  }
  const auto narrowed = narrowing_of(*decl.if_condition);
  if (!narrowed.has_value()) {
    return std::nullopt;
  }
  const auto unit = session_.types_.builtin("unit");
  const auto check_under =
      [&](const std::vector<ast::ptr<ast::node>> &body,
          const std::optional<type_domain> &domain) -> type_id {
    if (domain.has_value() && domain->empty()) {
      return session_.types_.builtin("never");
    }
    if (domain.has_value()) {
      scoped_domains_.emplace_back(narrowed->param, *domain);
    }
    ++function_.in_speculative_static_branch_;
    const auto result = check_body_nodes(body, expected_tail);
    --function_.in_speculative_static_branch_;
    if (domain.has_value()) {
      scoped_domains_.pop_back();
    }
    return result;
  };
  const auto if_type = check_under(decl.if_body, narrowed->then_domain);
  const auto else_type =
      decl.else_body.empty()
          ? unit
          : check_under(decl.else_body, narrowed->else_domain);
  return std::pair{if_type, else_type};
}

/// What the code *after* a `static if` may assume: when the branch always
/// returns and there is no `else`, reaching the next statement means the
/// condition was false. `nullopt` when that says nothing usable.
auto checker::narrowing_past(const ast::node &node)
    -> std::optional<narrowing> {
  if (node.kind != ast::node_kind::static_decl) {
    return std::nullopt;
  }
  const auto &decl = ast::cast<const ast::static_decl>(node);
  if (decl.decl_kind != ast::static_decl_kind::conditional_compilation ||
      decl.if_condition == nullptr || !decl.else_body.empty() ||
      !inside_unbound_generic_scope()) {
    return std::nullopt;
  }
  const ast::node *last = nullptr;
  for (const auto &item : decl.if_body) {
    if (item != nullptr) {
      last = item.get();
    }
  }
  if (last == nullptr || !stmt_definitely_returns(*last)) {
    return std::nullopt;
  }
  auto narrowed = narrowing_of(*decl.if_condition);
  if (!narrowed.has_value() || !narrowed->else_domain.has_value()) {
    return std::nullopt;
  }
  return narrowed;
}

auto checker::record_static_for_block(const ast::static_decl &decl) -> void {
  if (decl.for_patterns.size() != 1 || decl.for_patterns[0] == nullptr) {
    return;
  }
  // In a comptime-only function, failures are silent: a `static for` that
  // reads the call's parameters was already rewritten into an ordinary
  // loop (`rewrite_call_time_static`), so what is left fails only where
  // the call itself would report it.
  const auto silent = function_.in_comptime_only_function_;
  if (inside_unbound_generic_scope()) {
    defer_to_instances(decl,
                       [this, &decl, silent,
                        file = file_.file_id_](instance_subst &subst) -> void {
                         const auto saved_file = file_.file_id_;
                         file_.file_id_ = file;
                         unroll_static_for_block(*clone_of(subst, &decl),
                                                 silent);
                         file_.file_id_ = saved_file;
                       });
    return;
  }
  unroll_static_for_block(decl, silent);
}

auto checker::unroll_static_for_block(const ast::static_decl &decl, bool silent)
    -> void {
  if (auto unrolled = static_for_block_iterations(decl, silent)) {
    cache_.static_for_iterations_.insert_or_assign(&decl, std::move(*unrolled));
  }
}

/// A block `static for`'s iterations, each with the block `static for`s
/// directly inside its body unrolled under its bindings. `nullopt` when one
/// of them cannot be unrolled.
auto checker::static_for_block_iterations(const ast::static_decl &decl,
                                          bool silent)
    -> std::optional<std::vector<static_for_iteration>> {
  const auto iterations = evaluate_static_for_inline(decl, silent);
  if (!iterations.has_value()) {
    return std::nullopt;
  }
  auto inner = std::vector<const ast::static_decl *>{};
  auto static_ifs = std::vector<const ast::static_decl *>{};
  for (const auto &node : decl.for_body) {
    if (node == nullptr) {
      continue;
    }
    ast::walk(*node, [&](const ast::node &child) -> bool {
      if (child.kind == ast::node_kind::func_decl ||
          child.kind == ast::node_kind::quote_expr) {
        return false;
      }
      if (const auto *nested = ast::as<const ast::static_decl>(&child)) {
        if (nested->decl_kind == ast::static_decl_kind::for_block) {
          inner.push_back(nested);
          return false;
        }
        if (nested->decl_kind ==
                ast::static_decl_kind::conditional_compilation &&
            nested->if_condition != nullptr) {
          static_ifs.push_back(nested);
        }
      }
      return true;
    });
  }
  const auto bindings = collect_pattern_bindings(*decl.for_patterns[0]);
  auto unrolled = std::vector<static_for_iteration>{};
  unrolled.reserve(iterations->size());
  for (const auto &scope : *iterations) {
    auto iteration = static_for_iteration{};
    for (const auto &binding : bindings) {
      const auto value = scope.find(binding.name);
      const auto type = session_.node_types_.find(binding.node);
      const auto *literal =
          value == scope.end() || type == session_.node_types_.end()
              ? nullptr
              : materialize_static_value(value->second, binding.span,
                                         type->second);
      // A loop variable with no literal form (a list) cannot be written
      // into each copy of the body; the loop is not unrolled.
      if (literal == nullptr) {
        return std::nullopt;
      }
      iteration.bindings.emplace_back(binding.name, literal);
    }
    comptime_eval_.push_locals(scope);
    // A condition that reads nothing it can evaluate now keeps the decision
    // checking made, if any.
    for (const auto *nested : static_ifs) {
      const auto condition = comptime_eval_.try_evaluate(*nested->if_condition);
      if (condition.has_value() &&
          condition->kind == comptime::value_kind::boolean) {
        iteration.static_ifs.emplace(nested, condition->boolean);
      }
    }
    for (const auto *nested : inner) {
      auto nested_iterations = static_for_block_iterations(*nested, silent);
      if (!nested_iterations.has_value()) {
        comptime_eval_.pop_locals();
        return std::nullopt;
      }
      iteration.nested.emplace_back(nested, std::move(*nested_iterations));
    }
    comptime_eval_.pop_locals();
    unrolled.push_back(std::move(iteration));
  }
  return unrolled;
}

auto checker::check_static_decl(const ast::static_decl &decl, bool in_body)
    -> void {
  comptime_eval_.set_file(file_.file_id_);
  switch (decl.decl_kind) {
  case ast::static_decl_kind::binding: {
    // Already checked on demand by a reference that came first
    // (`check_static_binding_early`).
    if (cache_.early_checked_static_bindings_.contains(&decl)) {
      return;
    }
    auto declared = k_unknown_type;
    if (decl.type_annotation != nullptr) {
      declared = resolve_type(*decl.type_annotation, current_resolve_ctx());
    }
    auto found = k_unknown_type;
    if (decl.initializer != nullptr) {
      // A reference to this binding from its own initializer is a cycle,
      // not a reason to check it again on demand.
      cache_.statics_in_progress_.insert(&decl);
      found = infer_expr(*decl.initializer, declared);
      cache_.statics_in_progress_.erase(&decl);
      // An unannotated binding's type is whatever its initializer settles
      // to, and every reader needs that type rather than the initializer's
      // still-open literal leaves (`[1, 2, 3]` is `array[int32, 3]`).
      if (decl.type_annotation == nullptr) {
        found = demand(found);
      }
      if (decl.type_annotation != nullptr) {
        type_mismatch(decl.initializer->span, declared, found,
                      "from the annotation", decl.initializer.get());
      }
    }
    cache_.static_types_.insert_or_assign(
        &decl, decl.type_annotation != nullptr ? declared : found);
    // A forward reference from an earlier-checked `static let` (or an
    // ordinary expression that reached this name first — see
    // `resolve_ident`) may already have resolved this binding lazily;
    // `ensure_static_binding_evaluated` skips re-evaluating (and
    // potentially re-diagnosing) it in that case.
    ensure_static_binding_evaluated(decl);
    return;
  }
  case ast::static_decl_kind::assertion:
    if (decl.assert_condition != nullptr && !decl.assert_condition->has_error) {
      require_bool(*decl.assert_condition, "a `static assert` condition");
      const auto evaluated = comptime_eval_.evaluate(*decl.assert_condition);
      // Skip the "condition was false" diagnostic (but not evaluation
      // itself, which still surfaces a genuine problem like an undefined
      // name) in three cases where "false" isn't a trustworthy verdict
      // yet: inside a `static if`'s speculative, not-known-to-be-reached
      // fallback branch (`function_.in_speculative_static_branch_`); anywhere
      // in a still-abstract generic template pass (`inside_unbound_generic_
      // scope`); or past a preceding sibling already known to have
      // returned (`function_.in_known_unreachable_code_`) — see each helper's
      // doc comment for why an unconditional `static assert false` standing in
      // for "no case matched" must not be judged reachable in any of them.
      if (!evaluated.is_error() &&
          evaluated.kind == comptime::value_kind::boolean &&
          !evaluated.is_true() &&
          function_.in_speculative_static_branch_ == 0 &&
          !function_.in_known_unreachable_code_ &&
          !inside_unbound_generic_scope()) {
        report_failed_static_assert(decl);
      }
    }
    return;
  case ast::static_decl_kind::conditional_compilation: {
    const auto taken_branch = resolve_static_if_branch(decl);
    // When the condition evaluated to a real boolean, only the taken
    // branch is checked — this is real branch selection, not just
    // type-checking both sides. When evaluation couldn't determine a
    // value (unsupported construct, forward reference, inside a generic
    // template, ...), fall back to checking both branches so users still
    // get diagnostics for whichever branch has real problems.
    if (!taken_branch.has_value()) {
      defer_static_if(decl);
    }
    if (taken_branch.has_value()) {
      session_.static_if_taken_branch_.insert_or_assign(&decl, *taken_branch);
      check_body_nodes(*taken_branch ? decl.if_body : decl.else_body,
                       k_unknown_type);
    } else if (check_narrowed_static_if(decl, k_unknown_type).has_value()) {
      // Checked under each branch's narrowing.
    } else {
      ++function_.in_speculative_static_branch_;
      check_body_nodes(decl.if_body, k_unknown_type);
      check_body_nodes(decl.else_body, k_unknown_type);
      --function_.in_speculative_static_branch_;
    }
    return;
  }
  case ast::static_decl_kind::for_inline:
  case ast::static_decl_kind::for_block: {
    push_scope();
    auto element = k_unknown_type;
    if (decl.for_iterable != nullptr) {
      element = static_for_element_type(*decl.for_iterable);
    }
    for (const auto &pattern : decl.for_patterns) {
      if (pattern != nullptr) {
        check_pattern(*pattern,
                      decl.for_patterns.size() == 1 ? element : k_unknown_type);
        for (const auto &binding : collect_pattern_bindings(*pattern)) {
          session_.static_loop_bindings_.insert(span_key(binding.span));
        }
      }
    }
    if (decl.for_guard != nullptr) {
      require_bool(*decl.for_guard, "a `static for` guard");
    }
    if (decl.for_yield != nullptr) {
      const auto expected_element =
          std::exchange(cache_.static_for_expected_element_, k_unknown_type);
      cache_.static_for_reads_.insert_or_assign(&decl, static_for_reads{});
      cache_.static_for_yields_.emplace_back(&decl,
                                             function_.scopes_.size() - 1);
      const auto yield_type = infer_expr(*decl.for_yield, expected_element);
      cache_.static_for_yields_.pop_back();
      // The list the loop builds is the one its use expects, so a yielded
      // literal takes that element type before the yield is run (which
      // would settle it at its default). A mismatch is reported where the
      // list is used.
      if (expected_element != k_unknown_type) {
        static_cast<void>(agrees(expected_element, yield_type));
      }
      cache_.static_for_yield_types_.insert_or_assign(&decl, yield_type);
    }
    const auto block_form = decl.decl_kind == ast::static_decl_kind::for_block;
    // At module level the body is checked once here, with the loop
    // variables unbound, and its compile-time declarations run once per
    // iteration below; here they are not evaluated for a verdict.
    const auto block_at_module = block_form && !in_body;
    function_.static_for_block_depth_ += block_form ? 1 : 0;
    function_.in_speculative_static_branch_ += block_at_module ? 1 : 0;
    const auto saved_silent = block_at_module ? comptime_eval_.set_silent(true)
                                              : comptime_eval_.silent();
    check_body_nodes(decl.for_body, k_unknown_type);
    comptime_eval_.set_silent(saved_silent);
    function_.in_speculative_static_branch_ -= block_at_module ? 1 : 0;
    function_.static_for_block_depth_ -= block_form ? 1 : 0;
    pop_scope();

    // The inline form's values, computed once here for code that is not
    // generic; a template's are computed per instance instead
    // (`record_static_for_inline`). In a comptime-only function a failure
    // here is silent, as in `record_static_for_block`.
    if (decl.decl_kind == ast::static_decl_kind::for_inline) {
      if (!inside_unbound_generic_scope()) {
        if (auto values = evaluate_static_for_inline(
                decl, function_.in_comptime_only_function_)) {
          cache_.static_for_values_.insert_or_assign(&decl, std::move(*values));
        }
      }
      return;
    }

    // In a function body, the block form is code that runs, once per
    // iteration: lowering unrolls it from the iterations recorded here.
    if (in_body) {
      // One inside another is unrolled with the outermost, so it can read
      // the enclosing loops' variables.
      if (function_.static_for_block_depth_ == 0) {
        record_static_for_block(decl);
      }
      return;
    }

    // At module level, the body's compile-time declarations run once per
    // iteration. An iterable inside a generic scope (a functor body) may
    // depend on a type parameter with no value yet.
    if (!inside_unbound_generic_scope()) {
      if (const auto iterations = evaluate_static_for_inline(decl, false)) {
        for (const auto &scope : *iterations) {
          comptime_eval_.push_locals(scope);
          run_static_for_iteration(decl.for_body);
          comptime_eval_.pop_locals();
        }
      }
    }
    return;
  }
  }
}

auto checker::report_failed_static_assert(const ast::static_decl &decl)
    -> void {
  emit_diag(diagnostic(diagnostic_level::error,
                       decl.assert_message.has_value()
                           ? *decl.assert_message
                           : "`static assert` condition was false",
                       file_.file_id_)
                .with_label(decl.assert_condition->span,
                            "this condition evaluated to `false`"));
}

auto checker::run_static_for_iteration(
    const std::vector<ast::ptr<ast::node>> &body) -> void {
  for (const auto &node : body) {
    const auto *decl = ast::as<const ast::static_decl>(node.get());
    if (decl == nullptr || decl->has_error) {
      continue;
    }
    switch (decl->decl_kind) {
    case ast::static_decl_kind::assertion: {
      if (decl->assert_condition == nullptr ||
          decl->assert_condition->has_error) {
        break;
      }
      const auto evaluated = comptime_eval_.evaluate(*decl->assert_condition);
      if (!evaluated.is_error() &&
          evaluated.kind == comptime::value_kind::boolean &&
          !evaluated.is_true()) {
        report_failed_static_assert(*decl);
      }
      break;
    }
    case ast::static_decl_kind::conditional_compilation: {
      if (decl->if_condition == nullptr || decl->if_condition->has_error) {
        break;
      }
      const auto evaluated = comptime_eval_.evaluate(*decl->if_condition);
      if (!evaluated.is_error() &&
          evaluated.kind == comptime::value_kind::boolean) {
        run_static_for_iteration(evaluated.is_true() ? decl->if_body
                                                     : decl->else_body);
      }
      break;
    }
    case ast::static_decl_kind::for_block: {
      if (const auto iterations = evaluate_static_for_inline(*decl, false)) {
        for (const auto &scope : *iterations) {
          comptime_eval_.push_locals(scope);
          run_static_for_iteration(decl->for_body);
          comptime_eval_.pop_locals();
        }
      }
      break;
    }
    default:
      break;
    }
  }
}

auto checker::reads_call_time_value(const ast::expr &expr, int depth) -> bool {
  const auto call_time = [&](const std::string &name) -> bool {
    const auto *binding = find_value_in_scopes(name).binding;
    if (binding == nullptr) {
      return false;
    }
    const auto key = span_key(binding->span);
    if (session_.static_loop_bindings_.contains(key)) {
      return false;
    }
    if (const auto let = session_.comptime_let_initializers_.find(key);
        let != session_.comptime_let_initializers_.end() && depth < 64) {
      return reads_call_time_value(*let->second, depth + 1);
    }
    return true;
  };
  auto found = false;
  ast::walk(expr, [&](const ast::node &node) -> bool {
    if (found || node.kind == ast::node_kind::quote_expr) {
      return false;
    }
    if (const auto *ident = ast::as<const ast::ident_expr>(&node)) {
      found = call_time(ident->name);
    } else if (const auto *path = ast::as<const ast::module_path_expr>(&node);
               path != nullptr && !path->segments.empty()) {
      found = call_time(path->segments.front());
    }
    return !found;
  });
  return found;
}

auto checker::rewrite_call_time_static(ast::ptr<ast::node> &slot) -> bool {
  auto *decl = ast::as<ast::static_decl>(slot.get());
  if (decl == nullptr || decl->has_error) {
    return false;
  }
  auto replacement = ast::ptr<ast::node>{};
  switch (decl->decl_kind) {
  case ast::static_decl_kind::conditional_compilation: {
    if (decl->if_condition == nullptr ||
        !reads_call_time_value(*decl->if_condition)) {
      return false;
    }
    auto stmt = ast::make<ast::if_stmt>();
    stmt->span = decl->span;
    stmt->branches.push_back(
        ast::if_branch{.span = decl->span,
                       .condition = std::move(decl->if_condition),
                       .let_pattern = nullptr,
                       .let_expr = nullptr,
                       .body = std::move(decl->if_body)});
    stmt->else_body = std::move(decl->else_body);
    replacement = std::move(stmt);
    break;
  }
  case ast::static_decl_kind::for_block: {
    if (decl->for_iterable == nullptr ||
        !reads_call_time_value(*decl->for_iterable)) {
      return false;
    }
    auto stmt = ast::make<ast::for_stmt>();
    stmt->span = decl->span;
    stmt->patterns = std::move(decl->for_patterns);
    stmt->iterable = std::move(decl->for_iterable);
    stmt->guard = std::move(decl->for_guard);
    stmt->body = std::move(decl->for_body);
    replacement = std::move(stmt);
    break;
  }
  case ast::static_decl_kind::for_inline: {
    if (decl->for_iterable == nullptr || decl->for_yield == nullptr ||
        !reads_call_time_value(*decl->for_iterable)) {
      return false;
    }
    auto comprehension = ast::make<ast::for_expr>();
    comprehension->span = decl->span;
    auto clause = ast::for_expr::iter_clause{};
    for (auto &pattern : decl->for_patterns) {
      clause.patterns.push_back(std::move(pattern));
    }
    clause.iterable = std::move(decl->for_iterable);
    comprehension->clauses.push_back(std::move(clause));
    comprehension->guard = std::move(decl->for_guard);
    comprehension->yield_expr = std::move(decl->for_yield);
    auto stmt = ast::make<ast::expr_stmt>();
    stmt->span = decl->span;
    stmt->expr = std::move(comprehension);
    replacement = std::move(stmt);
    break;
  }
  default:
    return false;
  }
  session_.retired_static_nodes_.push_back(std::move(slot));
  slot = std::move(replacement);
  return true;
}

/// Records every module-level `static def` in `items` (descending into
/// inline submodules) as compile-time-only (`session_.comptime_only_
/// functions_`) and by name (`session_.static_functions_by_name_`).
auto checker::register_static_functions(
    const std::vector<ast::ptr<ast::node>> &items) -> void {
  for (const auto &item : items) {
    if (item == nullptr || item->has_error) {
      continue;
    }
    if (item->kind == ast::node_kind::func_decl) {
      const auto &fn = ast::cast<const ast::func_decl>(*item);
      if (fn.modifiers.is_static && !fn.name.empty()) {
        session_.static_functions_by_name_.insert_or_assign(fn.name, &fn);
        session_.comptime_only_functions_.insert(&fn);
      }
    } else if (item->kind == ast::node_kind::sub_module_decl) {
      register_static_functions(
          ast::cast<const ast::sub_module_decl>(*item).items);
    }
  }
}

} // namespace cinder::semantic::detail
