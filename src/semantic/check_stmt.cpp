#include "checker.h"

namespace cinder::semantic::detail {

// ==========================================================================
//  Statements
// ==========================================================================

/// Human-readable phrase for a binding's origin, used in the "declared
/// here" note on an immutable-assignment error (e.g. "bound with `let`").
auto checker::binding_origin_description(binding_origin origin)
    -> std::string_view {
  switch (origin) {
  case binding_origin::let_binding:
    return "bound with `let`";
  case binding_origin::pattern_binding:
    return "bound by a pattern";
  case binding_origin::parameter:
    return "a parameter";
  case binding_origin::var_binding:
    return "declared with `var`";
  case binding_origin::mut_binding:
    return "declared with `mut`";
  case binding_origin::synthetic:
    return "introduced here";
  }
  return "bound here";
}

/// Walks through field/index/group access to find the identifier at the
/// root of an assignment target (e.g. `a` in `a.b[0].c = x`), so mutation
/// through a `let`-bound root can be flagged even when the assignment
/// itself targets a nested field.
auto checker::assignment_root_ident(const ast::expr &target)
    -> const ast::ident_expr * {
  const auto *current = &target;
  while (true) {
    switch (current->kind) {
    case ast::node_kind::ident_expr:
      return dynamic_cast<const ast::ident_expr *>(current);
    case ast::node_kind::field_expr: {
      const auto &field = dynamic_cast<const ast::field_expr &>(*current);
      if (field.object == nullptr) {
        return nullptr;
      }
      current = field.object.get();
      break;
    }
    case ast::node_kind::index_expr: {
      const auto &index = dynamic_cast<const ast::index_expr &>(*current);
      if (index.object == nullptr) {
        return nullptr;
      }
      current = index.object.get();
      break;
    }
    case ast::node_kind::group_expr: {
      const auto &group = dynamic_cast<const ast::group_expr &>(*current);
      if (group.inner == nullptr) {
        return nullptr;
      }
      current = group.inner.get();
      break;
    }
    default:
      return nullptr;
    }
  }
}

/// Checks an assignment statement: a plain identifier target must not be
/// `let`/pattern-bound; a computed target (field/index chain) is checked
/// for mutation through a `let`-bound root; a compound-assignment
/// operator (`+=` etc.) requires a numeric target; and the assigned
/// value's type is checked against the target's type.
/// If `target` is `v[i]` with `v` a user struct/sum/opaque, resolves the
/// write against `index_set` and answers the element type the assigned
/// value must have. `nullopt` for every other assignment shape, including
/// an index into a builtin container, which keeps its direct store.
auto checker::check_user_index_assignment(const ast::expr &target)
    -> std::optional<type_id> {
  if (target.kind != ast::node_kind::index_expr) {
    return std::nullopt;
  }
  const auto &index = dynamic_cast<const ast::index_expr &>(target);
  if (index.object == nullptr) {
    return std::nullopt;
  }
  // Chosen from the head alone; the instance waits for the elements
  // (`dispatch_index_when_settled`), and the assigned value is checked
  // against the element leaf, which is often what settles it.
  const auto object =
      demand_shape(base_shape(infer_expr(*index.object, k_unknown_type)));
  const auto &entry = session_.types_.entry(object);
  if (entry.kind != type_kind::struct_kind &&
      entry.kind != type_kind::sum_kind &&
      entry.kind != type_kind::opaque_kind) {
    return std::nullopt;
  }
  const auto key =
      index.index != nullptr
          ? strip_refs(infer_expr(*index.index, expected_index_key(index)))
          : k_unknown_type;
  const auto element = require_index_set_trait(index, object, entry, key);
  record_expr_type(target, element);
  return element;
}

auto checker::check_assignment(const ast::assign_stmt &stmt) -> void {
  auto target_type = k_unknown_type;

  // Everything known about the assigned value described the *old* one. Drop
  // it before checking the new value, so nothing proved about `x` before an
  // `x = read()` can be used after it.
  if (stmt.target != nullptr) {
    if (const auto *root = assignment_root_ident(*stmt.target)) {
      invalidate_facts(root->name);
    }
  }

  if (stmt.target != nullptr) {
    if (stmt.target->kind == ast::node_kind::ident_expr) {
      const auto &ident = dynamic_cast<const ast::ident_expr &>(*stmt.target);
      if (const auto *binding = lookup_value(ident.name)) {
        target_type = binding->type;
        // Unlike the "not found" branch below, this path resolves the
        // target straight from the scope binding rather than through
        // `infer_expr`, so it needs its own persistence hook — see
        // `session_.node_types_`'s doc comment.
        record_expr_type(ident, target_type);
        if (binding->origin == binding_origin::let_binding ||
            binding->origin == binding_origin::pattern_binding) {
          auto diag =
              diagnostic(diagnostic_level::error,
                         std::format("cannot assign to immutable binding `{}`",
                                     ident.name),
                         file_.file_id_);
          diag.with_label(ident.span, "this binding is immutable");
          diag.children.push_back(
              diagnostic(
                  diagnostic_level::note,
                  std::format("`{}` is {}", ident.name,
                              binding_origin_description(binding->origin)),
                  file_.file_id_)
                  .with_label(binding->span, "binding declared here"));
          diag.with_help(std::format(
              "Declare it with `var {}` to allow reassignment, or shadow "
              "it with a new `let`.",
              ident.name));
          emit_diag(diag);
          mark_error();
        }
      } else {
        target_type =
            record_expr_type(ident, resolve_ident(ident, k_unknown_type));
      }
    } else if (const auto assigned_element =
                   check_user_index_assignment(*stmt.target);
               assigned_element.has_value()) {
      // `v[i] = x` on a user type dispatches to `index_set`, and must not
      // go through `infer_expr` on the target: that would resolve the
      // same syntax as a *read* against `index`, reporting "has no
      // `index` impl" for a type whose only omission is the write half.
      target_type = *assigned_element;
    } else {
      target_type = infer_expr(*stmt.target, k_unknown_type);
      auto root_name = std::string{};
      if (const auto handle = place_through_deref(*stmt.target)) {
        // The binding's mutability is beside the point: no binding makes
        // a write through `deref` legal.
        report_write_through_deref(stmt.target->span, *handle, "assign to");
      } else if (const auto *root = assignment_root_ident(*stmt.target)) {
        root_name = root->name;
      } else if (stmt.target->kind == ast::node_kind::module_path_expr) {
        const auto &path =
            dynamic_cast<const ast::module_path_expr &>(*stmt.target);
        if (!path.segments.empty()) {
          root_name = path.segments.front();
        }
      }
      if (!root_name.empty()) {
        const auto *binding = lookup_value(root_name);
        // A `&mut`/`*mut` binding is *itself* immutable here — what is
        // being written is the pointee, and the right to write it comes
        // from the type, not from how the pointer was bound. `let p =
        // xs.as_mut_ptr()` followed by `p[0] = 1` changes `xs`, never
        // `p`; requiring `var p` would be asking the user to declare the
        // wrong thing mutable.
        const auto &binding_entry = binding != nullptr
                                        ? session_.types_.entry(binding->type)
                                        : session_.types_.entry(k_unknown_type);
        const auto writes_through_indirection =
            binding_entry.kind == type_kind::ref_kind ||
            (binding_entry.kind == type_kind::ptr_kind && binding_entry.is_mut);
        // Writing *through* a read-only `*T` is refused on the type, not
        // on the binding: `var p` would not make it legal, and reporting
        // "declare `p` with `var`" here would hand the user advice that
        // both fails to help and, taken, would let the write through.
        if (binding != nullptr && binding_entry.kind == type_kind::ptr_kind &&
            !binding_entry.is_mut &&
            stmt.target->kind != ast::node_kind::ident_expr) {
          error_with_help(
              stmt.target->span,
              std::format("cannot write through `{}` — it is a `{}`", root_name,
                          session_.types_.display(binding->type)),
              "this pointer is read-only",
              "A `*T` only permits reads. Obtain a `*mut T` instead — for "
              "example with `.as_mut_ptr()` rather than `.as_ptr()`.");
        } else if (binding != nullptr &&
                   binding->origin == binding_origin::let_binding &&
                   !writes_through_indirection) {
          error_with_help(
              stmt.target->span,
              std::format("cannot mutate `{}` — it is bound with `let`",
                          root_name),
              "mutation through an immutable binding",
              std::format("Declare `{}` with `var` to allow mutation.",
                          root_name));
        }
      }
    }
  }

  const auto stripped = strip_refs(target_type);
  if (stmt.op != ast::assign_op::assign &&
      !session_.types_.is_unknown(stripped) &&
      !session_.types_.is_numeric(stripped)) {
    error(stmt.span,
          std::format("compound assignment requires a numeric target, "
                      "found `{}`",
                      session_.types_.display(stripped)),
          "not a numeric value");
  }

  if (stmt.value != nullptr) {
    const auto found = infer_expr(*stmt.value, stripped);
    // `x += y` is `x = x + y`: the operands must agree exactly as they do
    // for `+`, since numbers never convert implicitly.
    if (stmt.op == ast::assign_op::assign ||
        session_.types_.is_numeric(stripped)) {
      type_mismatch(stmt.value->span, stripped, found,
                    "from the assignment target", stmt.value.get());
    }
  }
}

/// Checks one node in statement position. Returns the node's value type so
/// blocks can surface their tail expression; bindings extend the innermost
/// scope.
/// Checks one statement-position AST node and returns its value type
/// (used as the tail-expression type when it is the last node of a
/// block). Handles every statement kind directly; a bare expression or a
/// nested declaration in statement position falls through to
/// `infer_expr`/`check_item` respectively.
auto checker::check_body_node(const ast::node &node, type_id expected_tail)
    -> type_id {
  if (node.has_error) {
    return k_unknown_type;
  }
  const auto unit = session_.types_.builtin("unit");

  switch (node.kind) {
  case ast::node_kind::let_stmt: {
    const auto &stmt = dynamic_cast<const ast::let_stmt &>(node);
    if (const auto *lambda = generic_lambda_of(stmt)) {
      bind_generic_lambda(stmt, *lambda);
      return unit;
    }
    if (stmt.pattern != nullptr &&
        stmt.pattern->kind == ast::node_kind::binding_pattern) {
      const auto &binding =
          dynamic_cast<const ast::binding_pattern &>(*stmt.pattern);
      if (binding.is_mut &&
          report_mutable_generic_lambda(stmt.initializer.get(),
                                        stmt.type_annotation != nullptr,
                                        binding.name)) {
        bind_value(binding.name, k_error_type, binding_origin::mut_binding,
                   binding.span);
        return unit;
      }
    }
    auto declared = k_unknown_type;
    if (stmt.type_annotation != nullptr) {
      declared = resolve_type(*stmt.type_annotation, current_resolve_ctx());
    }
    auto found = k_unknown_type;
    if (stmt.initializer != nullptr) {
      found = infer_expr(*stmt.initializer, declared);
      if (stmt.type_annotation != nullptr) {
        type_mismatch(stmt.initializer->span, declared, found,
                      "from the annotation", stmt.initializer.get());
      }
    }
    const auto binding_type =
        stmt.type_annotation != nullptr ? declared : found;
    if (!stmt.else_body.empty()) {
      check_body_nodes(stmt.else_body, k_unknown_type);
    }
    if (stmt.pattern != nullptr) {
      if (stmt.pattern->kind == ast::node_kind::binding_pattern) {
        const auto &binding =
            dynamic_cast<const ast::binding_pattern &>(*stmt.pattern);
        // Bypasses check_pattern (which records this itself at its own
        // call site), so record explicitly: a later pass (the move
        // checker) needs every plain `let` binding's type keyed by its
        // pattern node, the same way a destructured binding's is.
        record_expr_type(*stmt.pattern, binding_type);
        bind_value(binding.name, binding_type,
                   binding.is_mut ? binding_origin::mut_binding
                                  : binding_origin::let_binding,
                   binding.span);
        // An immutable binding's type is a permanent fact about it: a `let
        // i: index[n]` is an `i < n` for the rest of the block. (A `var` is
        // deliberately *not* assumed — see `check_assignment`, which has to
        // invalidate what assignment breaks.)
        if (!binding.is_mut) {
          assume_binding(binding.name, binding_type);
          // `let n = T.name()`: a later `static if n == "int8"` narrows
          // `T` exactly as `static if T.name() == "int8"` would.
          if (stmt.initializer != nullptr) {
            if (const auto reflected = reflected_name_of(*stmt.initializer);
                reflected.has_value() &&
                stmt.initializer->kind == ast::node_kind::call_expr) {
              function_.type_name_aliases_.insert_or_assign(binding.name,
                                                            *reflected);
            } else {
              function_.type_name_aliases_.erase(binding.name);
            }
          }
          // Also make the binding available to `resolve_static_if_branch`
          // for any `static if` later in this same function body: a
          // pattern like `std.limits.min`'s `let n = T.name()` followed
          // by `static if n == "int8": ...` only narrows correctly once
          // `n` itself is a known compile-time value, not just `T`. Tried
          // unconditionally (not gated on being inside a generic
          // instance) since `try_evaluate` is silent and cheap on
          // failure — an ordinary runtime-dependent `let` simply fails to
          // resolve a name and returns `std::nullopt`, same as today.
          if (stmt.initializer != nullptr) {
            if (auto value = comptime_eval_.try_evaluate(*stmt.initializer);
                value.has_value()) {
              comptime_eval_.bind_local(binding.name, std::move(*value));
            }
            // An instance knows what the template could not: `T.name()`
            // is a string there.
            defer_to_instances(
                stmt,
                [this, &stmt,
                 name = binding.name](instance_subst &subst) -> void {
                  const auto *initializer =
                      clone_of(subst, stmt.initializer.get());
                  if (auto value = comptime_eval_.try_evaluate(*initializer);
                      value.has_value()) {
                    comptime_eval_.bind_local(name, std::move(*value));
                  }
                });
          }
        }
      } else {
        check_pattern(*stmt.pattern, strip_refs(binding_type));
      }
    }
    return unit;
  }

  case ast::node_kind::var_stmt: {
    const auto &stmt = dynamic_cast<const ast::var_stmt &>(node);
    if (report_mutable_generic_lambda(stmt.initializer.get(),
                                      stmt.type_annotation != nullptr,
                                      stmt.name)) {
      record_expr_type(stmt, k_error_type);
      bind_value(stmt.name, k_error_type, binding_origin::var_binding,
                 stmt.span);
      return unit;
    }
    auto declared = k_unknown_type;
    if (stmt.type_annotation != nullptr) {
      declared = resolve_type(*stmt.type_annotation, current_resolve_ctx());
    }
    auto found = k_unknown_type;
    if (stmt.initializer != nullptr) {
      found = infer_expr(*stmt.initializer, declared);
      if (stmt.type_annotation != nullptr) {
        type_mismatch(stmt.initializer->span, declared, found,
                      "from the annotation", stmt.initializer.get());
      }
    }
    const auto binding_type =
        stmt.type_annotation != nullptr ? declared : found;
    // `var` has no pattern node to key against, so key on the statement
    // itself — a later pass (the move checker) needs this binding's type
    // and node_types is keyed generically by `const ast::node *`.
    record_expr_type(stmt, binding_type);
    bind_value(stmt.name, binding_type, binding_origin::var_binding, stmt.span);
    return unit;
  }

  case ast::node_kind::assign_stmt:
    check_assignment(dynamic_cast<const ast::assign_stmt &>(node));
    return unit;

  case ast::node_kind::expr_stmt: {
    const auto &stmt = dynamic_cast<const ast::expr_stmt &>(node);
    return stmt.expr != nullptr ? infer_expr(*stmt.expr, expected_tail) : unit;
  }

  case ast::node_kind::return_stmt: {
    const auto &stmt = dynamic_cast<const ast::return_stmt &>(node);
    if (function_.in_generator_) {
      // A generator's only output channel is `yield`; `return` just marks
      // exhaustion (translates to the iterator's `next()` returning
      // `none` forever after), so a value here would have nowhere to go.
      if (stmt.value != nullptr) {
        error_with_help(
            stmt.span,
            "`return <value>` is not allowed inside a `generator def`",
            "generators have no second return channel",
            "Use `yield` to hand values back to the caller, and a bare "
            "`return` to end the generator early.");
      }
      return session_.types_.builtin("never");
    }
    if (stmt.value != nullptr) {
      const auto expected_for_value = function_.checking_existential_return_
                                          ? k_unknown_type
                                          : function_.return_type_;
      const auto found = infer_expr(*stmt.value, expected_for_value);
      if (function_.checking_existential_return_) {
        function_.existential_underlying_ =
            join_branch_type(function_.existential_underlying_, found,
                             stmt.value->span, "existential return");
      } else if (function_.inferring_return_) {
        note_inferred_return(found, stmt.value->span);
      } else if (function_.return_annotated_ &&
                 !agrees(function_.return_type_, found)) {
        if (report_deref_if_missing(stmt.value.get(), function_.return_type_,
                                    found)) {
          mark_error();
          return session_.types_.builtin("never");
        }
        auto diag = diagnostic(
            diagnostic_level::error,
            std::format("return type mismatch: expected `{}`, found `{}`",
                        session_.types_.display(function_.return_type_),
                        session_.types_.display(found)),
            file_.file_id_);
        diag.with_label(
            stmt.value->span,
            std::format("this returns `{}`", session_.types_.display(found)));
        diag.with_note(
            std::format("the enclosing function is declared to return `{}`",
                        session_.types_.display(function_.return_type_)));
        emit_diag(diag);
        mark_error();
      } else if (function_.return_annotated_) {
        // Structurally fine — but returning into a refined return type is
        // still a narrowing, and owes the same proof an argument would.
        check_narrowing(function_.return_type_, found, stmt.value.get(),
                        stmt.value->span, "for this returned value");
      }
    } else if (!function_.checking_existential_return_ &&
               function_.return_annotated_ &&
               !session_.types_.is_unit(function_.return_type_) &&
               !session_.types_.is_unknown(function_.return_type_)) {
      error_with_help(
          stmt.span,
          std::format("bare `return` in a function that returns `{}`",
                      session_.types_.display(function_.return_type_)),
          "missing return value",
          std::format("Return a `{}` value, or change the return type to "
                      "`unit`.",
                      session_.types_.display(function_.return_type_)));
    }
    return session_.types_.builtin("never");
  }

  case ast::node_kind::if_stmt: {
    // Mirrors `infer_if_expr` (used for `if` in ordinary expression
    // position, e.g. `return if cond: a else: b`) rather than always
    // typing to `unit`: an `if` used as a block's tail statement is
    // exactly as value-producing as `match` already is below — join each
    // branch's tail type against `expected_tail` the same way. When
    // `expected_tail` is `k_unknown_type` (this `if` is a genuine
    // mid-body statement, not a tail — see `check_body_nodes`, which
    // only ever passes a real `expected_tail` to the *last* item), this
    // still ends up `unit` exactly like before.
    const auto &stmt = dynamic_cast<const ast::if_stmt &>(node);
    auto result = expected_tail;
    for (const auto &branch : stmt.branches) {
      push_scope();
      check_if_branch_header(branch);
      // Path conditions, exactly as in `infer_if_expr` — see the section
      // comment on `assume_condition`.
      auto assumed = fact_scope(function_.facts_);
      for (const auto &earlier : stmt.branches) {
        if (&earlier == &branch) {
          break;
        }
        assume_condition(earlier.condition.get(), /*negated=*/true);
      }
      assume_condition(branch.condition.get(), /*negated=*/false);
      if (branch.let_pattern != nullptr && branch.let_expr == nullptr) {
        check_pattern(dynamic_cast<const ast::pattern &>(*branch.let_pattern),
                      k_unknown_type);
      }
      const auto branch_type = check_body_nodes(branch.body, expected_tail);
      pop_scope();
      result =
          join_branch_type(result, branch_type,
                           branch.body.empty() || branch.body.back() == nullptr
                               ? branch.span
                               : branch.body.back()->span,
                           "`if`");
    }
    if (!stmt.else_body.empty()) {
      auto refuted = fact_scope(function_.facts_);
      for (const auto &branch : stmt.branches) {
        assume_condition(branch.condition.get(), /*negated=*/true);
      }
      const auto else_type = check_body_nodes(stmt.else_body, expected_tail);
      result = join_branch_type(result, else_type,
                                stmt.else_body.back() != nullptr
                                    ? stmt.else_body.back()->span
                                    : stmt.span,
                                "`if`");
    } else if (session_.types_.entry(result).name == "never") {
      // No `else`: the untaken path always exists and is implicitly
      // `unit` — an `if` whose only branch(es) purely diverge (`return`/
      // `panic`/...) must not make the whole construct register as
      // `never` itself, or a later statement in the same block becomes
      // unreachable in the type system's eyes even though control
      // genuinely falls through when the condition is false. Concretely:
      // `if cond: return x` followed by more statements — `join_branch_type`
      // adopts the branch's `never` because `result` started as
      // `k_unknown_type` (see its "found is `never`" special case), which
      // is correct when there *is* an `else` (every path is spoken for)
      // but wrong here.
      result = unit;
    }
    return result != k_unknown_type ? result : unit;
  }

  case ast::node_kind::break_stmt:
  case ast::node_kind::continue_stmt: {
    // Both are only meaningful with a loop to act on. Outside one there is
    // nothing to exit or advance, so this is an error rather than a no-op.
    if (function_.loop_depth_ == 0) {
      const auto *word =
          node.kind == ast::node_kind::break_stmt ? "break" : "continue";
      error_with_help(
          node.span, std::format("`{}` is only allowed inside a loop", word),
          std::format("there is no enclosing `while` or `for` loop for this "
                      "`{}` to act on",
                      word),
          node.kind == ast::node_kind::break_stmt
              ? "To leave a function early, use `return` instead."
              : "`continue` skips to the next iteration of a loop; if you "
                "meant to skip the rest of a function, use `return`.");
    }
    // `never`, like `return`: control does not fall through to whatever
    // follows, so this must not constrain a surrounding block's type.
    return session_.types_.builtin("never");
  }

  case ast::node_kind::while_stmt: {
    const auto &stmt = dynamic_cast<const ast::while_stmt &>(node);
    if (stmt.condition != nullptr) {
      require_bool(*stmt.condition, "a `while` condition");
    }
    push_scope();
    if (stmt.let_expr != nullptr && stmt.let_pattern != nullptr) {
      const auto subject = infer_expr(*stmt.let_expr, k_unknown_type);
      check_pattern(*stmt.let_pattern, strip_refs(subject));
    }
    ++function_.loop_depth_;
    check_body_nodes(stmt.body, k_unknown_type);
    --function_.loop_depth_;
    pop_scope();
    return unit;
  }

  case ast::node_kind::scope_stmt: {
    const auto &stmt = dynamic_cast<const ast::scope_stmt &>(node);
    check_body_nodes(stmt.body, k_unknown_type);
    // Lowering wraps the body in a block and needs its (unit) type.
    return record_expr_type(stmt, unit);
  }

  case ast::node_kind::for_stmt: {
    const auto &stmt = dynamic_cast<const ast::for_stmt &>(node);
    auto element = k_unknown_type;
    if (stmt.iterable != nullptr) {
      element = resolve_loop_iterable(*stmt.iterable, stmt,
                                      session_.for_iterator_dispatches_[&stmt]);
      if (session_.for_iterator_dispatches_[&stmt].decl == nullptr) {
        session_.for_iterator_dispatches_.erase(&stmt);
      }
    }
    push_scope();
    {
      auto patterns = std::vector<const ast::pattern *>{};
      patterns.reserve(stmt.patterns.size());
      for (const auto &pattern : stmt.patterns) {
        patterns.push_back(pattern.get());
      }
      check_loop_head_patterns(patterns, element);
    }
    if (stmt.guard != nullptr) {
      require_bool(*stmt.guard, "a `for` guard");
    }
    ++function_.loop_depth_;
    check_body_nodes(stmt.body, k_unknown_type);
    --function_.loop_depth_;
    pop_scope();
    return unit;
  }

  case ast::node_kind::match_stmt: {
    const auto &stmt = dynamic_cast<const ast::match_stmt &>(node);
    check_match(stmt.subject.get(), stmt.arms, expected_tail, stmt.span);
    return expected_tail != k_unknown_type ? expected_tail : unit;
  }

  case ast::node_kind::crew_stmt: {
    const auto &stmt = dynamic_cast<const ast::crew_stmt &>(node);
    push_scope();
    bind_value(stmt.name, k_unknown_type, binding_origin::synthetic, stmt.span);
    check_body_nodes(stmt.body, k_unknown_type);
    pop_scope();
    return unit;
  }

  case ast::node_kind::splice_stmt: {
    const auto &stmt = dynamic_cast<const ast::splice_stmt &>(node);
    if (stmt.expr == nullptr) {
      return unit;
    }
    const auto operand_type = infer_expr(*stmt.expr, k_unknown_type);
    require_quote_value(operand_type, stmt.expr->span, "a spliced statement");
    // Reification, mirroring `splice_expr` above: a quoted statement
    // fragment is checked and grafted in directly; a quoted expression
    // fragment is accepted too (splicing `` `(f())` `` in statement
    // position is just an expression-statement, same as writing `f()`
    // there directly).
    const auto fragment_value = comptime_eval_.evaluate(*stmt.expr);
    if (fragment_value.is_error()) {
      return k_error_type;
    }
    if (fragment_value.kind == comptime::value_kind::stmt_fragment &&
        fragment_value.fragment != nullptr) {
      const auto fragment_type =
          check_body_node(*fragment_value.fragment, expected_tail);
      session_.spliced_fragments_[&node] = fragment_value.fragment;
      return fragment_type;
    }
    if (fragment_value.kind == comptime::value_kind::expr_fragment &&
        fragment_value.fragment != nullptr) {
      const auto *fragment_expr =
          dynamic_cast<const ast::expr *>(fragment_value.fragment);
      if (fragment_expr != nullptr) {
        const auto fragment_type = infer_expr(*fragment_expr, expected_tail);
        session_.spliced_fragments_[&node] = fragment_expr;
        return fragment_type;
      }
    }
    error_with_help(
        stmt.expr->span,
        std::format("a spliced statement must resolve to a quoted "
                    "statement or expression, found {}",
                    quote_fragment_kind_name(fragment_value.kind)),
        "this splice is used in statement position",
        "Only a value quoted with expression or statement content can be "
        "spliced here; a quoted definition belongs in item position "
        "instead.");
    return k_error_type;
  }

  case ast::node_kind::asm_stmt:
  case ast::node_kind::use_decl:
  case ast::node_kind::dep_decl:
    return unit;

  case ast::node_kind::static_decl: {
    const auto &decl = dynamic_cast<const ast::static_decl &>(node);
    if (decl.decl_kind != ast::static_decl_kind::conditional_compilation) {
      if (decl.decl_kind == ast::static_decl_kind::for_inline) {
        cache_.static_for_expected_element_ = list_element_type(expected_tail);
      }
      check_item(node, /*at_module_scope=*/false);
      // `static for pat in iter => yield_expr` evaluates to the list of
      // what it yields, like a `for ... =>` comprehension — so as a
      // function's tail statement it is the function's value.
      if (decl.decl_kind == ast::static_decl_kind::for_inline) {
        return record_static_for_inline(decl, expected_tail);
      }
      return unit;
    }
    // Mirrors `if_stmt` above: a `static if` used as a block's tail
    // statement is exactly as value-producing as `if`/`match` — only
    // which branch(es) get checked differs (real compile-time branch
    // selection when a condition value is available, versus joining
    // both like an ordinary `if`).
    const auto taken_branch = resolve_static_if_branch(decl);
    if (taken_branch.has_value()) {
      session_.static_if_taken_branch_.insert_or_assign(&decl, *taken_branch);
      const auto branch_type = check_body_nodes(
          *taken_branch ? decl.if_body : decl.else_body, expected_tail);
      return branch_type != k_unknown_type ? branch_type : unit;
    }
    defer_static_if(decl);
    if (const auto narrowed = check_narrowed_static_if(decl, expected_tail)) {
      auto joined = join_branch_type(expected_tail, narrowed->first, decl.span,
                                     "`static if`");
      if (!decl.else_body.empty()) {
        joined = join_branch_type(joined, narrowed->second, decl.span,
                                  "`static if`");
      }
      if (decl.else_body.empty() &&
          session_.types_.entry(joined).name == "never") {
        joined = unit;
      }
      return joined != k_unknown_type ? joined : unit;
    }
    auto result = expected_tail;
    ++function_.in_speculative_static_branch_;
    const auto if_type = check_body_nodes(decl.if_body, expected_tail);
    result =
        join_branch_type(result, if_type,
                         decl.if_body.empty() || decl.if_body.back() == nullptr
                             ? decl.span
                             : decl.if_body.back()->span,
                         "`static if`");
    if (!decl.else_body.empty()) {
      const auto else_type = check_body_nodes(decl.else_body, expected_tail);
      result = join_branch_type(result, else_type,
                                decl.else_body.back() != nullptr
                                    ? decl.else_body.back()->span
                                    : decl.span,
                                "`static if`");
    }
    --function_.in_speculative_static_branch_;
    if (decl.else_body.empty() &&
        session_.types_.entry(result).name == "never") {
      // See the identical `if_stmt` comment above: a branch-only `static
      // if` with no `else` must not make the whole construct register as
      // `never` when its one branch purely diverges, or later statements
      // in the same block would wrongly become unreachable.
      result = unit;
    }
    return result != k_unknown_type ? result : unit;
  }

  default:
    // Nested declarations and bare expressions in statement position.
    if (const auto *expr = dynamic_cast<const ast::expr *>(&node)) {
      return infer_expr(*expr, expected_tail);
    }
    check_item(node, /*at_module_scope=*/false);
    return unit;
  }
}

} // namespace cinder::semantic::detail
