#include "checker.h"
#include "src/parser/ast_traversal.h"

namespace cinder::semantic::detail {

// ==========================================================================
//  Literals
// ==========================================================================

/// Reports an error when an integer literal's value does not fit in its
/// target builtin type's range (a no-op for non-integer or non-builtin
/// targets, or when the literal's value could not be parsed). When
/// `negated` is set, the literal is the operand of a unary `-` that will
/// be applied to it, so a signed target's range extends one further (a
/// signed type's magnitude of its minimum value is one more than its
/// maximum, e.g. `int8` allows magnitude 128 here for `-128`).
auto checker::check_integer_fit(const ast::literal_expr &lit, type_id target,
                                bool negated) -> void {
  const auto &entry = session_.types_.entry(target);
  if (entry.kind != type_kind::builtin_kind) {
    return;
  }
  auto max_value = integer_max_value(entry.name);
  if (!max_value.has_value()) {
    return;
  }
  const auto is_negative_min = negated && is_signed_integer_name(entry.name);
  if (is_negative_min) {
    *max_value += 1;
  }
  const auto value = parse_integer_literal(lit.value);
  if (value.has_value() && *value <= *max_value) {
    return;
  }
  auto diag =
      diagnostic(diagnostic_level::error,
                 std::format("integer literal `{}{}` does not fit in `{}`",
                             negated ? "-" : "", lit.value, entry.name),
                 file_.file_id_);
  diag.with_label(lit.span, std::format("too large for `{}`", entry.name));
  if (is_negative_min) {
    diag.with_note(std::format("the most negative `{}` value is -{}",
                               entry.name, *max_value));
  } else {
    diag.with_note(
        std::format("the largest `{}` value is {}", entry.name, *max_value));
  }
  diag.with_help("Use a wider integer type, or reduce the value.");
  emit_diag(diag);
  mark_error();
}

/// An integer literal nothing has yet said the type of.
///
/// Before it, this returned
/// `int32` on the spot — a decision made at the leaf, from no evidence,
/// and irrevocable. `int32` is usually right, which is exactly what made
/// it expensive: every place it was wrong became its own repair.
///
/// Now the literal mints a leaf and raises a `defaulting` obligation whose
/// candidate is `int32`. Real constraints — a later argument, a return, an
/// annotated binding — reach `type_mismatch` and solve it. `int32` is
/// applied only once the fixpoint has stalled, which is the whole meaning
/// of "last resort" and the reason the rule is now visible in a diagnostic
/// rather than implicit in the order the checker happened to walk.
///
/// Only when `expected` says *nothing*. An expected type that is wrong for
/// an integer (a struct, say) is still evidence — it is the mistake about
/// to be reported — so it keeps the old answer and the old message.
auto checker::open_integer_literal(const ast::literal_expr &lit,
                                   type_id expected, bool negated) -> type_id {
  if (!session_.types_.is_unknown(expected)) {
    const auto fallback = session_.types_.builtin("int32");
    check_integer_fit(lit, fallback, negated);
    return fallback;
  }
  const auto leaf = leaf_ctxt_.fresh_type(
      "the type of this integer literal",
      source_location{.file_id = file_.file_id_, .span = lit.span});
  integer_literal_leaves_.insert(leaf);
  pending_leaf_values_.push_back(
      pending_leaf_literal_value{.literal = &lit,
                                 .leaf = leaf,
                                 .negated = negated,
                                 .file = file_.file_id_});
  leaf_queue_.add(infer::obligation{
      .kind = infer::obligation_kind::defaulting,
      .watches = {leaf},
      .goal = std::format("a type for the integer literal `{}`", lit.value),
      .why = infer::k_no_cause,
      .payload = static_cast<uint64_t>(session_.types_.builtin("int32")),
  });
  return leaf;
}

/// Types a literal expression. An integer or float literal adopts the
/// expected numeric type when one is given (checking integer fit),
/// otherwise defaults to `int32`/`float64` per the language's literal
/// defaulting rule. `negated` is set when this literal is the immediate
/// operand of a unary `-`, widening the accepted range for a signed
/// target's minimum value (see `check_integer_fit`).
auto checker::infer_literal(const ast::literal_expr &lit, type_id expected,
                            bool negated) -> type_id {
  switch (lit.lit_kind) {
  case token_kind::int_lit: {
    const auto stripped = strip_refs(expected);
    if (session_.types_.is_integer(stripped)) {
      check_integer_fit(lit, stripped, negated);
      return stripped;
    }
    if (session_.types_.is_float(stripped)) {
      return stripped;
    }
    // A type parameter of the body being checked (`return 7` in a `def
    // f[T]() -> T`) is not evidence the literal should default to `int32`:
    // the literal has to fit every type `T` can be (phase 9, rule 4).
    //
    // Only the body's *own* parameter (`is_rigid_param`): at an ordinary
    // call site like `identity(7)`, `expected` is also a type param —
    // `identity`'s own unsolved `T` — but there this literal's concrete
    // type (`int32`) is exactly what the call solves `T` from.
    if (is_rigid_param(stripped)) {
      return check_literal_as_param(lit, stripped, negated);
    }
    return open_integer_literal(lit, stripped, negated);
  }
  case token_kind::float_lit: {
    const auto stripped = strip_refs(expected);
    if (session_.types_.is_float(stripped)) {
      return stripped;
    }
    if (is_rigid_param(stripped)) {
      return check_literal_as_param(lit, stripped, negated);
    }
    return session_.types_.builtin("float64");
  }
  case token_kind::string_lit:
    return session_.types_.builtin("str");
  case token_kind::char_lit:
    return session_.types_.builtin("char");
  case token_kind::kw_true:
  case token_kind::kw_false:
    return session_.types_.builtin("bool");
  case token_kind::kw_unit:
    return session_.types_.builtin("unit");
  default:
    return k_unknown_type;
  }
}

// ==========================================================================
//  Binary and unary operators
// ==========================================================================

/// Resolves `method`'s return type the way it would have resolved *inside
/// its own impl* — reinstating that impl's `self.output`-style associated
/// type bindings (`session_.impl_assoc_types_`, keyed by `(target,
/// trait_name)`) around an ordinary `signature_return_type` call. Without this,
/// a method looked up from outside its impl (an operator overload's dispatch)
/// would see `function_.self_assoc_types_` as whatever it happened to be left
/// at — empty, most of the time — and `self.output` would resolve to `unknown`.
/// `trait[arg]` as `session_.impl_assoc_types_` files a parameterized trait's
/// bindings — the key that tells two impls of the same trait apart. Built
/// from the resolved argument types so the impl side and the use site
/// spell it the same way regardless of how each wrote them.
auto checker::parameterized_trait_key(std::string_view trait_name,
                                      type_id argument) -> std::string {
  return std::format("{}[{}]", trait_name, session_.types_.display(argument));
}

/// The same key, read off an `impl trait[args] for ...` declaration.
/// Falls back to the bare trait name when the trait takes no arguments.
auto checker::impl_trait_arguments_key(const ast::impl_decl &decl,
                                       std::string_view trait_name,
                                       const resolve_ctx &ctx) -> std::string {
  if (decl.trait_type == nullptr ||
      decl.trait_type->kind != ast::node_kind::named_type) {
    return std::string(trait_name);
  }
  const auto &named = ast::cast<const ast::named_type>(*decl.trait_type);
  const auto *argument = named.type_args.size() == 1
                             ? ast::as<const ast::type_expr>(
                                   named.type_args.front().value.get())
                             : nullptr;
  if (argument == nullptr) {
    return std::string(trait_name);
  }
  return parameterized_trait_key(trait_name, resolve_type(*argument, ctx));
}

auto checker::resolve_operator_return_type(type_id target,
                                           std::string_view trait_name,
                                           const method_entry &method)
    -> type_id {
  const auto target_it = session_.impl_assoc_types_.find(target);
  if (target_it == session_.impl_assoc_types_.end()) {
    return signature_return_type(*method.decl, method.owner);
  }
  const auto trait_it = target_it->second.find(std::string(trait_name));
  if (trait_it == target_it->second.end()) {
    return signature_return_type(*method.decl, method.owner);
  }
  const auto saved_assoc = function_.self_assoc_types_;
  function_.self_assoc_types_ = trait_it->second;
  const auto result = signature_return_type(*method.decl, method.owner);
  function_.self_assoc_types_ = saved_assoc;
  return result;
}

/// For a user struct/sum/opaque operand, requires it to implement
/// `trait_name` (reporting a missing-impl error with an `impl`/`deriving`
/// hint otherwise); returns `unknown` for anything else. When `binary` is
/// an arithmetic operator being dispatched to a real overload method (see
/// `ast::overload_trait_of`), also records an `session_.operator_dispatches_` entry
/// so `hir::lower_binary` emits a call to that method instead of a raw numeric
/// opcode, and returns the method's real (impl-specific) return type rather
/// than `unknown`. `wire_dispatch` is true for `==`/`!=` too (dispatches to
/// `eq`), but false for ordering comparisons (`<`/`<=`/
/// `>`/`>=`), which always yield `bool` regardless of this function's
/// return value and have no overload-method dispatch of their own yet.
auto checker::require_operand_trait(const ast::binary_expr &binary,
                                    type_id operand,
                                    std::string_view trait_name,
                                    std::string_view op_name,
                                    bool wire_dispatch) -> type_id {
  const auto target = strip_refs(operand);
  const auto entry = session_.types_.entry(target);
  if (entry.kind == type_kind::struct_kind ||
      entry.kind == type_kind::sum_kind ||
      entry.kind == type_kind::opaque_kind) {
    if (type_has_trait(entry, trait_name)) {
      if (wire_dispatch && binary.lhs != nullptr) {
        if (const auto *method = find_method(entry, trait_name, target);
            method != nullptr && !method->decl->params.empty() &&
            param_name_of(method->decl->params.front()) == "self") {
          // An operator whose overload comes from an impl over a generic
          // target needs that impl compiled for *this* operand type before
          // the dispatch can name anything real — see
          // `instantiate_impl_method_for`. Without the instance,
          // `hir::lower_binary` composes `wrap::eq`, which is the
          // uncompiled template.
          const auto *callee =
              instantiate_impl_method_for(binary, *method, entry, target);
          session_.operator_dispatches_[&binary] = resolved_callee{
              .decl = callee != nullptr ? callee : method->decl,
              .owner_module = method->owner->module_name,
              .impl_target_type = callee != nullptr ? "" : entry.name,
              .receiver = binary.lhs.get()};
          return resolve_operator_return_type(target, trait_name, *method);
        }
      }
      return k_unknown_type; // operator impl decides the result type
    }
    error_with_help(binary.span,
                    std::format("operator `{}` is not defined for type `{}`",
                                op_name, entry.name),
                    std::format("`{}` does not implement the `{}` trait",
                                entry.name, trait_name),
                    std::format("Add `impl {} for {}` (or `deriving {}` where "
                                "applicable) to support this operator.",
                                trait_name, entry.name, trait_name));
    return k_error_type;
  }
  if (entry.kind == type_kind::builtin_kind) {
    // Only comparisons reach here with `eq`/`ord`; every other trait is an
    // arithmetic operator's (`ast::overload_trait_of`).
    const auto arithmetic = trait_name != "eq" && trait_name != "ord";
    // A builtin number has every operator primitively, and `bool`/`char`
    // have the comparisons. Their `impl eq`/`impl add`/...
    // (`traits.scalar.cn`) are written *with* that operator, so
    // dispatching to the impl would call itself forever.
    if (session_.types_.is_numeric(target)) {
      return k_unknown_type;
    }
    if (session_.types_.is_boolean(target) || entry.name == "char") {
      if (!arithmetic) {
        return k_unknown_type;
      }
      report_non_numeric_arithmetic(binary, op_name, target);
      return k_error_type;
    }
    if (wire_dispatch && binary.lhs != nullptr) {
      // An extension method on the builtin type, or an impl on it
      // (`impl add for str`).
      if (const auto *method = resolve_method(entry, target, trait_name).method;
          method != nullptr && !method->decl->params.empty() &&
          param_name_of(method->decl->params.front()) == "self") {
        session_.operator_dispatches_[&binary] =
            resolved_callee{.decl = method->decl,
                            .owner_module = method->owner->module_name,
                            .impl_target_type = entry.name,
                            .receiver = binary.lhs.get()};
        return resolve_operator_return_type(target, trait_name, *method);
      }
    }
    if (arithmetic) {
      // No impl or extension supplies the operator, and lowering has no
      // primitive one to fall back on.
      report_non_numeric_arithmetic(binary, op_name, target);
      return k_error_type;
    }
    return k_unknown_type; // comparisons of other builtins are primitive
  }
  return k_unknown_type;
}

/// `true + true`, `'a' * 2`, `"a" - "b"`: an arithmetic operator on a
/// builtin that has neither the operator primitively nor an impl of its
/// trait.
auto checker::report_non_numeric_arithmetic(const ast::binary_expr &binary,
                                            std::string_view op_name,
                                            type_id operand) -> void {
  const auto shown = session_.types_.display(operand);
  error_with_help(
      binary.span,
      std::format("operator `{}` requires numeric operands, found `{}`",
                  op_name, shown),
      std::format("`{}` is not a number and does not implement `{}`", shown,
                  ast::overload_trait_of(binary.op)),
      session_.types_.is_boolean(operand)
          ? std::string("Combine `bool` values with `and`/`or`/`not`, or "
                        "convert explicitly, e.g. `int32(flag)`.")
          : std::format("Convert the operand to a number first, e.g. "
                        "`int32(...)`, or use a type that implements `{}`.",
                        ast::overload_trait_of(binary.op)));
}

/// Types an arithmetic operator (`+`, `-`, `*`, `/`, `%` and their
/// wrapping/saturating forms): both operands must be the same numeric
/// type, or (for non-numeric operands) must implement the operator's
/// overload trait. Reports a mismatched-numeric-types error rather than
/// converting either side, since Cinder never converts numbers implicitly.
/// Arithmetic whose operands are both still the leaf `open`: the result is
/// that leaf, and the operator is chosen once it settles.
///
/// Checked again then exactly as it would have been now — a number needs
/// nothing further; anything else must implement the operator's trait,
/// which also wires the dispatch (`str + str` reaches `str::add`). A
/// literal's leaf never reaches the second case: its default is a number,
/// and a non-number meeting it is refused where they meet.
auto checker::defer_open_arithmetic(const ast::binary_expr &binary,
                                    type_id open) -> type_id {
  defer_method_call(
      std::format("the `{}` operator", ast::binary_op_name(binary.op)), open,
      std::vector<type_id>{}, [this, &binary](type_id settled) -> void {
        const auto operand = base_shape(settled);
        if (mentions_type_param(operand)) {
          defer_operator_dispatch(binary, operand, operator_kind::arithmetic);
          return;
        }
        if (session_.types_.is_numeric(operand) ||
            session_.types_.is_unknown(operand)) {
          return;
        }
        const auto op_name = ast::binary_op_name(binary.op);
        const auto trait_name = ast::overload_trait_of(binary.op);
        if (!trait_name.empty()) {
          (void)require_operand_trait(binary, operand, trait_name, op_name,
                                      /*wire_dispatch=*/true);
          return;
        }
        error(binary.span,
              std::format("operator `{}` requires numeric operands, found `{}`",
                          op_name, session_.types_.display(operand)),
              "not a numeric value");
      });
  return open;
}

/// A template's operator whose implementation depends on its parameters —
/// `a + b` on a `T` is the builtin add for `int32` and `money::add` for
/// `money` — wired by each instance for its own operand type.
auto checker::defer_operator_dispatch(const ast::binary_expr &binary,
                                      type_id lhs, operator_kind kind) -> void {
  if (function_.current_template_ == nullptr || !mentions_template_param(lhs)) {
    return;
  }
  defer_to_instances(
      binary, [this, &binary, lhs, kind](instance_subst &subst) -> void {
        const auto &clone = *clone_of(subst, &binary);
        const auto substituted = substitute_type(lhs, subst);
        const auto operand = base_shape(substituted);
        if (session_.types_.is_unknown(operand) ||
            mentions_template_param(operand)) {
          return;
        }
        const auto op_name = ast::binary_op_name(clone.op);
        // Bounds refuse a `&int32` for `T: ord` at the call, but not every
        // instance is reached through a checked bound (a method's own
        // `where` is not checked at its call yet). Lowered, the operator
        // would work on the addresses, so the instance refuses it here.
        if (needs_explicit_deref(substituted)) {
          const auto shown = session_.types_.display(substituted);
          error_with_help(
              clone.span,
              std::format("operator `{}` is not defined for `{}`", op_name,
                          shown),
              std::format("this instance has `{}` operands", shown),
              std::format("`{}` is the address of a number, and the "
                          "operator would work on the address. Instantiate "
                          "with the number itself: dereference the "
                          "reference with `*` where it is produced.",
                          shown));
          return;
        }
        switch (kind) {
        case operator_kind::arithmetic: {
          const auto trait_name = ast::overload_trait_of(clone.op);
          if (!session_.types_.is_numeric(operand) && !trait_name.empty()) {
            (void)require_operand_trait(clone, operand, trait_name, op_name,
                                        /*wire_dispatch=*/true);
          }
          return;
        }
        case operator_kind::equality:
          (void)require_operand_trait(clone, operand, "eq", op_name,
                                      /*wire_dispatch=*/true);
          wire_str_equality_dispatch(clone, operand);
          return;
        case operator_kind::ordering:
          (void)require_operand_trait(clone, operand, "ord", op_name,
                                      /*wire_dispatch=*/false);
          wire_ord_dispatch(clone, operand);
          return;
        }
      });
}

auto checker::infer_arithmetic(const ast::binary_expr &binary, type_id expected)
    -> type_id {
  // Operands participate as their *base* type: a `positive` is an `int32`
  // to `+`, and `p + 1` is an `int32`, not a `positive` — arithmetic does
  // not carry a predicate through it, and pretending otherwise would be
  // unsound (`p + 1` overflows to a negative). Widening is free; this is
  // where it is free (`spec/dependent-types-design.md` §3.1).
  // A type parameter of the body being checked isn't `is_numeric`, but it
  // still has to reach `infer_literal` as the expected type, not collapse
  // to `k_unknown_type`: only there is a literal checked against every
  // type `T` can be instead of defaulting to `int32`. Only the body's own
  // parameter (`is_rigid_param`): at an ordinary call site the expected
  // type can also be the callee's unsolved `T`, and there a literal
  // operand's concrete type is exactly what solves it — see the matching
  // guard in `infer_literal`.
  const auto stripped_expected = strip_refs(expected);
  const auto numeric_expected = session_.types_.is_numeric(stripped_expected) ||
                                        is_rigid_param(stripped_expected)
                                    ? base_shape(expected)
                                    : k_unknown_type;
  // Kept undecorated as well as stripped. `base_shape` drops the `&`, and
  // for an aggregate that is exactly right — its value already is the
  // address, so `&p + &q` is `p + q`. For a number it is not: the `&` is a
  // materialized address, and stripping it here compiles an add on the
  // pointer. See `type_table::reference_is_transparent`.
  const auto raw_lhs = binary.lhs != nullptr
                           ? infer_expr(*binary.lhs, numeric_expected)
                           : k_unknown_type;
  const auto lhs = base_shape(raw_lhs);
  const auto rhs_expected =
      session_.types_.is_numeric(lhs) ||
              session_.types_.entry(lhs).kind == type_kind::type_param_kind
          ? lhs
          : numeric_expected;
  const auto raw_rhs = binary.rhs != nullptr
                           ? infer_expr(*binary.rhs, rhs_expected)
                           : k_unknown_type;
  auto rhs = base_shape(raw_rhs);
  // The operands are tied together first (`a / b` says they agree). What
  // is still open after that is *not* defaulted here: `let b = a + 2`
  // followed by `let c: int64 = b` says what `a` is, one statement later
  // (phase 11, defect 2). The operator is chosen from the operand type,
  // so that choice waits — see `defer_open_arithmetic` below.
  auto lhs_settled = lhs;
  if (leaf_ctxt_.meta_count() != 0) {
    solve_leaves(lhs_settled, rhs);
    lhs_settled = leaf_ctxt_.zonk(lhs_settled);
    rhs = leaf_ctxt_.zonk(rhs);
  }
  const auto lhs_final = lhs_settled;
  const auto op_name = ast::binary_op_name(binary.op);
  if (binary.lhs != nullptr && !require_explicit_deref(*binary.lhs, raw_lhs)) {
    return k_error_type;
  }
  if (binary.rhs != nullptr && !require_explicit_deref(*binary.rhs, raw_rhs)) {
    return k_error_type;
  }

  // An unannotated parameter's leaf is no exception: `x + 1` is the type
  // of `x`, which is what makes it the implicit generic's result type
  // (`link_open_param_call`). The operator is chosen per instance.
  if (leaf_ctxt_.meta_count() != 0) {
    if (mentions_type_var(lhs_final)) {
      return defer_open_arithmetic(binary, lhs_final);
    }
    if (mentions_type_var(rhs)) {
      return defer_open_arithmetic(binary, rhs);
    }
  }

  defer_operator_dispatch(binary, lhs_final, operator_kind::arithmetic);
  // An operand whose type is a type parameter is justified by its bounds,
  // here, rather than waiting for an instance to find out (phase 9).
  if (session_.types_.entry(lhs_final).kind == type_kind::type_param_kind ||
      session_.types_.entry(rhs).kind == type_kind::type_param_kind) {
    return check_bound_arithmetic(binary, lhs_final, rhs);
  }
  if (session_.types_.is_unknown(lhs_final) ||
      session_.types_.is_unknown(rhs)) {
    return session_.types_.is_numeric(lhs_final) ? lhs_final
           : session_.types_.is_numeric(rhs)     ? rhs
                                                 : k_unknown_type;
  }

  const auto trait_name = ast::overload_trait_of(binary.op);
  if (!session_.types_.is_numeric(lhs_final)) {
    if (!trait_name.empty()) {
      return require_operand_trait(binary, lhs_final, trait_name, op_name,
                                   /*wire_dispatch=*/true);
    }
    error(binary.span,
          std::format("operator `{}` requires numeric operands, found `{}`",
                      op_name, session_.types_.display(lhs_final)),
          "not a numeric value");
    return k_error_type;
  }
  if (!session_.types_.is_numeric(rhs)) {
    error(binary.rhs->span,
          std::format("operator `{}` requires numeric operands, found `{}`",
                      op_name, session_.types_.display(rhs)),
          "not a numeric value");
    return k_error_type;
  }
  if (lhs_final != rhs) {
    auto diag =
        diagnostic(diagnostic_level::error,
                   std::format("mismatched numeric types `{}` and `{}` in `{}` "
                               "expression",
                               session_.types_.display(lhs_final),
                               session_.types_.display(rhs), op_name),
                   file_.file_id_);
    diag.with_label(binary.span, "operands must have the same numeric type");
    diag.with_help(std::format(
        " Cinder never converts numbers implicitly; convert one side "
        "explicitly, e.g. `{}(...)`.",
        session_.types_.display(lhs_final)));
    emit_diag(diag);
    mark_error();
    return k_error_type;
  }
  return lhs_final;
}

/// Types `==`/`!=` (`is_equality`) or `<`/`<=`/`>`/`>=`: operands must be
/// mutually compatible types, and (for a user struct/sum operand) must
/// implement `eq`/`ord`. Always yields `bool`. For `==`/`!=` against a
/// struct/sum/opaque operand, also wires a real `eq` method dispatch
/// (mirrors arithmetic operators); `ord` has no dispatch of its own yet
/// (`<`/`<=`/`>`/`>=` would need to translate `cmp()`'s `ordering` result,
/// not just call through).
auto checker::infer_comparison(const ast::binary_expr &binary, bool is_equality)
    -> type_id {
  // Comparison, like arithmetic, sees a refined value as its base — an
  // `index[n]` compares against a `usize` without ceremony, and the trait
  // lookup below (`ord`/`eq`) is asked of `usize`, which is what actually
  // implements it.
  const auto raw_lhs = binary.lhs != nullptr
                           ? infer_expr(*binary.lhs, k_unknown_type)
                           : k_unknown_type;
  auto lhs = base_shape(raw_lhs);
  const auto raw_rhs =
      binary.rhs != nullptr ? infer_expr(*binary.rhs, lhs) : k_unknown_type;
  auto rhs = base_shape(raw_rhs);
  // The operands are one type (`i == 7` says `7` is whatever `i` is), so
  // their leaves are tied now. A later use that solves `i` then solves the
  // literal too, instead of leaving it at its `int32` default.
  if (leaf_ctxt_.meta_count() != 0) {
    solve_leaves(lhs, rhs);
    lhs = leaf_ctxt_.zonk(lhs);
    rhs = leaf_ctxt_.zonk(rhs);
  }
  const auto bool_type = session_.types_.builtin("bool");
  // A `&int32` operand is an address, and comparing it compiles a compare
  // of the address — the same trap arithmetic reports (`infer_arithmetic`).
  if (binary.lhs != nullptr && !require_explicit_deref(*binary.lhs, raw_lhs)) {
    return bool_type;
  }
  if (binary.rhs != nullptr && !require_explicit_deref(*binary.rhs, raw_rhs)) {
    return bool_type;
  }
  defer_operator_dispatch(binary, lhs,
                          is_equality ? operator_kind::equality
                                      : operator_kind::ordering);
  const auto rigid_lhs =
      session_.types_.entry(lhs).kind == type_kind::type_param_kind;
  if (rigid_lhs ||
      session_.types_.entry(rhs).kind == type_kind::type_param_kind) {
    const auto subject = rigid_lhs ? lhs : rhs;
    const auto other = rigid_lhs ? rhs : lhs;
    const auto op_name = ast::binary_op_name(binary.op);
    const auto builtin_comparable = domain_all(subject, [&](type_id m) -> bool {
      return is_equality
                 ? session_.types_.entry(m).kind == type_kind::builtin_kind
                 : session_.types_.is_numeric(m) ||
                       session_.types_.entry(m).name == "char";
    });
    if (!builtin_comparable &&
        !require_bound_operator(binary.span, subject,
                                is_equality ? "eq" : "ord", op_name)) {
      return bool_type;
    }
    if (!session_.types_.is_unknown(other) && other != subject) {
      error(binary.span,
            std::format("cannot compare `{}` with `{}`",
                        session_.types_.display(lhs),
                        session_.types_.display(rhs)),
            "operands have different types");
    }
    return bool_type;
  }
  if (session_.types_.is_unknown(lhs) || session_.types_.is_unknown(rhs)) {
    return bool_type;
  }
  if (!session_.types_.compatible(lhs, rhs) &&
      !session_.types_.compatible(rhs, lhs)) {
    error(binary.span,
          std::format("cannot compare `{}` with `{}`",
                      session_.types_.display(lhs),
                      session_.types_.display(rhs)),
          "operands have different types");
    return bool_type;
  }
  const auto trait_name = is_equality ? "eq" : "ord";
  require_operand_trait(binary, lhs, trait_name, ast::binary_op_name(binary.op),
                        /*wire_dispatch=*/is_equality);
  if (is_equality) {
    wire_str_equality_dispatch(binary, lhs);
  } else {
    wire_ord_dispatch(binary, lhs);
  }
  return bool_type;
}

/// Wires `<`/`<=`/`>`/`>=` between two operands implementing `ord` to a
/// real call of their `cmp` method — `require_operand_trait`'s dispatch
/// wiring assumes the operator's own name is the method to call (true for
/// `eq`/`add`/...), which doesn't hold for `ord`: the trait's one method
/// is `cmp`, returning `ordering`, not a same-named boolean method. Uses
/// the same `session_.operator_dispatches_` map so `hir::lower_binary` needs no
/// new bookkeeping to *find* the dispatch, only new bookkeeping to *use* it —
/// translating the returned `ordering` into `bool` per `binary.op` is
/// lowering's job, so `cmp`'s resolved return type is stashed alongside it
/// in `session_.ord_dispatch_result_types_` (lowering has no type-resolution
/// machinery of its own to derive it from `method->decl->return_type`).
auto checker::wire_ord_dispatch(const ast::binary_expr &binary, type_id lhs)
    -> void {
  if (binary.lhs == nullptr) {
    return;
  }
  const auto target = strip_refs(lhs);
  const auto &entry = session_.types_.entry(target);
  const auto is_self_method = [this](const method_entry *method) -> bool {
    return method != nullptr && !method->decl->params.empty() &&
           param_name_of(method->decl->params.front()) == "self";
  };
  const method_entry *method = nullptr;
  // A builtin number or `char` orders primitively. Its `impl ord`
  // (`traits.scalar.cn`) is written *with* `<`, so dispatching `<` to it
  // would call itself forever.
  if (session_.types_.is_numeric(target) ||
      session_.types_.is_boolean(target) ||
      (entry.kind == type_kind::builtin_kind && entry.name == "char")) {
    return;
  }
  if (entry.kind == type_kind::struct_kind ||
      entry.kind == type_kind::sum_kind ||
      entry.kind == type_kind::opaque_kind) {
    method = find_method(entry, "cmp", target);
  } else if (entry.kind == type_kind::builtin_kind) {
    method = resolve_method(entry, target, "cmp").method;
  }
  if (!is_self_method(method)) {
    return;
  }
  // As in `require_operand_trait`: a `cmp` reached through an impl over a
  // generic target only exists once compiled for this operand type.
  const auto *callee =
      instantiate_impl_method_for(binary, *method, entry, target);
  session_.operator_dispatches_[&binary] =
      resolved_callee{.decl = callee != nullptr ? callee : method->decl,
                      .owner_module = method->owner->module_name,
                      .impl_target_type = callee != nullptr ? "" : entry.name,
                      .receiver = binary.lhs.get()};
  session_.ord_dispatch_result_types_[&binary] =
      resolve_operator_return_type(target, "ord", *method);
}

/// Wires `==`/`!=` between two `str` operands to a real call of
/// `std.string`'s `str::eq` extend method (`rt_str_eq`-backed). `str` values
/// are heap blocks, not scalars, so `==` cannot compare them directly.
/// Recorded through
/// the same `session_.operator_dispatches_` map arithmetic operators use;
/// `hir::lower_binary` negates the call's result for `!=` itself (it
/// already has `bin.op`, so no extra bookkeeping is needed here). A
/// no-op for anything but two `str` operands — every other builtin
/// scalar already has real `==`/`!=` codegen.
auto checker::wire_str_equality_dispatch(const ast::binary_expr &binary,
                                         type_id lhs) -> void {
  const auto entry = session_.types_.entry(strip_refs(lhs));
  if (entry.kind != type_kind::builtin_kind || entry.name != "str" ||
      binary.lhs == nullptr) {
    return;
  }
  const auto found = resolve_method(entry, strip_refs(lhs), "eq");
  if (found.origin != method_origin::builtin_extend) {
    return;
  }
  const auto *method = found.method;
  session_.operator_dispatches_[&binary] =
      resolved_callee{.decl = method->decl,
                      .owner_module = method->owner->module_name,
                      .impl_target_type = entry.name,
                      .receiver = binary.lhs.get()};
}

/// Infers `expr` expecting `bool` and reports an error if it isn't —
/// Cinder has no truthiness, so every condition must be an explicit `bool`.
auto checker::require_bool(const ast::expr &expr, std::string_view context)
    -> void {
  const auto found =
      strip_refs(infer_expr(expr, session_.types_.builtin("bool")));
  if (!session_.types_.is_unknown(found) &&
      !session_.types_.is_boolean(found)) {
    error_with_help(
        expr.span,
        std::format("{} must be `bool`, found `{}`", context,
                    session_.types_.display(found)),
        "expected `bool` here",
        " Cinder has no truthiness; write an explicit comparison such as "
        "`x != 0` or `!list.is_empty()`.");
  }
}

/// Reports an error unless `found` names one of the four quote-value
/// types (`expr`/`stmt`/`def_expr`/`type_expr`) — used at a splice site,
/// where the operand must be quoted syntax, not an ordinary value like
/// `~(42)`. `unknown`/`error` are accepted so one upstream gap doesn't
/// cascade into a second, redundant diagnostic here.
/// Human-readable name for a compile-time quote-fragment kind, used in
/// splice-position mismatch diagnostics (e.g. splicing a quoted statement
/// where only a quoted expression is usable).
auto checker::quote_fragment_kind_name(comptime::value_kind kind)
    -> std::string_view {
  switch (kind) {
  case comptime::value_kind::expr_fragment:
    return "a quoted expression";
  case comptime::value_kind::stmt_fragment:
    return "a quoted statement";
  case comptime::value_kind::def_expr_fragment:
    return "a quoted definition";
  case comptime::value_kind::type_expr_fragment:
    return "a quoted type";
  default:
    return "a non-quote value";
  }
}

auto checker::require_quote_value(type_id raw_found, source_span span,
                                  std::string_view context) -> void {
  // A splice operand has to be a type: `~(42)` is an integer literal that
  // nothing else constrains, so its default is owed here.
  const auto found = demand(raw_found);
  if (session_.types_.is_unknown(found)) {
    return;
  }
  const auto &entry = session_.types_.entry(found);
  if (entry.kind == type_kind::builtin_kind &&
      (entry.name == "expr" || entry.name == "stmt" ||
       entry.name == "def_expr" || entry.name == "type_expr")) {
    return;
  }
  error_with_help(
      span,
      std::format("{} must be a quoted fragment (`expr`, `stmt`, or "
                  "`def_expr`), found `{}`",
                  context, session_.types_.display(found)),
      "expected quoted syntax here",
      "`~` splices previously-captured syntax (from a backtick quote "
      "`` `(...)` ``) back into code; it does not evaluate an arbitrary "
      "compile-time value.");
}

/// Dispatches a binary expression to the appropriate typing rule for its
/// operator family: arithmetic, equality/ordering comparison, logical
/// (`and`/`or`, requiring `bool` operands), bitwise/shift (requiring
/// integer operands), membership (`in`/`not in`), ranges, or `|`
/// (bitwise-or on integers; otherwise untyped, since `|` also appears in
/// sum-type/pipeline/pattern syntax this checker does not model).
auto checker::infer_binary(const ast::binary_expr &binary, type_id expected)
    -> type_id {
  switch (binary.op) {
  case ast::binary_op::add:
  case ast::binary_op::sub:
  case ast::binary_op::mul:
  case ast::binary_op::div:
  case ast::binary_op::mod:
  case ast::binary_op::add_wrap:
  case ast::binary_op::sub_wrap:
  case ast::binary_op::mul_wrap:
  case ast::binary_op::add_sat:
  case ast::binary_op::sub_sat:
  case ast::binary_op::mul_sat:
    return infer_arithmetic(binary, expected);

  case ast::binary_op::eq_eq:
  case ast::binary_op::bang_eq:
    return infer_comparison(binary, true);
  case ast::binary_op::lt:
  case ast::binary_op::lt_eq:
  case ast::binary_op::gt:
  case ast::binary_op::gt_eq:
    return infer_comparison(binary, false);

  case ast::binary_op::logical_and:
  case ast::binary_op::logical_or:
    if (binary.lhs != nullptr) {
      require_bool(*binary.lhs, "the left operand of a logical operator");
    }
    if (binary.rhs != nullptr) {
      require_bool(*binary.rhs, "the right operand of a logical operator");
    }
    return session_.types_.builtin("bool");

  case ast::binary_op::shl:
  case ast::binary_op::shr:
  case ast::binary_op::bit_and:
  case ast::binary_op::bit_xor: {
    const auto lhs = binary.lhs != nullptr
                         ? strip_refs(infer_expr(*binary.lhs, expected))
                         : k_unknown_type;
    const auto rhs = binary.rhs != nullptr
                         ? strip_refs(infer_expr(*binary.rhs, lhs))
                         : k_unknown_type;
    for (const auto operand : {lhs, rhs}) {
      if (is_rigid_param(operand) &&
          domain_all(operand, [&](type_id m) -> bool {
            return session_.types_.is_integer(m);
          })) {
        continue;
      }
      if (!session_.types_.is_unknown(operand) &&
          !session_.types_.is_integer(operand)) {
        error(binary.span,
              std::format("operator `{}` requires integer operands, found "
                          "`{}`",
                          ast::binary_op_name(binary.op),
                          session_.types_.display(operand)),
              "not an integer value");
        return k_error_type;
      }
    }
    return session_.types_.is_integer(lhs) ? lhs : rhs;
  }

  case ast::binary_op::in:
  case ast::binary_op::not_in: {
    if (binary.lhs != nullptr) {
      infer_expr(*binary.lhs, k_unknown_type);
    }
    if (binary.rhs != nullptr) {
      infer_expr(*binary.rhs, k_unknown_type);
    }
    return session_.types_.builtin("bool");
  }

  case ast::binary_op::range:
  case ast::binary_op::range_inclusive: {
    // An expected `range[E]` seeds *both* bounds with `E`. Without it the
    // left bound is inferred blind, so `0..n` with `n: usize` takes its
    // element type from the bare literal `0` — an `int32` — and comes out
    // as `range[int32]` even where a `range[usize]` was asked for. That
    // matters now that a range can select an impl (`index[range[usize]]`):
    // an element type decided by which side happens to be a literal would
    // pick the wrong one, or none.
    const auto hint = range_element_type(expected).value_or(k_unknown_type);
    const auto lhs = binary.lhs != nullptr
                         ? strip_refs(infer_expr(*binary.lhs, hint))
                         : k_unknown_type;
    const auto rhs =
        binary.rhs != nullptr
            ? strip_refs(infer_expr(
                  *binary.rhs, session_.types_.is_unknown(hint) ? lhs : hint))
            : k_unknown_type;
    // Both bounds name the one element type, so tie them before choosing:
    // `0..10` is two literal leaves that must agree, and the range is a
    // `range[?a]` whose `?a` a use of the loop variable (or, failing
    // that, the last resort) settles.
    solve_leaves(lhs, rhs);
    const auto open = [this](type_id type) -> bool {
      auto seen = std::unordered_set<type_id>{};
      return mentions_type_var(settle(type), seen);
    };
    const auto element = session_.types_.is_integer(hint)  ? hint
                         : session_.types_.is_integer(lhs) ? lhs
                         : session_.types_.is_integer(rhs) ? rhs
                         : open(lhs)                       ? settle(lhs)
                         : open(rhs)                       ? settle(rhs)
                                                           : k_unknown_type;
    return resolve_range_type(element);
  }

  case ast::binary_op::pipe:
  case ast::binary_op::bit_or: {
    const auto lhs = binary.lhs != nullptr
                         ? strip_refs(infer_expr(*binary.lhs, expected))
                         : k_unknown_type;
    const auto rhs = binary.rhs != nullptr
                         ? strip_refs(infer_expr(*binary.rhs, lhs))
                         : k_unknown_type;
    if (session_.types_.is_integer(lhs) && session_.types_.is_integer(rhs)) {
      return lhs;
    }
    return k_unknown_type; // sum declarations, execution graphs, patterns
  }
  }
  return k_unknown_type;
}

/// Types a unary operator: `-` requires (and preserves) a numeric
/// operand, `not` requires and yields `bool`, `~` requires (and preserves)
/// an integer operand, `*` (deref) unwraps a pointer/reference, and
/// `&`/`&mut` wrap the operand in a reference type.
/// `&p[i]` / `&mut p[i]` where `p` is a raw pointer: the *offset address*,
/// `p + i`, and so a pointer again rather than a borrow
/// (`spec/specification/03-advanced/38-machine-layer.md`: "`&p[i]` is the
/// offset address"). Typing it as `&T` instead left the machine layer
/// with no way to express pointer arithmetic at all — the one thing a
/// raw pointer is for. `nullopt` when this isn't that shape, leaving the
/// ordinary borrow rules to run.
/// An expected `cell[T]`/`cell_mut[T]` wins over it: a single-element
/// view and an offset address are the same address at runtime, and an
/// `index_mut` impl body writing `&mut self.data[i]` against a declared
/// `cell_mut[T]` means the view. Only where nothing else is expected does
/// the machine-layer reading apply.
auto checker::raw_pointer_element_address(const ast::unary_expr &unary,
                                          type_id expected)
    -> std::optional<type_id> {
  if (unary.operand == nullptr ||
      unary.operand->kind != ast::node_kind::index_expr) {
    return std::nullopt;
  }
  if (const auto &expected_entry = session_.types_.entry(expected);
      expected_entry.kind == type_kind::builtin_generic_kind &&
      (expected_entry.name == "cell" || expected_entry.name == "cell_mut")) {
    return std::nullopt;
  }
  const auto &index = ast::cast<const ast::index_expr>(*unary.operand);
  if (index.object == nullptr) {
    return std::nullopt;
  }
  const auto object =
      strip_refs(base_shape(infer_expr(*index.object, k_unknown_type)));
  const auto &entry = session_.types_.entry(object);
  if (entry.kind != type_kind::ptr_kind) {
    return std::nullopt;
  }
  // The offset address carries the pointer's own mutability: `&p[i]` on a
  // `*mut T` is a `*mut T`, on a `*T` a `*T`. `&` and `&mut` agree here
  // because neither is a borrow — an address into raw memory is as
  // writable as the pointer it came from, no more and no less.
  return session_.types_.ptr_to(entry.result, entry.is_mut);
}

auto checker::infer_unary(const ast::unary_expr &unary, type_id expected)
    -> type_id {
  // `-<int literal>` is checked as a unit rather than inferring the
  // literal generically first: the literal's *magnitude* (e.g. `128`) is
  // one more than its type's positive max when it names that type's
  // minimum value (e.g. `-128` as `int8`), so the fit check must know a
  // negation is about to apply before it runs, not after.
  if (unary.op == ast::unary_op::neg && unary.operand != nullptr) {
    if (const auto *lit =
            ast::as<const ast::literal_expr>(unary.operand.get());
        lit != nullptr && lit->lit_kind == token_kind::int_lit) {
      const auto result_type = infer_literal(*lit, expected, /*negated=*/true);
      record_expr_type(*lit, result_type);
      return result_type;
    }
    // `-<int literal> as <Type>` parses as `-(<int literal> as <Type>)`
    // (`as` binds tighter than unary `-`), so the literal's immediate
    // parent is the cast, not this unary node. Without this, the cast's
    // own operand check (which never sees `negated`) rejects e.g.
    // `-2147483648 as int32` as a positive-value overflow even though
    // `-2147483648` is exactly `int32::min`.
    if (const auto *cast =
            ast::as<const ast::cast_expr>(unary.operand.get());
        cast != nullptr && cast->operand != nullptr) {
      if (const auto *lit =
              ast::as<const ast::literal_expr>(cast->operand.get());
          lit != nullptr && lit->lit_kind == token_kind::int_lit) {
        const auto target =
            cast->target_type != nullptr
                ? resolve_type(*cast->target_type, current_resolve_ctx())
                : expected;
        const auto result_type = infer_literal(*lit, target,
                                               /*negated=*/true);
        record_expr_type(*lit, result_type);
        record_expr_type(*cast, result_type);
        return result_type;
      }
    }
  }
  const auto operand =
      unary.operand != nullptr
          ? infer_expr(*unary.operand, unary.op == ast::unary_op::neg
                                           ? expected
                                           : k_unknown_type)
          : k_unknown_type;
  const auto stripped = strip_refs(operand);

  switch (unary.op) {
  case ast::unary_op::neg:
    if (session_.types_.entry(stripped).kind == type_kind::type_param_kind) {
      if (domain_all(stripped, [&](type_id m) -> bool {
            return session_.types_.is_float(m) ||
                   is_signed_integer_name(session_.types_.entry(m).name);
          })) {
        return stripped;
      }
      const auto fact =
          require_bound_operator(unary.span, stripped, "neg", "-");
      if (!fact.has_value()) {
        return k_error_type;
      }
      const auto output = bound_assoc_type(*fact, stripped, "output");
      return output == k_unknown_type ? stripped : output;
    }
    if (!session_.types_.is_unknown(stripped) &&
        !session_.types_.is_numeric(stripped)) {
      error(unary.span,
            std::format("unary `-` requires a numeric operand, found `{}`",
                        session_.types_.display(stripped)),
            "not a numeric value");
      return k_error_type;
    }
    return stripped;
  case ast::unary_op::logical_not:
    if (!session_.types_.is_unknown(stripped) &&
        !session_.types_.is_boolean(stripped)) {
      error(unary.span,
            std::format("`not` requires a `bool` operand, found `{}`",
                        session_.types_.display(stripped)),
            "expected `bool` here");
    }
    return session_.types_.builtin("bool");
  case ast::unary_op::bit_not:
    if (!session_.types_.is_unknown(stripped) &&
        !session_.types_.is_integer(stripped)) {
      error(unary.span,
            std::format("`~` requires an integer operand, found `{}`",
                        session_.types_.display(stripped)),
            "not an integer value");
    }
    return stripped;
  case ast::unary_op::deref: {
    // What a deref reads is chosen by the operand's head — a borrow, a
    // pointer, a cell — so a head still open is owed its answer now.
    const auto &entry = session_.types_.entry(demand_shape(operand));
    if (entry.kind == type_kind::ptr_kind) {
      require_machine_context(unary.span, "dereferencing a raw pointer");
      return entry.result;
    }
    if (entry.kind == type_kind::ref_kind) {
      // A `&T`/`&mut T` is a checked borrow, not raw memory — never gated.
      return entry.result;
    }
    // `&xs[i]`/`&mut xs[i]` hand back a `cell[T]`/`cell_mut[T]`, which is
    // the element's address, so reading through one is a read of `T`.
    if (entry.kind == type_kind::builtin_generic_kind &&
        (entry.name == "cell" || entry.name == "cell_mut") &&
        entry.args.size() == 1) {
      return entry.args.front();
    }
    // A handle that implements `deref` (spec ch. 18): `*h` reads the
    // target `h.deref()` lends.
    if (unary.operand != nullptr) {
      if (const auto target =
              deref_operator_target(*unary.operand, stripped, unary.span)) {
        return *target;
      }
    }
    if (cannot_be_dereferenced(entry)) {
      report_deref_of_non_reference(unary, stripped);
      return k_error_type;
    }
    return k_unknown_type;
  }
  case ast::unary_op::addr_of: {
    if (const auto offset = raw_pointer_element_address(unary, expected)) {
      return *offset;
    }
    // `&v[i]` on a user type: `v[i]` alone already dispatched to `index`'s
    // `at` (an ordinary read) by the `infer_expr` call above — but an
    // enclosing `&` means a read-only interior reference was wanted
    // instead, which only `index_ref`'s `at_ref` can hand out (a user
    // type has no directly addressable element the way a list/array/slice
    // does). Re-dispatch here rather than in `infer_index`, which has no
    // way to know about this enclosing `&` at the point it runs — the
    // mirror of `addr_of_mut`'s identical `index_mut` re-dispatch below
    // (spec/todo.md item 17).
    //
    // Guarded to skip a range-index read (`v[a..b]`, already typed
    // `slice[T]` by the plain read above): a sub-view is already a
    // complete, already-immutable value with nothing to re-borrow, the
    // same reason `index_ref` documents no impl exists for a range key —
    // `&v[a..b]` should stay a plain `&slice[T]`, not misfire into
    // `index_ref`'s single-element `at_ref` and report a bogus key-type
    // mismatch.
    const auto &stripped_entry_for_index = session_.types_.entry(stripped);
    if (unary.operand != nullptr &&
        unary.operand->kind == ast::node_kind::index_expr &&
        !(stripped_entry_for_index.kind == type_kind::builtin_generic_kind &&
          stripped_entry_for_index.name == "slice")) {
      const auto &index = ast::cast<const ast::index_expr>(*unary.operand);
      if (index.object != nullptr) {
        // Chosen from the head alone; the instance waits for the
        // elements (`dispatch_index_when_settled`).
        const auto object_type =
            demand_shape(base_shape(infer_expr(*index.object, k_unknown_type)));
        const auto &object_entry =
            session_.types_.entry(strip_refs(object_type));
        if (object_entry.kind == type_kind::struct_kind ||
            object_entry.kind == type_kind::sum_kind ||
            object_entry.kind == type_kind::opaque_kind) {
          if (stripped == k_error_type) {
            // The read dispatch this `&` sits on top of already rejected
            // the key (`check_index_key`); re-dispatching against
            // `index_ref` would report the same mismatch a second time in
            // the trait's own words.
            return k_error_type;
          }
          if (type_has_trait(object_entry, "index_ref")) {
            const auto result = require_index_ref_trait(
                index, object_type, object_entry, recorded_key_type(index));
            if (!session_.types_.is_unknown(result)) {
              record_expr_type(index, result);
              // A `cell[T]` *is* the address of the element, so it stands
              // as the whole `&v[i]` — matching `&mut v[i]`'s `cell_mut[T]`
              // result above.
              return result;
            }
          } else if (type_has_trait(object_entry, "index")) {
            error_with_help(
                unary.span,
                std::format("cannot borrow `{}[...]` this way",
                            session_.types_.display(strip_refs(object_type))),
                std::format(
                    "`{}` can be read at an index but has no `index_ref` "
                    "impl",
                    session_.types_.display(strip_refs(object_type))),
                std::format(
                    "Taking an interior reference is a separate capability "
                    "from reading by value. Add:\n\n    impl "
                    "index_ref[usize] for {}:\n"
                    "        def at_ref(self, i: usize) -> cell[...]:\n"
                    "            ...",
                    object_entry.name));
            return k_error_type;
          }
        }
      }
    }
    // `&expr` where a `cell[T]` is expected (an `index_mut`-style trait
    // method's body handing back the single-element view its signature
    // promises) types directly as the `cell[T]` rather than `&T`: the two
    // are the same bare address at runtime (`spec/todo.md` item 19), so
    // there is nothing this would paper over — it just lets the checker
    // record the shape lowering (and the borrow checker's view tracking)
    // already expect instead of a mismatched-but-compatible `&T`.
    const auto &expected_entry = session_.types_.entry(expected);
    if (expected_entry.kind == type_kind::builtin_generic_kind &&
        expected_entry.name == "cell" && expected_entry.args.size() == 1 &&
        session_.types_.compatible(expected_entry.args.front(), stripped)) {
      return expected;
    }
    return session_.types_.ref_to(stripped, false);
  }
  case ast::unary_op::addr_of_mut: {
    if (const auto offset = raw_pointer_element_address(unary, expected)) {
      return *offset;
    }
    // Lending a value mutably hands someone else the right to change it, so
    // nothing known about it survives the loan. The design doc is explicit
    // that narrowing does not flow through an `&mut`
    // (`spec/dependent-types-design.md` §5.1); this is where that is
    // enforced, rather than by trying to track what the callee does.
    if (unary.operand != nullptr) {
      if (const auto *root = assignment_root_ident(*unary.operand)) {
        invalidate_facts(root->name);
      }
      if (const auto handle = place_through_deref(*unary.operand)) {
        report_write_through_deref(unary.span, *handle, "mutably borrow");
        return k_error_type;
      }
    }
    // `&mut container[a..b]` — `infer_index`'s range-slice result is
    // always the immutable `slice[T]` (the same range-index expression
    // written bare, or under a plain `&`, should stay immutable) — but
    // wrapped directly in `&mut`, the intent is a mutable sub-view, so
    // upgrade it to `slice_mut[T]` here rather than teaching `infer_index`
    // about an enclosing-expression context it otherwise has no reason to
    // know about.
    if (unary.operand != nullptr &&
        unary.operand->kind == ast::node_kind::index_expr) {
      const auto &stripped_entry = session_.types_.entry(stripped);
      if (stripped_entry.kind == type_kind::builtin_generic_kind &&
          stripped_entry.name == "slice") {
        const auto element = stripped_entry.args.empty()
                                 ? k_unknown_type
                                 : stripped_entry.args[0];
        return session_.types_.ref_to(
            session_.types_.builtin_generic("slice_mut", {element}), true);
      }
      // `&mut v[i]` on a user type: `v[i]` alone already dispatched to
      // `index`'s `at` (an ordinary read) by the `infer_expr` call above —
      // but an enclosing `&mut` means a mutable interior reference was
      // wanted instead, which only `index_mut`'s `at_mut` can hand out (a
      // user type has no directly addressable element the way a
      // list/array/slice does). Re-dispatch here rather than in
      // `infer_index`, which has no way to know about this enclosing
      // `&mut` at the point it runs.
      const auto &index = ast::cast<const ast::index_expr>(*unary.operand);
      if (index.object != nullptr) {
        // Chosen from the head alone; the instance waits for the
        // elements (`dispatch_index_when_settled`).
        const auto object_type =
            demand_shape(base_shape(infer_expr(*index.object, k_unknown_type)));
        const auto &object_entry =
            session_.types_.entry(strip_refs(object_type));
        if (object_entry.kind == type_kind::struct_kind ||
            object_entry.kind == type_kind::sum_kind ||
            object_entry.kind == type_kind::opaque_kind) {
          if (stripped == k_error_type) {
            // The read dispatch this `&mut` sits on top of already
            // rejected the key (`check_index_key`); re-dispatching against
            // `index_mut` would report the same mismatch a second time in
            // the trait's own words.
            return k_error_type;
          }
          if (type_has_trait(object_entry, "index_mut")) {
            const auto result = require_index_mut_trait(
                index, object_type, object_entry, recorded_key_type(index));
            if (!session_.types_.is_unknown(result)) {
              record_expr_type(index, result);
              // A `cell_mut[T]` *is* the address of the element, so it
              // stands as the whole `&mut v[i]`. Any other view — the
              // `slice_mut[T]` a range impl hands back — is a value, and
              // `&mut` of it is a reference to that value, matching what
              // `&mut arr[a..b]` on a builtin container already answers
              // (`&mut slice_mut[T]`). Without this the two spellings
              // disagree and a `slice_mut[T]` argument fails to solve the
              // callee's `T` against a `&mut slice_mut[T]` parameter.
              const auto &result_entry = session_.types_.entry(result);
              const auto is_cell =
                  result_entry.kind == type_kind::builtin_generic_kind &&
                  (result_entry.name == "cell" ||
                   result_entry.name == "cell_mut");
              return is_cell || result == k_error_type
                         ? result
                         : session_.types_.ref_to(result, true);
            }
          } else if (type_has_trait(object_entry, "index")) {
            error_with_help(
                unary.span,
                std::format("cannot borrow `{}[...]` mutably",
                            session_.types_.display(strip_refs(object_type))),
                std::format(
                    "`{}` can be read at an index but has no `index_mut` "
                    "impl",
                    session_.types_.display(strip_refs(object_type))),
                std::format("Taking a mutable interior reference is a separate "
                            "capability from reading. Add:\n\n    impl "
                            "index_mut[usize] for {}:\n"
                            "        type output_mut = cell_mut[...]\n"
                            "        def at_mut(mut self, i: usize) -> "
                            "self.output_mut:\n            ...",
                            object_entry.name));
            return k_error_type;
          }
        }
      }
    }
    // `&mut expr` where a `cell_mut[T]` is expected — the mutable sibling
    // of the `addr_of` case above.
    const auto &expected_entry = session_.types_.entry(expected);
    if (expected_entry.kind == type_kind::builtin_generic_kind &&
        expected_entry.name == "cell_mut" && expected_entry.args.size() == 1 &&
        session_.types_.compatible(expected_entry.args.front(), stripped)) {
      return expected;
    }
    return session_.types_.ref_to(stripped, true);
  }
  }
  return k_unknown_type;
}

} // namespace cinder::semantic::detail
