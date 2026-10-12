#include "checker.h"

namespace cinder::semantic::detail {

// ==========================================================================
//  Patterns
// ==========================================================================

/// Checks a pattern against the type it is matching (`subject`), binding
/// any names it introduces and reporting shape mismatches (wrong tuple
/// arity, unknown struct field, wrong variant argument count) and literal
/// type mismatches. Recurses into every subpattern regardless of whether
/// `subject` is fully known, so bindings are always produced even when the
/// checker can't fully verify the pattern's shape.
/// Binds the loop variables of a `for` head — statement or comprehension
/// clause — against `element`, the type of one thing the iterable yields.
///
/// One pattern matches the element whole (`for (k, v) in pairs`); several
/// are its positional components (`for k, v in pairs`), which is the same
/// destructuring written without parentheses. The multi-name form used to
/// bind every name to `k_unknown_type` — "could be anything" — so a body
/// that used a loop variable at the wrong type was accepted in silence and
/// then lowered against a type nobody had checked. Splitting an element
/// that is not a tuple, or into the wrong number of pieces, is a mistake
/// worth naming rather than papering over.
auto checker::check_loop_head_patterns(
    const std::vector<const ast::pattern *> &patterns, type_id raw_element)
    -> void {
  if (patterns.empty()) {
    return;
  }
  if (patterns.size() == 1) {
    if (patterns.front() != nullptr) {
      check_pattern(*patterns.front(), raw_element);
    }
    return;
  }

  // Splitting an element is a point of demand: whether it *is* a tuple has
  // to be known to say so.
  const auto element = demand_shape(raw_element);
  const auto element_entry = session_.types_.entry(element);
  const auto is_tuple = element_entry.kind == type_kind::tuple_kind;
  // Point at the loop variables. The enclosing statement's span runs to
  // the end of the body, so using it would underline the loop's last line
  // rather than the names actually at fault.
  auto vars_span = source_span{};
  for (const auto *pattern : patterns) {
    if (pattern == nullptr) {
      continue;
    }
    vars_span = vars_span.empty() ? pattern->span
                                  : source_span{.start = vars_span.start,
                                                .end = pattern->span.end};
  }

  if (is_tuple && element_entry.args.size() != patterns.size()) {
    error_with_help(
        vars_span,
        std::format("this loop binds {} names, but each element of `{}` has {}",
                    patterns.size(), session_.types_.display(element),
                    element_entry.args.size()),
        "the number of loop variables must match the element's arity",
        "write one name per component, or bind the whole element with a "
        "single name and index it");
  } else if (!is_tuple && !session_.types_.is_unknown(element)) {
    error_with_help(
        vars_span,
        std::format("this loop binds {} names, but `{}` is not a tuple",
                    patterns.size(), session_.types_.display(element)),
        "only a tuple element can be split across several loop variables",
        "use a single loop variable here");
  }

  for (size_t i = 0; i < patterns.size(); ++i) {
    if (patterns[i] == nullptr) {
      continue;
    }
    const auto component = is_tuple && i < element_entry.args.size()
                               ? element_entry.args[i]
                               : k_unknown_type;
    check_pattern(*patterns[i], component);
  }
}

auto checker::check_pattern(const ast::pattern &pattern, type_id raw_subject)
    -> void {
  if (pattern.has_error) {
    return;
  }
  // A structural pattern has to know the shape it destructures, so it is a
  // point of demand; a binding or wildcard names the value whatever it is
  // and leaves the type open for a later use to say.
  const auto subject = pattern.kind == ast::node_kind::binding_pattern ||
                               pattern.kind == ast::node_kind::wildcard_pattern
                           ? raw_subject
                           : demand_shape(raw_subject);
  const auto stripped = strip_refs(subject);
  const auto &entry = session_.types_.entry(stripped);
  // Every pattern kind below matches against `stripped` — recording it
  // uniformly here (rather than in each branch) is what lets a later
  // lowering pass ask "what type does this pattern node match?" for any
  // pattern, including ones nested inside tuple/constructor/struct
  // destructuring, without re-deriving it from the AST a second time.
  record_expr_type(pattern, stripped);

  switch (pattern.kind) {
  case ast::node_kind::wildcard_pattern:
    return;
  case ast::node_kind::binding_pattern: {
    const auto &binding = dynamic_cast<const ast::binding_pattern &>(pattern);
    // A binding takes the subject *unstripped*, unlike every structural
    // pattern below. Stripping exists so that destructuring can see
    // through a reference — matching a `&point` against `point { x, y }`
    // has to reach the fields — but a binding does not destructure
    // anything, it names the value it was handed. When that value really
    // is a reference, stripping invented a different type for it:
    // `@some(p)` on an `option[&mut int32]` bound `p` as `int32`, so `*p`
    // had nothing to dereference and reached lowering with no concrete
    // type at all. That is the shape every `iter_mut` yields.
    record_expr_type(pattern, subject);
    bind_value(binding.name, subject,
               binding.is_mut ? binding_origin::mut_binding
                              : binding_origin::pattern_binding,
               binding.span);
    // The name a pattern binds carries its type's facts, exactly as a `let`
    // does. This is what makes `try_from` work: after
    // `@some(i) => ...`, `i` has type `index[8]`, and it is *this* line that
    // turns that type into the usable fact `i < 8`. Mutable bindings are
    // skipped for the same reason a `var` is — see `assume_binding`.
    if (!binding.is_mut) {
      assume_binding(binding.name, subject);
    }
    return;
  }
  case ast::node_kind::literal_pattern: {
    const auto &literal = dynamic_cast<const ast::literal_pattern &>(pattern);
    auto lit = ast::literal_expr{};
    lit.lit_kind = literal.lit_kind;
    lit.value = literal.value;
    lit.span = literal.span;
    const auto found = infer_literal(lit, stripped);
    type_mismatch(literal.span, stripped, found, "from the match subject");
    return;
  }
  case ast::node_kind::tuple_pattern: {
    const auto &tuple = dynamic_cast<const ast::tuple_pattern &>(pattern);
    if (entry.kind == type_kind::tuple_kind &&
        entry.args.size() != tuple.elements.size()) {
      error(tuple.span,
            std::format("tuple pattern has {} element{}, but `{}` has {}",
                        tuple.elements.size(),
                        tuple.elements.size() == 1 ? "" : "s",
                        session_.types_.display(stripped), entry.args.size()),
            "pattern shape does not match the value");
    } else if (entry.kind == type_kind::array_kind &&
               entry.array_size.has_value() &&
               *entry.array_size != tuple.elements.size()) {
      error(tuple.span,
            std::format("tuple pattern has {} element{}, but `{}` has {}",
                        tuple.elements.size(),
                        tuple.elements.size() == 1 ? "" : "s",
                        session_.types_.display(stripped), *entry.array_size),
            "pattern shape does not match the value");
    }
    for (size_t i = 0; i < tuple.elements.size(); ++i) {
      if (tuple.elements[i] != nullptr) {
        const auto element =
            entry.kind == type_kind::tuple_kind && i < entry.args.size()
                ? entry.args[i]
            : entry.kind == type_kind::array_kind ? entry.result
                                                  : k_unknown_type;
        check_pattern(*tuple.elements[i], element);
      }
    }
    return;
  }
  case ast::node_kind::constructor_pattern: {
    const auto &ctor = dynamic_cast<const ast::constructor_pattern &>(pattern);
    check_constructor_pattern(ctor, stripped);
    return;
  }
  case ast::node_kind::option_pattern: {
    const auto &option = dynamic_cast<const ast::option_pattern &>(pattern);
    auto inner = k_unknown_type;
    if (entry.kind == type_kind::builtin_generic_kind &&
        entry.name == "option" && !entry.args.empty()) {
      inner = entry.args[0];
    }
    if (option.inner != nullptr) {
      check_pattern(*option.inner, inner);
    }
    return;
  }
  case ast::node_kind::result_pattern: {
    const auto &result = dynamic_cast<const ast::result_pattern &>(pattern);
    auto inner = k_unknown_type;
    if (entry.kind == type_kind::builtin_generic_kind &&
        entry.name == "result") {
      const auto slot =
          result.result_kind == ast::option_result_kind::err ? 1u : 0u;
      if (slot < entry.args.size()) {
        inner = entry.args[slot];
      }
    }
    if (result.inner != nullptr) {
      check_pattern(*result.inner, inner);
    }
    return;
  }
  case ast::node_kind::struct_pattern: {
    const auto &struct_pattern =
        dynamic_cast<const ast::struct_pattern &>(pattern);
    for (const auto &field : struct_pattern.fields) {
      if (field.is_rest) {
        continue;
      }
      auto field_type = k_unknown_type;
      if (entry.kind == type_kind::struct_kind) {
        if (const auto found = struct_field_type(entry, field.name)) {
          field_type = *found;
        } else {
          error(field.span,
                std::format("no field `{}` on struct `{}`", field.name,
                            entry.name),
                "unknown field in pattern");
        }
      }
      // A shorthand field (`{x}`, field.pattern == nullptr) has no
      // sub-pattern node to key `session_.node_types_` against, so it's
      // recorded here instead — see `session_.struct_pattern_field_types_`'s
      // doc comment.
      session_.struct_pattern_field_types_[&field] = field_type;
      if (field.pattern != nullptr) {
        check_pattern(*field.pattern, field_type);
      } else if (!field.name.empty()) {
        bind_value(field.name, field_type, binding_origin::pattern_binding,
                   field.span);
      }
    }
    return;
  }
  case ast::node_kind::array_pattern: {
    const auto &array = dynamic_cast<const ast::array_pattern &>(pattern);
    const auto element = element_type_of(stripped, array.span);
    for (const auto &item : array.elements) {
      if (item != nullptr) {
        check_pattern(*item, element);
      }
    }
    return;
  }
  case ast::node_kind::range_pattern: {
    const auto &range = dynamic_cast<const ast::range_pattern &>(pattern);
    if (range.start != nullptr) {
      const auto found = infer_expr(*range.start, stripped);
      type_mismatch(range.start->span, stripped, found,
                    "from the match subject");
    }
    if (range.end != nullptr) {
      const auto found = infer_expr(*range.end, stripped);
      type_mismatch(range.end->span, stripped, found, "from the match subject");
    }
    return;
  }
  case ast::node_kind::ref_pattern: {
    const auto &ref = dynamic_cast<const ast::ref_pattern &>(pattern);
    if (ref.inner != nullptr) {
      check_pattern(*ref.inner, stripped);
    }
    return;
  }
  case ast::node_kind::or_pattern: {
    const auto &alternatives = dynamic_cast<const ast::or_pattern &>(pattern);
    for (const auto &alternative : alternatives.alternatives) {
      if (alternative != nullptr) {
        check_pattern(*alternative, stripped);
      }
    }
    return;
  }
  case ast::node_kind::group_pattern: {
    const auto &group = dynamic_cast<const ast::group_pattern &>(pattern);
    if (group.inner != nullptr) {
      check_pattern(*group.inner, stripped);
    }
    if (group.alias.has_value()) {
      bind_value(*group.alias, stripped, binding_origin::pattern_binding,
                 group.span);
    }
    return;
  }
  default:
    return;
  }
}

/// Checks a `@variant(...)` pattern against the sum type it should match:
/// resolves prelude option/result variants specially, otherwise looks up
/// the variant on `subject`'s sum type and checks the destructured
/// sub-patterns against its declared payload types, reporting an unknown
/// variant or wrong destructuring arity.
auto checker::check_constructor_pattern(const ast::constructor_pattern &ctor,
                                        type_id subject) -> void {
  const auto &entry = session_.types_.entry(subject);

  // Prelude option/result variants in pattern position.
  if (ctor.name == "some" || ctor.name == "none" || ctor.name == "ok" ||
      ctor.name == "err") {
    auto inner = k_unknown_type;
    if (entry.kind == type_kind::builtin_generic_kind) {
      if (entry.name == "option" && !entry.args.empty()) {
        inner = entry.args[0];
      } else if (entry.name == "result") {
        const auto slot = ctor.name == "err" ? 1u : 0u;
        if (slot < entry.args.size()) {
          inner = entry.args[slot];
        }
      }
    }
    for (const auto &arg : ctor.args) {
      if (arg != nullptr) {
        check_pattern(*arg, inner);
      }
    }
    return;
  }

  if (entry.kind == type_kind::sum_kind) {
    const auto *variant = find_variant(entry, ctor.name);
    if (variant == nullptr) {
      auto diag =
          diagnostic(diagnostic_level::error,
                     std::format("unknown variant `@{}` for sum type `{}`",
                                 ctor.name, entry.name),
                     file_.file_id_);
      diag.with_label(ctor.span, "no such variant");
      if (const auto *variants = sum_variants_of(entry)) {
        auto names = std::string{};
        for (const auto &candidate : *variants) {
          if (!names.empty()) {
            names += ", ";
          }
          names += std::format("`@{}`", candidate.name);
        }
        diag.with_note(
            std::format("`{}` declares the variants {}", entry.name, names));
      }
      emit_diag(diag);
      mark_error();
      for (const auto &arg : ctor.args) {
        if (arg != nullptr) {
          check_pattern(*arg, k_unknown_type);
        }
      }
      return;
    }

    const auto payload = variant_payload_types(entry, *variant);
    if (ctor.args.size() != payload.size()) {
      error(ctor.span,
            std::format("variant `@{}` carries {} value{}, but the pattern "
                        "destructures {}",
                        ctor.name, payload.size(),
                        payload.size() == 1 ? "" : "s", ctor.args.size()),
            "pattern shape does not match the variant");
    }
    for (size_t i = 0; i < ctor.args.size(); ++i) {
      if (ctor.args[i] != nullptr) {
        check_pattern(*ctor.args[i],
                      i < payload.size() ? payload[i] : k_unknown_type);
      }
    }
    return;
  }

  // Unknown subject — still resolve nested bindings.
  for (const auto &arg : ctor.args) {
    if (arg != nullptr) {
      check_pattern(*arg, k_unknown_type);
    }
  }
}

// ==========================================================================
//  Match checking and exhaustiveness
// ==========================================================================

/// Whether `pattern` matches every possible value of its subject's type
/// (a wildcard, a plain binding, or an or-pattern with an irrefutable
/// alternative) — such an arm guarantees exhaustiveness regardless of
/// what other arms cover.
auto checker::pattern_is_irrefutable(const ast::pattern &pattern) -> bool {
  switch (pattern.kind) {
  case ast::node_kind::wildcard_pattern:
  case ast::node_kind::binding_pattern:
    return true;
  case ast::node_kind::group_pattern: {
    const auto &group = dynamic_cast<const ast::group_pattern &>(pattern);
    return group.inner != nullptr && pattern_is_irrefutable(*group.inner);
  }
  case ast::node_kind::or_pattern: {
    const auto &alternatives = dynamic_cast<const ast::or_pattern &>(pattern);
    return std::ranges::any_of(
        alternatives.alternatives, [this](const auto &alternative) -> bool {
          return alternative != nullptr && pattern_is_irrefutable(*alternative);
        });
  }
  default:
    return false;
  }
}

/// Records the variant/literal names `pattern` covers (a constructor's
/// variant name, an option/result kind, a boolean literal), recursing
/// into or-pattern alternatives and group-pattern inner patterns.
auto checker::collect_covered_names(const ast::pattern &pattern,
                                    std::unordered_set<std::string> &covered)
    -> void {
  switch (pattern.kind) {
  case ast::node_kind::constructor_pattern:
    covered.insert(
        dynamic_cast<const ast::constructor_pattern &>(pattern).name);
    return;
  case ast::node_kind::option_pattern:
    covered.insert(
        dynamic_cast<const ast::option_pattern &>(pattern).option_kind ==
                ast::option_result_kind::some
            ? "some"
            : "none");
    return;
  case ast::node_kind::result_pattern:
    covered.insert(
        dynamic_cast<const ast::result_pattern &>(pattern).result_kind ==
                ast::option_result_kind::err
            ? "err"
            : "ok");
    return;
  case ast::node_kind::literal_pattern: {
    const auto &literal = dynamic_cast<const ast::literal_pattern &>(pattern);
    if (literal.lit_kind == token_kind::kw_true) {
      covered.insert("true");
    } else if (literal.lit_kind == token_kind::kw_false) {
      covered.insert("false");
    }
    return;
  }
  case ast::node_kind::or_pattern: {
    const auto &alternatives = dynamic_cast<const ast::or_pattern &>(pattern);
    for (const auto &alternative : alternatives.alternatives) {
      if (alternative != nullptr) {
        collect_covered_names(*alternative, covered);
      }
    }
    return;
  }
  case ast::node_kind::group_pattern: {
    const auto &group = dynamic_cast<const ast::group_pattern &>(pattern);
    if (group.inner != nullptr) {
      collect_covered_names(*group.inner, covered);
    }
    return;
  }
  default:
    return;
  }
}

/// Reports a non-exhaustive-match error listing the specific missing
/// variants/values, unless some unguarded arm is irrefutable. Only proves
/// exhaustiveness for sum types, `option`, `result`, and `bool`; any other
/// subject type (including `unknown`) is trusted rather than checked.
auto checker::check_match_exhaustiveness(
    type_id subject, source_span span, const std::vector<ast::match_arm> &arms)
    -> void {
  auto covered = std::unordered_set<std::string>{};
  for (const auto &arm : arms) {
    if (arm.pattern == nullptr || arm.has_error) {
      continue;
    }
    const auto &pattern = dynamic_cast<const ast::pattern &>(*arm.pattern);
    if (arm.guard != nullptr) {
      continue; // guarded arms never guarantee coverage
    }
    if (pattern_is_irrefutable(pattern)) {
      return; // an unconditional arm covers everything
    }
    collect_covered_names(pattern, covered);
  }

  const auto stripped = strip_refs(subject);
  const auto &entry = session_.types_.entry(stripped);

  auto required = std::vector<std::string>{};
  if (entry.kind == type_kind::sum_kind) {
    if (const auto *variants = sum_variants_of(entry)) {
      for (const auto &variant : *variants) {
        required.push_back(variant.name);
      }
    }
  } else if (entry.kind == type_kind::builtin_generic_kind &&
             entry.name == "option") {
    required = {"some", "none"};
  } else if (entry.kind == type_kind::builtin_generic_kind &&
             entry.name == "result") {
    required = {"ok", "err"};
  } else if (session_.types_.is_boolean(stripped)) {
    required = {"true", "false"};
  } else if (entry.kind == type_kind::builtin_kind) {
    error_with_help(
        span,
        std::format("non-exhaustive match on `{}`",
                    session_.types_.display(stripped)),
        "these arms may not cover every value",
        "Add a final `_ => ...` arm to handle the remaining values.");
    return;
  } else {
    return; // unknown or user types without variants — nothing to prove
  }

  auto missing = std::string{};
  for (const auto &name : required) {
    if (!covered.contains(name)) {
      if (!missing.empty()) {
        missing += ", ";
      }
      missing +=
          (session_.types_.is_boolean(stripped) ? std::format("`{}`", name)
                                                : std::format("`@{}`", name));
    }
  }
  if (missing.empty()) {
    return;
  }
  auto diag =
      diagnostic(diagnostic_level::error,
                 std::format("non-exhaustive match on `{}`: {} not covered",
                             session_.types_.display(stripped), missing),
                 file_.file_id_);
  diag.with_label(span, "missing match arms");
  diag.with_help(std::format(
      "Handle {} explicitly, or add a final `_ => ...` arm.", missing));
  emit_diag(diag);
  mark_error();
}

/// Types a `match` expression or statement: infers the subject (if any),
/// checks each arm's pattern against it, requires a `bool` guard, joins
/// arm result types via `join_branch_type`, and finally checks
/// exhaustiveness. Shared by `match_expr` and `match_stmt` handling.
auto checker::check_match(const ast::expr *subject,
                          const std::vector<ast::match_arm> &arms,
                          type_id expected, source_span span) -> type_id {
  auto subject_type = k_unknown_type;
  if (subject != nullptr) {
    subject_type = strip_refs(infer_expr(*subject, k_unknown_type));
  }

  auto result = expected;
  for (const auto &arm : arms) {
    push_scope();
    if (arm.pattern != nullptr) {
      check_pattern(dynamic_cast<const ast::pattern &>(*arm.pattern),
                    subject_type);
    }
    if (arm.guard != nullptr) {
      require_bool(*arm.guard, "a match guard");
    }
    type_id arm_type;
    source_span body_span;
    if (arm.body_expr != nullptr) {
      arm_type = infer_expr(*arm.body_expr, expected);
      body_span = arm.body_expr->span;
    } else {
      arm_type = check_body_nodes(arm.body_stmts, expected);
      body_span = arm.span;
    }
    result = join_branch_type(result, arm_type, body_span, "`match`");
    pop_scope();
  }

  if (!arms.empty()) {
    check_match_exhaustiveness(subject_type,
                               subject != nullptr ? subject->span : span, arms);
  }
  return result;
}

/// Checks a `static` declaration in whichever of its five forms it takes
/// (binding, assertion, conditional compilation, or either `static for`
/// variant), dispatching on `decl_kind`.
/// Type-checks a `static` declaration, then — for `binding`/`assertion`/
/// `conditional_compilation` — actually evaluates it via `comptime_eval_`
/// (the M2 milestone of the compile-time evaluation subsystem). Only the
/// scalar-arithmetic subset `comptime::evaluator` currently supports
/// evaluates for real; anything wider (a name it doesn't recognize, a
/// forward reference to a `static let` not yet checked in file order, a
/// call) reports its own "not yet supported"/"not a compile-time
/// constant yet" diagnostic from within the evaluator and this function
/// falls back to the old type-check-only behavior for that node so a
/// missing evaluator feature degrades to a diagnostic rather than a
/// crash. `static for` still only type-checks its body once against the
/// element type — iteration is a later milestone.
/// The `checker`-side half of `comptime::evaluator::variant_resolver_fn`
/// (installed on `comptime_eval_` in the constructor): given a node the
/// evaluator is about to evaluate, reports whether `infer_expr` already
/// resolved it as a sum-type variant constructor (bare `@unix` parses to
/// a plain `ident_expr` named `"unix"`, and `@other(s)` to a `call_expr`
/// whose callee is an `ident_expr` named `"other"` — the parser drops the
/// `@` entirely, so this is the only place that distinguishes a variant
/// reference from an ordinary identifier/call). Requires `node` to already
/// have an entry in `session_.node_types_`, which every caller of
/// `comptime_eval_.evaluate(...)` on a full expression tree (`static if`/
/// `static assert` conditions, `static let` initializers) already
/// guarantees by calling `infer_expr`/`require_bool` first.
auto checker::resolve_variant_tag(const ast::node &node)
    -> std::optional<std::pair<std::string, std::string>> {
  const auto it = session_.node_types_.find(&node);
  if (it == session_.node_types_.end()) {
    return std::nullopt;
  }
  const auto &entry = session_.types_.entry(strip_refs(it->second));
  auto tag = std::string{};
  if (const auto *ident = dynamic_cast<const ast::ident_expr *>(&node)) {
    tag = ident->name;
  } else if (const auto *call = dynamic_cast<const ast::call_expr *>(&node)) {
    if (const auto *callee =
            dynamic_cast<const ast::ident_expr *>(call->callee.get())) {
      tag = callee->name;
    }
  }
  if (tag.empty()) {
    return std::nullopt;
  }
  // `option`/`result` are builtin sum types with no declaration.
  const auto builtin_variant =
      entry.kind == type_kind::builtin_generic_kind &&
      ((entry.name == "option" && (tag == "some" || tag == "none")) ||
       (entry.name == "result" && (tag == "ok" || tag == "err")));
  if (entry.kind != type_kind::sum_kind && !builtin_variant) {
    return std::nullopt;
  }
  return std::make_pair(entry.name, std::move(tag));
}

} // namespace cinder::semantic::detail
