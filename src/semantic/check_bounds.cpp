#include "checker.h"

namespace cinder::semantic::detail {

/// The `+`-joined terms of a bound; a bare `Trait[Args]` is the one-term
/// case of the same thing.
auto checker::bound_terms(const ast::type_expr &bound)
    -> std::vector<const ast::type_expr *> {
  auto terms = std::vector<const ast::type_expr *>{};
  if (bound.kind == ast::node_kind::bound_type) {
    for (const auto &term :
         dynamic_cast<const ast::bound_type &>(bound).value.terms) {
      if (term.type != nullptr) {
        terms.push_back(term.type.get());
      }
    }
  } else {
    terms.push_back(&bound);
  }
  return terms;
}

/// The module and file that declare `trait`, so its `requires` clause and
/// its method signatures resolve in their own scope rather than the
/// caller's, and a diagnostic can point at it.
auto checker::trait_home(const ast::trait_decl &trait)
    -> std::pair<const module_members *, file_id_type> {
  for (const auto &[module_name, members] : index_.modules) {
    if (const auto it = members.traits.find(trait.name);
        it != members.traits.end() && it->second.decl == &trait) {
      return {&members, it->second.file_id};
    }
  }
  return {module_, file_id_};
}

/// The concept `name` names from the current module and file, if any —
/// the same scope rules `find_trait_decl_by_name` applies to traits.
auto checker::find_concept_decl_by_name(std::string_view name)
    -> std::optional<concept_decl_ref> {
  const auto key = std::string(name);
  if (module_ != nullptr) {
    if (const auto it = module_->concepts.find(key);
        it != module_->concepts.end()) {
      return it->second;
    }
  }
  if (const auto *binding = find_import(name)) {
    if (const auto *source = import_source_module(*binding)) {
      if (const auto it = source->concepts.find(imported_member_name(*binding));
          it != source->concepts.end()) {
        return it->second;
      }
    }
  }
  for (const auto *source : wildcard_import_sources()) {
    if (const auto it = source->concepts.find(key);
        it != source->concepts.end()) {
      return it->second;
    }
  }
  return find_prelude_concept(name);
}

/// Runs `body` as if checking code in `module` and `file`, so name lookup
/// sees that file's imports.
template <typename Body>
auto checker::in_scope_of(const module_members *module, file_id_type file,
                          Body body) -> decltype(body()) {
  const auto *saved_module = module_;
  const auto saved_file = file_id_;
  module_ = module;
  file_id_ = file;
  struct restore {
    checker *self;
    const module_members *module;
    file_id_type file;
    ~restore() {
      self->module_ = module;
      self->file_id_ = file;
    }
  } guard{this, saved_module, saved_file};
  return body();
}

/// Adds `trait[args]` to `out`, and everything it `requires`, once each. A
/// trait's requirements are facts about the same subject: `T: ord` lets a
/// body call `eq`'s methods too.
auto checker::add_fact_closure(const ast::trait_decl &trait,
                               std::vector<type_id> args,
                               std::vector<type_fact> &out) -> void {
  const auto already =
      std::ranges::any_of(out, [&](const type_fact &known) -> bool {
        return known.trait == &trait && known.args == args;
      });
  if (already) {
    return;
  }
  out.push_back(type_fact{.trait = &trait, .args = args});
  if (!trait.requires_bound.has_value()) {
    return;
  }
  auto bindings = type_scope{};
  for (size_t i = 0; i < trait.type_params.size() && i < args.size(); ++i) {
    bindings.emplace(trait.type_params[i].name, args[i]);
  }
  const auto [home, file] = trait_home(trait);
  const auto ctx = resolve_ctx{.module = home,
                               .param_bindings = &bindings,
                               .use_type_param_stack = false,
                               .quiet = true};
  for (const auto &term : trait.requires_bound->terms) {
    if (term.type == nullptr || term.type->kind != ast::node_kind::named_type) {
      continue;
    }
    const auto &named = dynamic_cast<const ast::named_type &>(*term.type);
    if (named.path.empty()) {
      continue;
    }
    const auto required =
        in_scope_of(home, file, [&] -> std::optional<const ast::trait_decl *> {
          return find_trait_decl_by_name(named.path.back());
        });
    if (required.has_value() && *required != nullptr) {
      add_fact_closure(**required, resolve_type_args(named.type_args, ctx),
                       out);
    }
  }
}

/// Records what `bound`, written in `ctx` and the current module, says
/// about `subject`. A concept contributes the trait bounds in its body, read
/// with its own parameter standing for `subject`.
auto checker::add_bound_facts(type_id subject, const ast::type_expr &bound,
                              const resolve_ctx &ctx) -> void {
  for (const auto *term : bound_terms(bound)) {
    if (term->kind != ast::node_kind::named_type) {
      continue;
    }
    const auto &named = dynamic_cast<const ast::named_type &>(*term);
    if (named.path.empty()) {
      continue;
    }
    if (names_copy(named)) {
      copy_type_params_.insert(subject);
      continue;
    }
    if (const auto trait = find_trait_decl_by_name(named.path.back());
        trait.has_value() && *trait != nullptr) {
      add_fact_closure(**trait, resolve_type_args(named.type_args, ctx),
                       declared_facts_[subject]);
      continue;
    }
    const auto concept_ref = find_concept_decl_by_name(named.path.back());
    if (!concept_ref.has_value() || concept_ref->decl == nullptr) {
      continue;
    }
    const auto &concept_decl = *concept_ref->decl;
    if (const auto members = concept_category(concept_decl)) {
      // A category's members are builtin scalars, so a parameter bounded
      // by one copies (ch. 14, Copy and move).
      if (std::ranges::all_of(*members, [this](type_id member) -> bool {
            return types_.is_copy(member);
          })) {
        copy_type_params_.insert(subject);
      }
      const auto found = declared_domains_.find(subject);
      declared_domains_[subject] =
          found != declared_domains_.end()
              ? intersect_domains(found->second, *members)
              : *members;
    }
    auto bindings = type_scope{};
    const auto args = resolve_type_args(named.type_args, ctx);
    for (size_t i = 0; i < concept_decl.params.size(); ++i) {
      const auto bound_to =
          i == 0 ? subject
                 : (i - 1 < args.size() ? args[i - 1] : k_unknown_type);
      bindings.emplace(concept_decl.params[i].name, bound_to);
    }
    auto home = module_;
    for (const auto &[module_name, members] : index_.modules) {
      if (const auto it = members.concepts.find(concept_decl.name);
          it != members.concepts.end() && it->second.decl == &concept_decl) {
        home = &members;
      }
    }
    const auto concept_ctx = resolve_ctx{.module = home,
                                         .param_bindings = &bindings,
                                         .use_type_param_stack = false,
                                         .quiet = true};
    in_scope_of(home, concept_ref->file_id, [&] -> void {
      for (const auto &constraint : concept_decl.constraints) {
        const auto *constraint_bound = dynamic_cast<const ast::type_expr *>(
            constraint.bound_or_expr.get());
        if (constraint.subject == nullptr || constraint_bound == nullptr) {
          continue;
        }
        const auto constrained = resolve_type(*constraint.subject, concept_ctx);
        if (types_.entry(constrained).kind == type_kind::type_param_kind) {
          add_bound_facts(constrained, *constraint_bound, concept_ctx);
        }
      }
    });
  }
}

/// Whether every term of `bound` names a trait or a concept — what makes
/// `[T: ord]` a bounded type parameter rather than a value parameter.
auto checker::names_trait_or_concept(const ast::type_expr &bound) -> bool {
  const auto terms = bound_terms(bound);
  return !terms.empty() &&
         std::ranges::all_of(terms, [&](const ast::type_expr *term) -> bool {
           if (term->kind != ast::node_kind::named_type) {
             return false;
           }
           const auto &named = dynamic_cast<const ast::named_type &>(*term);
           return !named.path.empty() &&
                  (names_copy(named) ||
                   find_trait_decl_by_name(named.path.back()).has_value() ||
                   find_concept_decl_by_name(named.path.back()).has_value());
         });
}

/// Records the facts a declaration states about its own parameters: their
/// inline bounds, and its `where` clause. Called once the parameters are on
/// the stack. In an instance the parameters are concrete and have nothing
/// to record; a bound there is a fact about a type, not an assumption.
auto checker::record_declared_facts(
    const std::vector<ast::type_param> &params,
    const std::vector<ast::where_constraint> *where_constraints) -> void {
  auto ctx = current_resolve_ctx();
  ctx.quiet = true;
  const auto rigid = [&](type_id id) -> bool {
    return types_.entry(id).kind == type_kind::type_param_kind;
  };
  for (const auto &param : params) {
    // `[T: ord]` parses as a value parameter, since `[n: usize]` has the
    // same shape; which one it is depends on what the name after the colon
    // resolves to (see the same rule for signature bounds in
    // `instantiate_functor`).
    if (param.bound_or_type == nullptr ||
        (param.is_value_param &&
         !names_trait_or_concept(*param.bound_or_type))) {
      continue;
    }
    if (const auto id = lookup_type_param(param.name); id && rigid(*id)) {
      add_bound_facts(*id, *param.bound_or_type, ctx);
    }
  }
  if (where_constraints == nullptr) {
    return;
  }
  for (const auto &constraint : *where_constraints) {
    if (constraint.subject == nullptr || constraint.bound_or_type == nullptr) {
      continue;
    }
    const auto subject = resolve_type(*constraint.subject, ctx);
    if (rigid(subject)) {
      add_bound_facts(subject, *constraint.bound_or_type, ctx);
    }
  }
}

/// The builtin types a category names — the membership `std.traits`'s
/// `is_integer`/`is_float`/... predicates test, minus the 128-bit types.
/// Those have no representation on the bytecode VM, so they have none of
/// the core-trait impls every other member has (`traits.scalar.cn`), and a
/// category bound promises those traits.
auto checker::category_members(std::string_view category)
    -> std::optional<type_domain> {
  static constexpr auto k_signed = std::array<std::string_view, 5>{
      "int8", "int16", "int32", "int64", "isize"};
  static constexpr auto k_unsigned = std::array<std::string_view, 6>{
      "uint8", "uint16", "uint32", "uint64", "usize", "byte"};
  static constexpr auto k_float =
      std::array<std::string_view, 2>{"float32", "float64"};
  auto out = type_domain{};
  const auto add = [&](const auto &names) -> auto {
    for (const auto name : names) {
      out.push_back(types_.builtin(name));
    }
  };
  if (category == "signed_integer" || category == "integer" ||
      category == "numeric") {
    add(k_signed);
  }
  if (category == "unsigned_integer" || category == "integer" ||
      category == "numeric") {
    add(k_unsigned);
  }
  if (category == "float" || category == "numeric") {
    add(k_float);
  }
  if (category == "bool") {
    out.push_back(types_.builtin("bool"));
  }
  if (category == "char") {
    out.push_back(types_.builtin("char"));
  }
  if (category == "str") {
    out.push_back(types_.builtin("str"));
  }
  if (out.empty()) {
    return std::nullopt;
  }
  std::ranges::sort(out);
  return out;
}

/// The category a concept stands for, when it is one of the builtin
/// category concepts `std.traits` declares.
auto checker::concept_category(const ast::concept_decl &concept_decl)
    -> std::optional<type_domain> {
  const auto *traits_module = index_.find_module("std.traits");
  if (traits_module == nullptr) {
    return std::nullopt;
  }
  const auto it = traits_module->concepts.find(concept_decl.name);
  if (it == traits_module->concepts.end() || it->second.decl != &concept_decl) {
    return std::nullopt;
  }
  return category_members(concept_decl.name);
}

/// The type of a method's `self` parameter. `self` is always borrowed
/// (ch. 14): a heap-represented `self` is a `&T`, or a `&mut T` when
/// written `mut self`, so that a borrow of it is the address of the
/// caller's place (`spec/todo.md` item 20). A scalar `self` is passed by
/// value, which reads the same.
auto checker::self_param_type(const ast::param &param) -> type_id {
  if (types_.is_unknown(self_type_) || self_type_ == k_error_type ||
      !types_.is_heap_represented(self_type_)) {
    return self_type_;
  }
  const auto *binding =
      dynamic_cast<const ast::binding_pattern *>(param.pattern.get());
  return types_.ref_to(self_type_, binding != nullptr && binding->is_mut);
}

/// Whether `concept_decl` is `std.traits.copy`, which the checker answers
/// itself (`type_table::is_copy`) rather than from its body.
auto checker::is_copy_concept(const ast::concept_decl &concept_decl) -> bool {
  const auto *traits_module = index_.find_module("std.traits");
  if (traits_module == nullptr || concept_decl.name != "copy") {
    return false;
  }
  const auto it = traits_module->concepts.find("copy");
  return it != traits_module->concepts.end() &&
         it->second.decl == &concept_decl;
}

/// Whether `named`, in a bound, is the built-in `copy` concept: the prelude
/// one, or the bare name when no stdlib is loaded, unless a trait or
/// another concept of that name is in scope.
auto checker::names_copy(const ast::named_type &named) -> bool {
  if (named.path.empty() || named.path.back() != "copy") {
    return false;
  }
  if (const auto trait = find_trait_decl_by_name("copy");
      trait.has_value() && *trait != nullptr) {
    return false;
  }
  const auto found = find_concept_decl_by_name("copy");
  return !found.has_value() || found->decl == nullptr ||
         is_copy_concept(*found->decl);
}

/// Whether `subject` satisfies `copy`. Not through `strip_refs`: `&T` is
/// `copy` whatever `T` is.
auto checker::type_is_copy(type_id subject) -> bool {
  if ((types_.is_unknown(subject) && !is_rigid_param(subject)) ||
      subject == k_error_type) {
    return true;
  }
  return value_is_copy(subject);
}

/// A user sum, `option` or `result`.
auto checker::is_sum_like(type_id id) const -> bool {
  const auto &entry = types_.entry(types_.strip_refinement(id));
  return entry.kind == type_kind::sum_kind ||
         (entry.kind == type_kind::builtin_generic_kind &&
          (entry.name == "option" || entry.name == "result"));
}

/// Whether a value of `id` copies (ch. 14, Copy and move), including a sum
/// whose every payload copies and that has no `drop`: a sum value never
/// changes after it is built, so sharing its block is a copy. `visiting`
/// breaks a recursive sum's cycle by assuming it copies, which holds
/// exactly when every other payload does.
auto checker::value_is_copy(type_id id) -> bool {
  auto visiting = std::unordered_set<type_id>{};
  return value_is_copy(id, visiting);
}

auto checker::value_is_copy(type_id id, std::unordered_set<type_id> &visiting)
    -> bool {
  if (types_.is_copy(id) || copy_type_params_.contains(id)) {
    return true;
  }
  const auto stripped = types_.strip_refinement(id);
  if (!is_sum_like(stripped)) {
    return false;
  }
  if (!visiting.insert(stripped).second) {
    return true;
  }
  const auto &entry = types_.entry(stripped);
  if (entry.kind == type_kind::builtin_generic_kind) {
    return std::ranges::all_of(entry.args, [&](type_id arg) -> bool {
      return !types_.is_unknown(arg) && value_is_copy(arg, visiting);
    });
  }
  if (type_has_trait(entry, "drop")) {
    return false;
  }
  const auto *variants = sum_variants_of(entry);
  if (variants == nullptr) {
    return false;
  }
  for (const auto &variant : *variants) {
    for (const auto payload : variant_payload_types(entry, variant)) {
      if (types_.is_unknown(payload) || !value_is_copy(payload, visiting)) {
        return false;
      }
    }
  }
  return true;
}

auto checker::intersect_domains(const type_domain &a, const type_domain &b)
    -> type_domain {
  auto out = type_domain{};
  std::ranges::set_intersection(a, b, std::back_inserter(out));
  return out;
}

auto checker::subtract_domains(const type_domain &a, const type_domain &b)
    -> type_domain {
  auto out = type_domain{};
  std::ranges::set_difference(a, b, std::back_inserter(out));
  return out;
}

auto checker::unite_domains(const type_domain &a, const type_domain &b)
    -> type_domain {
  auto out = type_domain{};
  std::ranges::set_union(a, b, std::back_inserter(out));
  return out;
}

/// What `param` can still be here, or `nullopt` if nothing restricts it.
auto checker::domain_of(type_id param) -> std::optional<type_domain> {
  auto domain = std::optional<type_domain>{};
  if (const auto it = declared_domains_.find(param);
      it != declared_domains_.end()) {
    domain = it->second;
  }
  for (const auto &[subject, narrowed] : scoped_domains_) {
    if (subject == param) {
      domain =
          domain.has_value() ? intersect_domains(*domain, narrowed) : narrowed;
    }
  }
  return domain;
}

/// Whether `id` is a type parameter of the code being checked, as opposed
/// to a callee's parameter showing through its signature — only the former
/// has facts and a domain here.
auto checker::is_rigid_param(type_id id) -> bool {
  if (types_.entry(id).kind != type_kind::type_param_kind) {
    return false;
  }
  return std::ranges::any_of(type_params_,
                             [&](const auto &scope) -> bool {
                               return std::ranges::any_of(
                                   scope, [&](const auto &entry) -> bool {
                                     return entry.second == id;
                                   });
                             }) ||
         id == self_type_;
}

/// A literal written where a `T` is expected (phase 9, rule 4). Legal when
/// every type `T` can be here could hold it; reported otherwise, with the
/// bound that would make it legal.
auto checker::check_literal_as_param(const ast::literal_expr &lit,
                                     type_id param, bool negated) -> type_id {
  const auto is_int = lit.lit_kind == token_kind::int_lit;
  const auto domain = domain_of(param);
  const auto name = types_.display(param);
  const auto spelled = std::format("{}{}", negated ? "-" : "", lit.value);
  if (!domain.has_value()) {
    error_with_help(
        lit.span,
        std::format("the literal `{}` cannot be a `{}`", spelled, name),
        std::format("`{}` could be any type here", name),
        std::format("A number is only a `{}` if `{}` is known to be a "
                    "number. Bound it by a category — `where {}: {}` — "
                    "or convert explicitly.",
                    name, name, name, is_int ? "numeric" : "float"));
    return k_error_type;
  }
  for (const auto member : *domain) {
    const auto &entry = types_.entry(member);
    const auto accepts =
        is_int ? types_.is_numeric(member) : types_.is_float(member);
    auto fits = accepts;
    if (accepts && is_int && types_.is_integer(member)) {
      auto max_value = integer_max_value(entry.name);
      if (max_value.has_value()) {
        if (negated && is_signed_integer_name(entry.name)) {
          *max_value += 1;
        }
        const auto value = parse_integer_literal(lit.value);
        fits = value.has_value() && *value <= *max_value;
      }
    }
    if (!fits) {
      error_with_help(
          lit.span,
          std::format("the literal `{}` cannot be a `{}` when `{}` is `{}`",
                      spelled, name, name, entry.name),
          std::format("`{}` can be `{}` here", name, entry.name),
          std::format("A literal written as a `{}` has to fit every type "
                      "`{}` can be at this point. Narrow `{}` first (a "
                      "`static if` on it), or tighten its bound.",
                      name, name, name));
      return k_error_type;
    }
  }
  return param;
}

/// `x as U` or `x as T` where a side is a type parameter (phase 9, rule 4):
/// legal when every type the parameter can be here is a number, and the
/// other side is a number too.
auto checker::check_cast_with_param(const ast::cast_expr &cast, type_id operand,
                                    type_id target) -> void {
  const auto numeric_side = [&](type_id side) -> bool {
    if (is_rigid_param(side)) {
      return domain_all(
          side, [&](type_id m) -> bool { return types_.is_numeric(m); });
    }
    return types_.is_unknown(side) || types_.is_numeric(side);
  };
  for (const auto side : {operand, target}) {
    if (!is_rigid_param(side) || numeric_side(side)) {
      continue;
    }
    const auto name = types_.display(side);
    error_with_help(
        cast.span, std::format("`as` needs `{}` to be a number", name),
        std::format("`{}` is not known to be numeric", name),
        std::format("`as` converts between numeric types. Bound `{}` by a "
                    "category — `where {}: numeric` — so every type it can "
                    "be is one.",
                    name, name));
    return;
  }
  if (!numeric_side(operand) || !numeric_side(target)) {
    error(cast.span,
          std::format("`as` cannot convert `{}` to `{}`",
                      types_.display(operand), types_.display(target)),
          "not a numeric conversion");
  }
}

/// The type parameter `expr` reflects on by name — `T.name()`, or a local
/// bound to it — if any.
auto checker::reflected_name_of(const ast::expr &expr)
    -> std::optional<type_id> {
  if (expr.kind == ast::node_kind::ident_expr) {
    const auto &ident = dynamic_cast<const ast::ident_expr &>(expr);
    if (const auto it = type_name_aliases_.find(ident.name);
        it != type_name_aliases_.end()) {
      return it->second;
    }
    return std::nullopt;
  }
  if (expr.kind != ast::node_kind::call_expr) {
    return std::nullopt;
  }
  const auto &call = dynamic_cast<const ast::call_expr &>(expr);
  if (!call.args.empty() || call.callee == nullptr ||
      call.callee->kind != ast::node_kind::field_expr) {
    return std::nullopt;
  }
  const auto &field = dynamic_cast<const ast::field_expr &>(*call.callee);
  if (field.field_name != "name" || field.object == nullptr ||
      field.object->kind != ast::node_kind::ident_expr) {
    return std::nullopt;
  }
  const auto param = lookup_type_param(
      dynamic_cast<const ast::ident_expr &>(*field.object).name);
  if (!param.has_value() || !is_rigid_param(*param)) {
    return std::nullopt;
  }
  return *param;
}

/// The builtin type a string literal names, for `n == "int8"`.
auto checker::builtin_named_by(const ast::expr &expr)
    -> std::optional<type_id> {
  const auto *lit = dynamic_cast<const ast::literal_expr *>(&expr);
  if (lit == nullptr || lit->lit_kind != token_kind::string_lit) {
    return std::nullopt;
  }
  auto text = std::string_view(lit->value);
  if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
    text = text.substr(1, text.size() - 2);
  }
  if (!is_builtin_scalar_name(text)) {
    return std::nullopt;
  }
  return types_.builtin(text);
}

/// Completes a narrowing with its `else` domain, when one can be stated.
auto checker::with_else(type_id param, const type_domain &then_domain)
    -> narrowing {
  auto out = narrowing{
      .param = param, .then_domain = then_domain, .else_domain = std::nullopt};
  if (const auto current = domain_of(param)) {
    out.then_domain = intersect_domains(*current, then_domain);
    out.else_domain = subtract_domains(*current, then_domain);
  }
  return out;
}

/// What `condition` establishes about a single type parameter, or
/// `nullopt` when it is not a condition this compiler reads facts from.
/// The forms read are the ones `std.limits` and `std.traits` are written
/// in: `T.name() == "lit"` (directly or through a `let` alias), `or`,
/// `not`, and the category predicates `is_integer[T]()` and friends.
auto checker::narrowing_of(const ast::expr &condition)
    -> std::optional<narrowing> {
  if (condition.kind == ast::node_kind::group_expr) {
    const auto &group = dynamic_cast<const ast::group_expr &>(condition);
    return group.inner != nullptr ? narrowing_of(*group.inner) : std::nullopt;
  }
  if (condition.kind == ast::node_kind::unary_expr) {
    const auto &unary = dynamic_cast<const ast::unary_expr &>(condition);
    if (unary.op != ast::unary_op::logical_not || unary.operand == nullptr) {
      return std::nullopt;
    }
    auto inner = narrowing_of(*unary.operand);
    if (!inner.has_value() || !inner->else_domain.has_value()) {
      return std::nullopt;
    }
    return narrowing{.param = inner->param,
                     .then_domain = *inner->else_domain,
                     .else_domain = inner->then_domain};
  }
  if (condition.kind == ast::node_kind::binary_expr) {
    const auto &binary = dynamic_cast<const ast::binary_expr &>(condition);
    if (binary.lhs == nullptr || binary.rhs == nullptr) {
      return std::nullopt;
    }
    if (binary.op == ast::binary_op::logical_or ||
        binary.op == ast::binary_op::logical_and) {
      const auto lhs = narrowing_of(*binary.lhs);
      const auto rhs = narrowing_of(*binary.rhs);
      // `is_integer[T]() and bits[T]() <= 64`: the branch runs only when
      // both hold, so it has at least what the side that narrows says. Its
      // `else` gets nothing — the other side failing says nothing about
      // `T`.
      if (binary.op == ast::binary_op::logical_and && (lhs || rhs) &&
          !(lhs && rhs && lhs->param == rhs->param)) {
        const auto &known = lhs ? *lhs : *rhs;
        if (lhs && rhs) {
          return std::nullopt;
        }
        return narrowing{.param = known.param,
                         .then_domain = known.then_domain,
                         .else_domain = std::nullopt};
      }
      if (!lhs || !rhs || lhs->param != rhs->param) {
        return std::nullopt;
      }
      const auto either = binary.op == ast::binary_op::logical_or;
      auto out = narrowing{
          .param = lhs->param,
          .then_domain =
              either ? unite_domains(lhs->then_domain, rhs->then_domain)
                     : intersect_domains(lhs->then_domain, rhs->then_domain),
          .else_domain = std::nullopt};
      if (lhs->else_domain && rhs->else_domain) {
        out.else_domain =
            either ? intersect_domains(*lhs->else_domain, *rhs->else_domain)
                   : unite_domains(*lhs->else_domain, *rhs->else_domain);
      }
      return out;
    }
    if (binary.op == ast::binary_op::eq_eq ||
        binary.op == ast::binary_op::bang_eq) {
      auto param = reflected_name_of(*binary.lhs);
      auto named = builtin_named_by(*binary.rhs);
      if (!param || !named) {
        param = reflected_name_of(*binary.rhs);
        named = builtin_named_by(*binary.lhs);
      }
      if (!param || !named) {
        return std::nullopt;
      }
      auto out = with_else(*param, type_domain{*named});
      if (binary.op == ast::binary_op::bang_eq) {
        if (!out.else_domain.has_value()) {
          return std::nullopt;
        }
        std::swap(out.then_domain, *out.else_domain);
      }
      return out;
    }
    return std::nullopt;
  }
  if (condition.kind == ast::node_kind::call_expr) {
    // `is_integer[T]()`: a generic call whose one bracket argument names
    // an in-scope parameter.
    const auto &call = dynamic_cast<const ast::call_expr &>(condition);
    if (!call.args.empty() || call.callee == nullptr ||
        call.callee->kind != ast::node_kind::index_expr) {
      return std::nullopt;
    }
    const auto &applied = dynamic_cast<const ast::index_expr &>(*call.callee);
    if (applied.object == nullptr || applied.index == nullptr ||
        applied.object->kind != ast::node_kind::ident_expr ||
        applied.index->kind != ast::node_kind::ident_expr) {
      return std::nullopt;
    }
    const auto &predicate =
        dynamic_cast<const ast::ident_expr &>(*applied.object).name;
    const auto param = lookup_type_param(
        dynamic_cast<const ast::ident_expr &>(*applied.index).name);
    if (!param.has_value() || !is_rigid_param(*param) ||
        !predicate.starts_with("is_")) {
      return std::nullopt;
    }
    const auto members = category_members(predicate.substr(3));
    if (!members.has_value()) {
      return std::nullopt;
    }
    return with_else(*param, *members);
  }
  return std::nullopt;
}

auto checker::domain_facts(const type_domain &domain)
    -> std::vector<type_fact> {
  if (domain.empty()) {
    return {};
  }
  if (const auto it = domain_facts_cache_.find(domain);
      it != domain_facts_cache_.end()) {
    return it->second;
  }
  auto facts = std::vector<type_fact>{};
  if (const auto *traits_module = index_.find_module("std.traits")) {
    for (const auto &[name, trait] : traits_module->traits) {
      if (trait.decl == nullptr || !trait.decl->type_params.empty()) {
        continue;
      }
      const auto all = std::ranges::all_of(domain, [&](type_id member) -> bool {
        return satisfies_trait(member, *trait.decl, {});
      });
      if (all) {
        facts.push_back(type_fact{.trait = trait.decl, .args = {}});
      }
    }
  }
  std::ranges::sort(facts, {}, [](const type_fact &fact) -> std::string {
    return fact.trait->name;
  });
  domain_facts_cache_.emplace(domain, facts);
  return facts;
}

/// Everything known about the type parameter `param`.
auto checker::facts_of(type_id param) -> std::vector<type_fact> {
  auto facts = std::vector<type_fact>{};
  if (const auto it = declared_facts_.find(param);
      it != declared_facts_.end()) {
    facts = it->second;
  }
  for (const auto &[subject, scoped] : scoped_facts_) {
    if (subject == param) {
      facts.insert(facts.end(), scoped.begin(), scoped.end());
    }
  }
  if (const auto domain = domain_of(param)) {
    for (const auto &fact : domain_facts(*domain)) {
      const auto known =
          std::ranges::any_of(facts, [&](const type_fact &have) -> bool {
            return have.trait == fact.trait && have.args == fact.args;
          });
      if (!known) {
        facts.push_back(fact);
      }
    }
  }
  return facts;
}

/// Recursively resolves a type-position AST node to an interned `type_id`,
/// dispatching on its concrete kind. `named_type` is the only case with
/// real resolution work (see `resolve_named_type`); the rest just resolve
/// their component types structurally.
auto checker::resolve_type(const ast::type_expr &type, const resolve_ctx &ctx)
    -> type_id {
  if (type.has_error) {
    return k_unknown_type;
  }

  switch (type.kind) {
  case ast::node_kind::named_type:
    return resolve_named_type(dynamic_cast<const ast::named_type &>(type), ctx);
  case ast::node_kind::tuple_type: {
    const auto &tuple = dynamic_cast<const ast::tuple_type &>(type);
    auto elements = std::vector<type_id>{};
    elements.reserve(tuple.elements.size());
    for (const auto &element : tuple.elements) {
      elements.push_back(element != nullptr ? resolve_type(*element, ctx)
                                            : k_unknown_type);
    }
    return types_.tuple_of(std::move(elements));
  }
  case ast::node_kind::slice_type: {
    const auto &slice = dynamic_cast<const ast::slice_type &>(type);
    const auto element = slice.element != nullptr
                             ? resolve_type(*slice.element, ctx)
                             : k_unknown_type;
    return types_.builtin_generic("slice", {element});
  }
  case ast::node_kind::array_type: {
    const auto &array = dynamic_cast<const ast::array_type &>(type);
    const auto element = array.element != nullptr
                             ? resolve_type(*array.element, ctx)
                             : k_unknown_type;
    const auto length = array.size != nullptr
                            ? resolve_length_arg(*array.size, ctx)
                            : k_unknown_type;
    return array_with_length(element, length);
  }
  case ast::node_kind::ref_type: {
    const auto &ref = dynamic_cast<const ast::ref_type &>(type);
    const auto inner =
        ref.inner != nullptr ? resolve_type(*ref.inner, ctx) : k_unknown_type;
    return types_.ref_to(inner, ref.is_mut);
  }
  case ast::node_kind::ptr_type: {
    const auto &ptr = dynamic_cast<const ast::ptr_type &>(type);
    const auto inner =
        ptr.inner != nullptr ? resolve_type(*ptr.inner, ctx) : k_unknown_type;
    return types_.ptr_to(inner, ptr.is_mut);
  }
  case ast::node_kind::mut_type:
    return resolve_mut_type(dynamic_cast<const ast::mut_type &>(type), ctx);
  case ast::node_kind::fn_type: {
    const auto &fn = dynamic_cast<const ast::fn_type &>(type);
    auto params = std::vector<type_id>{};
    params.reserve(fn.param_types.size());
    for (const auto &param : fn.param_types) {
      params.push_back(param != nullptr ? resolve_type(*param, ctx)
                                        : k_unknown_type);
    }
    const auto result = fn.return_type != nullptr
                            ? resolve_type(*fn.return_type, ctx)
                            : types_.builtin("unit");
    return types_.fn_of(std::move(params), result);
  }
  case ast::node_kind::refinement_type: {
    // An *inline* refinement (`x: int32 where self > 0`) — one written with
    // no declaration to name it. A refinement written as a `type`
    // declaration never reaches here; it is minted, nominally, by
    // `make_refinement_type`.
    const auto &refinement = dynamic_cast<const ast::refinement_type &>(type);
    const auto base = refinement.base != nullptr
                          ? resolve_type(*refinement.base, ctx)
                          : k_unknown_type;
    const auto *predicate =
        dynamic_cast<const ast::expr *>(refinement.predicate.get());
    if (predicate == nullptr || predicate->has_error) {
      return base;
    }
    return types_.refinement_of(
        nullptr, std::format("{} where ...", types_.display(base)),
        module_name_, base, {}, predicate);
  }
  case ast::node_kind::quote_type: {
    const auto &quote = dynamic_cast<const ast::quote_type &>(type);
    switch (quote.quote_kind) {
    case ast::quote_fragment_kind::expr:
      return types_.builtin("expr");
    case ast::quote_fragment_kind::stmt:
      return types_.builtin("stmt");
    case ast::quote_fragment_kind::def_expr:
      return types_.builtin("def_expr");
    case ast::quote_fragment_kind::type_expr:
      return types_.builtin("type_expr");
    case ast::quote_fragment_kind::none:
      return k_unknown_type;
    }
  }
  case ast::node_kind::splice_type: {
    const auto &splice = dynamic_cast<const ast::splice_type &>(type);
    return resolve_splice_type(splice, ctx);
  }
  case ast::node_kind::existential_type:
    return resolve_existential_type(
        dynamic_cast<const ast::existential_type &>(type), ctx);
  default:
    return k_unknown_type;
  }
}

/// Resolves `some Bound` (`existential_type`) to a fresh, nominal
/// `existential_kind` type — one per AST node, memoized in
/// `existential_types_` so every caller resolving the same node (a
/// function's own return-type resolution plus every external call site's
/// `signature_return_type` view of it) agrees on the same id. Only legal
/// in a function's return-type position (`ctx.existential_allowed`);
/// anywhere else this reports "not allowed here" and returns
/// `k_unknown_type`, unless `ctx.quiet` (an external, best-effort view
/// that shouldn't double-report a diagnostic the declaration's own check
/// already reports).
///
/// `ctx.is_generator_return` takes a completely separate path: a
/// `generator def`'s `some iterator[T]` must resolve straight to the
/// concrete `generator[T]` for *every* caller (this function's own body
/// check and every external call site's `signature_return_type` view
/// alike, since both call through here on the same AST node) — never the
/// general opaque wrapper, which only `check_function`'s own local
/// `return_type_` could see, not a caller resolving a call's type
/// elsewhere. Generator semantics predate, and stay fully independent of,
/// the general existential mechanism below; this is exactly the old,
/// pre-generalization behavior, preserved verbatim under this flag.
auto checker::resolve_existential_type(const ast::existential_type &ety,
                                       const resolve_ctx &ctx) -> type_id {
  if (!ctx.existential_allowed) {
    if (!ctx.quiet) {
      error_with_help(
          ety.span,
          "`some Trait[Args]` is only supported as a function's return "
          "type",
          "existential type not allowed here",
          "`some Trait` names an opaque type chosen by whatever function "
          "returns it; write a concrete type (or a generic type "
          "parameter) here instead.");
    }
    return k_unknown_type;
  }
  if (ctx.is_generator_return) {
    if (ety.value.terms.size() != 1 || ety.value.terms[0].type == nullptr ||
        ety.value.terms[0].type->kind != ast::node_kind::named_type) {
      return k_unknown_type;
    }
    const auto &named =
        dynamic_cast<const ast::named_type &>(*ety.value.terms[0].type);
    if (named.path.size() != 1 || named.path.front() != "iterator" ||
        named.type_args.size() != 1) {
      return k_unknown_type;
    }
    auto args = resolve_type_args(named.type_args, ctx);
    const auto item = args.empty() ? k_unknown_type : args.front();
    return types_.builtin_generic("generator", {item});
  }
  if (const auto found = existential_types_.find(&ety);
      found != existential_types_.end()) {
    return found->second;
  }
  auto bound = std::vector<bound_trait_ref>{};
  auto display_name = std::string("some ");
  for (const auto &term : ety.value.terms) {
    if (term.type == nullptr || term.type->kind != ast::node_kind::named_type) {
      continue;
    }
    const auto &named = dynamic_cast<const ast::named_type &>(*term.type);
    if (named.path.empty()) {
      continue;
    }
    const auto &trait_name = named.path.back();
    if (!ctx.quiet && !find_trait_anywhere(trait_name).has_value()) {
      error_with_help(
          term.span, std::format("`{}` is not a known trait", trait_name),
          "unknown trait in existential bound",
          "`some Trait` requires a real `trait` declaration in scope.");
    }
    auto args = resolve_type_args(named.type_args, ctx);
    if (!bound.empty()) {
      display_name += " + ";
    }
    display_name += trait_name;
    if (!args.empty()) {
      display_name += '[';
      for (size_t i = 0; i < args.size(); ++i) {
        if (i != 0) {
          display_name += ", ";
        }
        display_name += types_.display(args[i]);
      }
      display_name += ']';
    }
    bound.push_back(bound_trait_ref{.trait_name = trait_name,
                                    .trait_args = std::move(args)});
  }
  const auto id =
      types_.fresh_existential(std::move(display_name), std::move(bound));
  existential_types_.emplace(&ety, id);
  return id;
}

/// A quoted `` `(int32)` `` or `` `(std.foo.bar)` `` classifies as an
/// `expr` fragment at parse time (`parser::classify_and_parse_quote_
/// fragment` has no annotation context to know it's meant as a type — see
/// its doc comment) — the disambiguation is genuinely deferred to this
/// layer. Here, in type position, a bare name or dotted path is
/// unambiguous: it can only sensibly mean a named type, so reinterpret it
/// as one rather than rejecting the splice. Anything structurally richer
/// (a call, a binary expression, ...) isn't reinterpreted — return
/// `nullptr` and let the caller report the mismatch.
auto checker::reinterpret_as_named_type(const ast::node &fragment)
    -> const ast::type_expr * {
  auto path = std::vector<std::string>{};
  if (const auto *ident = dynamic_cast<const ast::ident_expr *>(&fragment)) {
    path.push_back(ident->name);
  } else if (const auto *module_path =
                 dynamic_cast<const ast::module_path_expr *>(&fragment)) {
    path = module_path->segments;
  } else {
    return nullptr;
  }
  auto named = ast::make<ast::named_type>();
  named->span = fragment.span;
  named->path = std::move(path);
  const auto *raw = named.get();
  synthesized_types_.push_back(std::move(named));
  return raw;
}

/// Type-position splice: `~(expr)`. Evaluates the operand at compile time
/// (mirroring `infer_expr`'s `splice_expr` case) and, if it resolves to a
/// `type_expr_fragment` quote value, resolves the wrapped `ast::type_expr`
/// in `splice.operand`'s place. No hygiene yet (M5): the wrapped type is
/// resolved against `ctx`, the splice site's own resolve context, not the
/// quote site's.
auto checker::resolve_splice_type(const ast::splice_type &splice,
                                  const resolve_ctx &ctx) -> type_id {
  if (splice.operand == nullptr) {
    return k_unknown_type;
  }
  const auto operand_type = infer_expr(*splice.operand, k_unknown_type);
  require_quote_value(operand_type, splice.operand->span,
                      "a type-position splice");
  const auto fragment_value = comptime_eval_.evaluate(*splice.operand);
  if (fragment_value.is_error()) {
    return k_error_type;
  }
  if (fragment_value.kind == comptime::value_kind::type_expr_fragment &&
      fragment_value.fragment != nullptr) {
    const auto *fragment_type =
        dynamic_cast<const ast::type_expr *>(fragment_value.fragment);
    if (fragment_type != nullptr) {
      return resolve_type(*fragment_type, ctx);
    }
  }
  if (fragment_value.kind == comptime::value_kind::expr_fragment &&
      fragment_value.fragment != nullptr) {
    if (const auto *reinterpreted =
            reinterpret_as_named_type(*fragment_value.fragment)) {
      return resolve_type(*reinterpreted, ctx);
    }
  }
  error_with_help(
      splice.operand->span,
      std::format("a type-position splice must resolve to a quoted "
                  "type, found {}",
                  quote_fragment_kind_name(fragment_value.kind)),
      "this splice is used in type position",
      "Only a value quoted as `` `(...)` `` with `type_expr` content "
      "can be spliced into a type position.");
  return k_error_type;
}

// ------------------------------------------------------------------------
//  Bounds at the call site (phase 9, rule 3)
// ------------------------------------------------------------------------

/// Whether `str` has a trait from `std.traits` without an `impl` of it:
/// `std.string` gives it `eq` and `cmp` as `extend` methods with their own
/// signatures, and it formats without one. Every other builtin scalar has
/// real impls (`traits.scalar.cn`), found the ordinary way.
auto checker::builtin_has_trait(type_id member, const ast::trait_decl &trait)
    -> bool {
  const auto *traits_module = index_.find_module("std.traits");
  if (traits_module == nullptr) {
    return false;
  }
  const auto it = traits_module->traits.find(trait.name);
  if (it == traits_module->traits.end() || it->second.decl != &trait) {
    return false;
  }
  const auto &entry = types_.entry(member);
  if (entry.kind != type_kind::builtin_kind || entry.name != "str") {
    return false;
  }
  return trait.name == "eq" || trait.name == "ord" || trait.name == "show" ||
         trait.name == "debug";
}

/// Whether `subject` implements `trait[args]` where a call needs it to:
/// through its own bounds if it is the caller's type parameter, through an
/// `impl` (or being a builtin number) if it is concrete.
auto checker::satisfies_trait(type_id subject, const ast::trait_decl &trait,
                              const std::vector<type_id> &args) -> bool {
  // A reference is looked through below, so a `&list[T]` has its list's
  // traits. A `&int32` does not have `int32`'s: it is an address, and a
  // generic body comparing or adding two of them would compute with the
  // addresses — the trap `needs_explicit_deref` refuses in direct code.
  if (needs_explicit_deref(subject)) {
    return false;
  }
  const auto stripped = strip_refs(subject);
  if (types_.is_unknown(stripped) && !is_rigid_param(stripped)) {
    return true;
  }
  if (stripped == k_error_type) {
    return true;
  }
  const auto args_agree = [&](const std::vector<type_id> &have) -> bool {
    if (args.size() != have.size()) {
      return args.empty();
    }
    for (size_t i = 0; i < args.size(); ++i) {
      if (args[i] != have[i] && !types_.is_unknown(args[i]) &&
          !types_.is_unknown(have[i])) {
        return false;
      }
    }
    return true;
  };
  if (is_rigid_param(stripped)) {
    for (const auto &fact : facts_of(stripped)) {
      if (fact.trait == &trait && args_agree(fact.args)) {
        return true;
      }
    }
    return false;
  }
  if (builtin_has_trait(stripped, trait)) {
    return true;
  }
  const auto entry = types_.entry(stripped);
  if (!type_has_trait(entry, trait.name)) {
    return false;
  }
  if (args.empty()) {
    return true;
  }
  const auto impl_args = trait_args_of_impl_for(stripped, trait.name);
  return !impl_args.has_value() || args_agree(*impl_args);
}

/// Whether `subject` satisfies the concept `concept_decl`: a category by
/// membership, any other concept by each of its trait constraints.
auto checker::satisfies_concept(type_id subject,
                                const ast::concept_decl &concept_decl) -> bool {
  if (is_copy_concept(concept_decl)) {
    return type_is_copy(subject);
  }
  const auto stripped = strip_refs(subject);
  if ((types_.is_unknown(stripped) && !is_rigid_param(stripped)) ||
      stripped == k_error_type) {
    return true;
  }
  if (const auto members = concept_category(concept_decl)) {
    if (is_rigid_param(stripped)) {
      const auto domain = domain_of(stripped);
      return domain.has_value() && std::ranges::includes(*members, *domain);
    }
    return std::ranges::binary_search(*members, stripped);
  }
  auto bindings = type_scope{};
  if (!concept_decl.params.empty()) {
    bindings.emplace(concept_decl.params.front().name, subject);
  }
  auto home = module_;
  auto home_file = file_id_;
  for (const auto &[module_name, members] : index_.modules) {
    if (const auto it = members.concepts.find(concept_decl.name);
        it != members.concepts.end() && it->second.decl == &concept_decl) {
      home = &members;
      home_file = it->second.file_id;
    }
  }
  const auto ctx = resolve_ctx{.module = home,
                               .param_bindings = &bindings,
                               .use_type_param_stack = false,
                               .quiet = true};
  return in_scope_of(home, home_file, [&] -> bool {
    for (const auto &constraint : concept_decl.constraints) {
      const auto *bound =
          dynamic_cast<const ast::type_expr *>(constraint.bound_or_expr.get());
      if (constraint.subject == nullptr || bound == nullptr) {
        continue;
      }
      const auto constrained = resolve_type(*constraint.subject, ctx);
      if (!bound_holds(constrained, *bound, ctx).empty()) {
        return false;
      }
    }
    return true;
  });
}

/// The terms of `bound` (written in `ctx` and the current module) that
/// `subject` does not satisfy, spelled for a diagnostic. Empty when every
/// term holds, or names nothing this compiler can check.
auto checker::bound_holds(type_id subject, const ast::type_expr &bound,
                          const resolve_ctx &ctx) -> std::vector<std::string> {
  auto missing = std::vector<std::string>{};
  for (const auto *term : bound_terms(bound)) {
    if (term->kind != ast::node_kind::named_type) {
      continue;
    }
    const auto &named = dynamic_cast<const ast::named_type &>(*term);
    if (named.path.empty()) {
      continue;
    }
    if (names_copy(named)) {
      if (!type_is_copy(subject)) {
        missing.emplace_back("copy");
      }
      continue;
    }
    if (const auto trait = find_trait_decl_by_name(named.path.back());
        trait.has_value() && *trait != nullptr) {
      const auto args = resolve_type_args(named.type_args, ctx);
      if (!satisfies_trait(subject, **trait, args)) {
        missing.push_back(
            display_fact(type_fact{.trait = *trait, .args = args}));
      }
      continue;
    }
    if (const auto concept_ref = find_concept_decl_by_name(named.path.back());
        concept_ref.has_value() && concept_ref->decl != nullptr &&
        !satisfies_concept(subject, *concept_ref->decl)) {
      missing.push_back(concept_ref->decl->name);
    }
  }
  return missing;
}

/// Checks every bound `decl` declares against the types a call solved its
/// parameters to (phase 9, rule 3), and reports the first that does not
/// hold — at the call, which is the line that chose the type. False when
/// one failed: no instance of a function whose bounds do not hold is made.
///
/// `fixed` carries the parameters a method inherits from its generic block
/// (`K` in `extend[K] pair[K]`), solved from the receiver: a method's own
/// `where K: ord` is about them, and is checked here like any other bound.
auto checker::check_call_bounds(const ast::call_expr &call,
                                const ast::func_decl &decl,
                                const module_members *owner,
                                file_id_type decl_file,
                                const generic_solution &solution,
                                const type_scope *fixed) -> bool {
  auto bindings = solution.const_slots;
  bindings.insert(solution.type_slots.begin(), solution.type_slots.end());
  if (fixed != nullptr) {
    bindings.insert(fixed->begin(), fixed->end());
  }
  const auto ctx = resolve_ctx{.module = owner,
                               .param_bindings = &bindings,
                               .use_type_param_stack = false,
                               .quiet = true};
  struct failure {
    std::string param;
    type_id subject = k_unknown_type;
    std::vector<std::string> missing;
    source_span bound_span;
  };
  auto failed = std::optional<failure>{};
  in_scope_of(owner, decl_file, [&] -> void {
    const auto check = [&](const std::string &param_name, type_id subject,
                           const ast::type_expr &bound) -> void {
      if (failed.has_value()) {
        return;
      }
      auto missing = bound_holds(subject, bound, ctx);
      if (!missing.empty()) {
        failed = failure{.param = param_name,
                         .subject = subject,
                         .missing = std::move(missing),
                         .bound_span = bound.span};
      }
    };
    for (const auto &param : decl.type_params) {
      if (param.bound_or_type == nullptr ||
          (param.is_value_param &&
           !names_trait_or_concept(*param.bound_or_type))) {
        continue;
      }
      const auto solved_to = solution.type_slots.find(param.name);
      if (solved_to != solution.type_slots.end()) {
        check(param.name, solved_to->second, *param.bound_or_type);
      }
    }
    for (const auto &constraint : decl.where_constraints) {
      if (constraint.subject == nullptr ||
          constraint.bound_or_type == nullptr) {
        continue;
      }
      const auto subject = resolve_type(*constraint.subject, ctx);
      auto spelled = std::string{};
      if (constraint.subject->kind == ast::node_kind::named_type) {
        const auto &named =
            dynamic_cast<const ast::named_type &>(*constraint.subject);
        spelled = named.path.empty() ? "" : named.path.back();
      }
      check(spelled, subject, *constraint.bound_or_type);
    }
  });
  if (!failed.has_value()) {
    return true;
  }
  const auto subject_name = types_.display(failed->subject);
  auto listed = std::string{};
  auto joined = std::string{};
  for (const auto &name : failed->missing) {
    listed += std::format("{}`{}`", listed.empty() ? "" : " and ", name);
    joined += (joined.empty() ? "" : " + ") + name;
  }
  // The caller's own parameter often shares the callee's spelling (`T` for
  // `T`), so it is named as *this function's* `T` to keep the two apart.
  const auto rigid_subject = is_rigid_param(strip_refs(failed->subject));
  auto diag = diagnostic(
      diagnostic_level::error,
      rigid_subject
          ? std::format("`{}` needs `{}: {}`, and nothing says this "
                        "function's `{}` is one",
                        decl.name, failed->param, joined, subject_name)
          : std::format("`{}` needs `{}: {}`, and `{}` does not satisfy it",
                        decl.name, failed->param, joined, subject_name),
      file_id_);
  diag.with_label(call.span,
                  rigid_subject
                      ? std::format("`{}`'s `{}` is this function's `{}` here",
                                    decl.name, failed->param, subject_name)
                      : std::format("`{}` is `{}` in this call", failed->param,
                                    subject_name));
  // When the reader never wrote `int32` as a type argument, say where it
  // came from: the annotation the call's result was checked against.
  if (const auto solved = expected_solved_params_.find(&call);
      solved != expected_solved_params_.end()) {
    const auto prefix = std::format("`{}` was solved to", failed->param);
    for (const auto &note : solved->second) {
      if (note.starts_with(prefix)) {
        diag.with_note(note);
      }
    }
  }
  diag.children.push_back(
      diagnostic(diagnostic_level::note,
                 std::format("`{}`'s bound on `{}` is declared here", decl.name,
                             failed->param),
                 decl_file)
          .with_label(failed->bound_span, "the bound"));
  if (is_rigid_param(strip_refs(failed->subject))) {
    diag.with_help(std::format(
        "`{}` is a type parameter here, so it only has the bounds written "
        "on it. Add the same bound — `where {}: {}` — and pass the "
        "requirement on to this function's own callers.",
        subject_name, subject_name, failed->missing.front()));
  } else if (std::ranges::contains(failed->missing, std::string("copy"))) {
    diag.with_help(std::format(
        "Only the builtin scalars, `str`, `&T`, raw pointers, and the "
        "read-only views `slice[T]` and `cell[T]` are `copy`, and no `impl` "
        "can make `{}` one. Lend it instead (`&x` makes `{}` a `&{}`), or "
        "take the `copy` bound off `{}` so its values move.",
        subject_name, failed->param, subject_name, failed->param));
  } else if (needs_explicit_deref(failed->subject)) {
    // A lambda argument is the usual source: `x => x` handed a `&int32`
    // returns it unchanged.
    const auto has_lambda =
        std::ranges::any_of(call.args, [](const ast::call_arg &arg) -> bool {
          return arg.value != nullptr &&
                 arg.value->kind == ast::node_kind::lambda_expr;
        });
    diag.with_help(std::format(
        "`{}` is the address of a number, not the number, so it has none "
        "of the number's traits. Pass the number itself by dereferencing "
        "the reference with `*`{}.",
        subject_name,
        has_lambda ? " — in a lambda that returns its parameter, write "
                     "`x => *x` rather than `x => x`"
                   : ""));
  } else {
    diag.with_help(std::format(
        "Pass a type that implements {}, or implement it for `{}` — `impl "
        "{} for {}:`.",
        listed, subject_name, failed->missing.front(), subject_name));
  }
  emit_diag(diag);
  mark_error();
  return false;
}

auto checker::instantiate_generic_function(
    const ast::call_expr &call, const ast::func_decl &decl,
    const module_members *owner, file_id_type decl_file,
    const value_bindings &solved, const std::vector<fn_param_info> &params,
    const explicit_generic_args &explicit_args, const ast::expr *ufcs_receiver)
    -> std::optional<type_id> {
  const auto solution =
      solve_generic_call(call, decl, owner, decl_file, solved, params,
                         explicit_args, ufcs_receiver);
  if (!solution.has_value()) {
    return std::nullopt;
  }
  // A solution written in this body's own type parameters is not a type an
  // instance can be compiled at — the body is itself generic (a method of
  // `extend[T] list[T]` calling `alloc[T]`), and its instances make this
  // call again with `T` concrete. It is checked like a template's call. A
  // solution whose bounds failed is reported already and compiles nothing.
  if (solution_mentions_template_param(*solution) || !solution->bounds_hold) {
    auto [param_types, result] =
        solved_signature(decl, owner, decl_file, *solution);
    if (call.callee != nullptr) {
      record_expr_type(*call.callee, types_.fn_of(param_types, result));
    }
    if (solution->bounds_hold) {
      defer_generic_call(call, decl, owner, decl_file, *solution,
                         ufcs_receiver);
      // Compile-time code runs this template body, not its instances, and
      // reads these parameters through what its own call bound them to.
      if (comptime_only_functions_.contains(&decl)) {
        comptime_call_type_slots_[&call] = solution->type_slots;
      }
    }
    if (result == k_unknown_type) {
      return std::nullopt;
    }
    return record_expr_type(call, result);
  }
  if (solution_is_open(*solution)) {
    auto [param_types, result] =
        solved_signature(decl, owner, decl_file, *solution);
    if (result == k_unknown_type) {
      result = leaf_ctxt_.fresh_type(
          std::format("the result of this call to `{}`", decl.name),
          source_location{.file_id = file_id_, .span = call.span});
    }
    if (call.callee != nullptr) {
      record_expr_type(*call.callee, types_.fn_of(param_types, result));
    }
    // The bounds held vacuously against a leaf; they are checked again
    // against what it settles to.
    when_solution_settles(
        decl, *solution,
        [this, &call, &decl, owner, decl_file, ufcs_receiver,
         result](const generic_solution &settled) -> void {
          auto concrete = settled;
          concrete.bounds_hold =
              check_call_bounds(call, decl, owner, decl_file, concrete);
          if (!concrete.bounds_hold) {
            return;
          }
          if (const auto answer = instantiate_solved_call(
                  call, decl, owner, decl_file, concrete, ufcs_receiver)) {
            (void)leaf_engine_.unify(result, *answer, infer::k_no_cause);
          }
        });
    return record_expr_type(call, result);
  }
  return instantiate_solved_call(call, decl, owner, decl_file, *solution,
                                 ufcs_receiver);
}

/// Whether any type `solution` solved to is still an open leaf.
auto checker::solution_is_open(const generic_solution &solution) -> bool {
  return std::ranges::any_of(
      solution.type_slots,
      [&](const auto &slot) -> bool { return mentions_type_var(slot.second); });
}

/// Runs `name_instance` with `solution` once every type it solved to is
/// concrete: now, if it already is.
///
/// Solving a call never defaults its arguments' leaves. The call's type is
/// known in terms of them; only the instance it names needs them settled,
/// so naming is the decision that waits, retried by the leaf queue as the
/// leaves are solved. A leaf nothing pins gets the queue's last resort, as
/// `demand` would have given it, but only after every later statement has
/// had its say: `let r = pick(5)` then `let c: int64 = r` names
/// `pick$int64`.
auto checker::when_solution_settles(
    const ast::func_decl &decl, const generic_solution &solution,
    std::function<void(const generic_solution &)> name_instance) -> void {
  if (!solution_is_open(solution)) {
    name_instance(solution);
    return;
  }
  auto open = std::vector<type_id>{};
  for (const auto &[name, type] : solution.type_slots) {
    if (mentions_type_var(type)) {
      open.push_back(type);
    }
  }
  defer_method_call(
      decl.name, types_.tuple_of(std::move(open)), std::vector<type_id>{},
      [this, &decl, solution,
       name_instance = std::move(name_instance)](type_id) -> void {
        if (const auto settled = settled_solution(decl, solution)) {
          name_instance(*settled);
        }
      });
}

/// A template's call to a generic function, made for each instance with
/// the solution's parameters replaced by the instance's answers.
auto checker::defer_generic_call(const ast::call_expr &call,
                                 const ast::func_decl &decl,
                                 const module_members *owner,
                                 file_id_type decl_file,
                                 const generic_solution &solution,
                                 const ast::expr *ufcs_receiver) -> void {
  defer_to_instances(call,
                     [this, &call, &decl, owner, decl_file, solution,
                      ufcs_receiver](instance_subst &subst) -> void {
                       const auto concrete =
                           substitute_solution(decl, solution, subst);
                       if (!concrete.has_value()) {
                         return;
                       }
                       (void)instantiate_solved_call(
                           *clone_of(subst, &call), decl, owner, decl_file,
                           *concrete, clone_of(subst, ufcs_receiver));
                     });
}

/// `solution` with every answer substituted, re-bound in declaration order
/// so its suffix names the instance; `nullopt` if an answer is still not
/// concrete.
auto checker::substitute_solution(const ast::func_decl &decl,
                                  const generic_solution &solution,
                                  instance_subst &subst)
    -> std::optional<generic_solution> {
  return map_solution(decl, solution, [&](type_id type) -> type_id {
    return substitute_type(type, subst);
  });
}

/// `solution` with its open leaves replaced by what they settled to;
/// `nullopt` if one is still not concrete.
auto checker::settled_solution(const ast::func_decl &decl,
                               const generic_solution &solution)
    -> std::optional<generic_solution> {
  return map_solution(decl, solution, [&](type_id type) -> type_id {
    return leaf_ctxt_.zonk(type);
  });
}

/// `solution` with `map` applied to every answer, re-bound in declaration
/// order so its suffix names the instance; `nullopt` if an answer is still
/// not concrete.
auto checker::map_solution(const ast::func_decl &decl,
                           const generic_solution &solution,
                           const std::function<type_id(type_id)> &map)
    -> std::optional<generic_solution> {
  auto out = generic_solution{};
  for (const auto &param : decl.type_params) {
    if (param.name.empty()) {
      continue;
    }
    if (const auto found = solution.const_slots.find(param.name);
        found != solution.const_slots.end()) {
      const auto value = map(found->second);
      const auto entry = types_.entry(value);
      if (entry.kind != type_kind::const_value_kind) {
        return std::nullopt;
      }
      bind_generic_constant(out, param, entry.result, entry.value.constant);
      continue;
    }
    if (const auto found = solution.type_slots.find(param.name);
        found != solution.type_slots.end()) {
      const auto type = map(found->second);
      if (mentions_type_param(type) || mentions_abstract_type(type)) {
        return std::nullopt;
      }
      bind_generic_type(out, param, type);
      continue;
    }
    return std::nullopt;
  }
  return out;
}

/// The instance a solved, concrete call names, and the call's type read
/// through it.
auto checker::instantiate_solved_call(const ast::call_expr &call,
                                      const ast::func_decl &decl,
                                      const module_members *owner,
                                      file_id_type decl_file,
                                      const generic_solution &solved_call,
                                      const ast::expr *ufcs_receiver)
    -> std::optional<type_id> {
  const auto *solution = &solved_call;
  const auto *instance = find_or_check_generic_instance(
      call, decl, owner, decl_file, *solution, decl.name + solution->suffix,
      /*fixed_type_params=*/nullptr);
  if (instance == nullptr) {
    return std::nullopt;
  }
  if (comptime_only_functions_.contains(&decl)) {
    comptime_call_type_slots_[&call] = solution->type_slots;
  }

  // The instance's signature, resolved under the same substitution its body
  // was checked with — this is what makes the *caller* see concrete types
  // (`array[int32, 3]`, not `array[int32, n]`) for the callee it names and
  // the value the call produces.
  auto bindings = solution->const_slots;
  bindings.insert(solution->type_slots.begin(), solution->type_slots.end());
  const auto ctx = resolve_ctx{.module = owner,
                               .param_bindings = &bindings,
                               .use_type_param_stack = false,
                               .quiet = true,
                               .existential_allowed = true};
  // Under the *declaring* file, not the caller's: these type expressions
  // were written in `decl`'s file and name what that file has in scope. A
  // return type naming a struct the callee's module imported
  // (`def wrap[T](v: T) -> holder[T]` over a `use other.holder`) resolves
  // to nothing when read through the caller's imports, and the call then
  // reaches lowering with no concrete type at all — the callee's own
  // signature is not the caller's to interpret.
  const auto saved_signature_file = file_id_;
  file_id_ = decl_file;
  auto param_types = std::vector<type_id>{};
  for (const auto &param : decl.params) {
    param_types.push_back(param.type_annotation != nullptr
                              ? resolve_type(*param.type_annotation, ctx)
                              : k_unknown_type);
  }
  // An omitted return type is inferred from `instance`'s own body
  // (`check_function_impl`'s `inferring_return_`), but that body is
  // usually still sitting in `pending_instances_` at this point — this
  // call is what requested the instance, not what checks it. Read
  // `inferred_returns_` for the rare case it was already checked (a
  // repeat call to the same instantiation, after the queue drained
  // between the two calls); otherwise mint a leaf and let
  // `finish_open_results` pin it once `flush_pending_instances` has run,
  // the same way `instantiate_open_param_call` resolves an implicit
  // generic's open-parameter call result.
  auto result_pending = false;
  type_id result = k_unknown_type;
  if (decl.return_type != nullptr) {
    result = resolve_type(*decl.return_type, ctx);
  } else if (const auto found = inferred_returns_.find(instance);
             found != inferred_returns_.end()) {
    result = found->second;
  } else {
    result = leaf_ctxt_.fresh_type(
        std::format("the result of this call to `{}`", decl.name),
        source_location{.file_id = file_id_, .span = call.span});
    result_pending = true;
  }
  file_id_ = saved_signature_file;
  if (call.callee != nullptr) {
    record_expr_type(*call.callee, types_.fn_of(param_types, result));
  }
  if (result_pending) {
    pending_open_results_.push_back(
        pending_open_result{.call = &call,
                            .instance = instance,
                            .leaf = result,
                            .fn_params = std::move(param_types),
                            .file = file_id_});
  }
  // A comptime-only instance (todo item 8) has no compiled function for an
  // ordinary call site to reach — `hir::lower_module` deliberately never
  // lowers one (`comptime_only_functions_`'s doc comment) — so, unless
  // this call is itself reached from inside another comptime-only
  // function's own body (already handled directly by the evaluator when
  // *that* function runs), fold the whole call to a literal now instead of
  // recording it as an ordinary callee.
  if (!in_comptime_only_function_ &&
      comptime_only_functions_.contains(instance)) {
    if (const auto *lit = try_fold_comptime_only_call(
            call, decl, *instance, result, solution->type_slots)) {
      folded_comptime_calls_[&call] = lit;
      return record_expr_type(call, result);
    }
  }
  // Deliberately last: a module-qualified call (`app.lib.at(v, i)`) already
  // recorded the *template* as its callee on the way in
  // (`infer_qualified_call`), and lowering must emit a call to the instance,
  // which is the only one of the two that exists as a function.
  resolved_callees_[&call] = resolved_callee{.decl = instance,
                                             .owner_module = owner->module_name,
                                             .impl_target_type = "",
                                             .receiver = ufcs_receiver};
  return record_expr_type(call, result);
}

/// Explains a call whose value parameter no argument determines — the one
/// way monomorphization fails that is the *caller's* to fix.
auto checker::report_unsolved_value_param(const ast::call_expr &call,
                                          const ast::func_decl &decl,
                                          const ast::type_param &param)
    -> void {
  error_with_help(
      call.span,
      std::format("cannot tell what `{}` is in this call to `{}`", param.name,
                  decl.name),
      std::format("`{}` is a compile-time value, and this call doesn't fix "
                  "it",
                  param.name),
      std::format(
          "`{}` is compiled once per constant `{}`, so `{}` has to be a "
          "definite number here. Either give it outright in brackets "
          "(`{}[8](...)`), or pass an argument whose type mentions it with "
          "a known length (an `array[T, {}]`, for instance).",
          decl.name, param.name, param.name, decl.name, param.name));
}

/// The same explanation for a type parameter the arguments leave open.
auto checker::report_unsolved_type_param(const ast::call_expr &call,
                                         const ast::func_decl &decl,
                                         const ast::type_param &param) -> void {
  error_with_help(
      call.span,
      std::format("cannot tell what `{}` is in this call to `{}`", param.name,
                  decl.name),
      std::format("`{}` is a type parameter this call leaves unsolved",
                  param.name),
      std::format("`{}` is compiled once per concrete type, so every type "
                  "parameter has to be pinned down. Either give it outright "
                  "in brackets (`{}[int32](...)`), or annotate the value "
                  "that should determine `{}` — an annotated `let`, a "
                  "declared return type, or any other context with a known "
                  "type is used to solve a parameter the arguments leave "
                  "open.",
                  decl.name, decl.name, param.name));
}

/// A value parameter that isn't a number has no constant to name an
/// instance after and none to compile the body against.
auto checker::report_non_numeric_value_param(const ast::call_expr &call,
                                             const ast::func_decl &decl,
                                             const ast::type_param &param)
    -> void {
  const auto underlying =
      param.bound_or_type != nullptr
          ? resolve_type(*param.bound_or_type,
                         resolve_ctx{.module = module_,
                                     .param_bindings = nullptr,
                                     .use_type_param_stack = false,
                                     .quiet = true})
          : types_.usize_type();
  error_with_help(
      call.span,
      std::format("`{}` cannot be compiled: it is generic over the "
                  "compile-time value `{}`, which is a `{}` rather than "
                  "a number",
                  decl.name, param.name, types_.display(underlying)),
      "no compiled copy can be made for this call",
      std::format("A function generic over a value is compiled once per "
                  "*constant* it is called with, and only integer values "
                  "have constants to compile for so far. Write the "
                  "state out (`{}[<state>]`) in the signature instead of "
                  "abstracting over `{}`.",
                  types_.display(underlying), param.name));
}

/// A method call on a value whose type is a type parameter
/// (ch. 19, Bounded Generics, rule 2).
///
/// The method must belong to a trait among the parameter's facts, and the
/// call is checked against that trait's signature with `self` standing for
/// the receiver and the trait's parameters for the fact's arguments. What
/// the parameter will be in some instance is not consulted: the body has
/// to be right for every type the signature admits, and saying so here is
/// the only place the message can point at the line that is wrong.
///
/// `static_call` is `T.zero()`: a static member reached through the
/// parameter itself, which the bound supplies the same way — only a method
/// without `self` qualifies, and every argument is written.
auto checker::check_bound_method_call(
    const ast::call_expr &call, const ast::field_expr &field, type_id receiver,
    const explicit_generic_args &explicit_args, bool static_call) -> type_id {
  struct provider {
    type_fact fact;
    const ast::trait_decl *trait = nullptr;
    const ast::func_decl *method = nullptr;
  };
  const auto facts = facts_of(receiver);
  auto providers = std::vector<provider>{};
  for (const auto &fact : facts) {
    const auto *trait = fact.trait;
    for (const auto &item : trait->items) {
      if (item == nullptr || item->kind != ast::node_kind::func_decl) {
        continue;
      }
      const auto &member = dynamic_cast<const ast::func_decl &>(*item);
      const auto takes_self = !member.params.empty() &&
                              param_name_of(member.params.front()) == "self";
      if (member.name == field.field_name && takes_self != static_call) {
        providers.push_back(
            provider{.fact = fact,
                     .trait = trait,
                     .method = &dynamic_cast<const ast::func_decl &>(*item)});
      }
    }
  }

  const auto receiver_name = types_.display(receiver);
  if (providers.empty()) {
    report_unbound_method(field, receiver_name, facts, static_call);
    infer_call_args_loosely(call);
    return k_error_type;
  }
  if (providers.size() > 1) {
    auto listed = std::string{};
    for (const auto &candidate : providers) {
      if (!listed.empty()) {
        listed += " and ";
      }
      listed += std::format("`{}`", display_fact(candidate.fact));
    }
    error_with_help(
        field.span,
        std::format("`{}` is ambiguous on `{}`", field.field_name,
                    receiver_name),
        "more than one bound provides this method",
        std::format("Both {} declare a method `{}`, and `{}` is bound by "
                    "each, so this call could mean either. Rename one of "
                    "the methods, or drop the bound this call does not "
                    "need.",
                    listed, field.field_name, receiver_name));
    infer_call_args_loosely(call);
    return k_error_type;
  }

  const auto &chosen = providers.front();
  const auto [owner, owner_file] = trait_home(*chosen.trait);
  const auto saved_self = self_type_;
  const auto saved_assoc = self_assoc_types_;
  self_type_ = receiver;
  self_assoc_types_.clear();
  auto params =
      signature_params(*chosen.method, owner,
                       /*skip_self=*/!static_call, &chosen.trait->type_params);
  auto result =
      signature_return_type(*chosen.method, owner, &chosen.trait->type_params);
  self_type_ = saved_self;
  self_assoc_types_ = saved_assoc;

  auto subst = std::unordered_map<type_id, type_id>{};
  for (size_t i = 0;
       i < chosen.trait->type_params.size() && i < chosen.fact.args.size();
       ++i) {
    const auto &param = chosen.trait->type_params[i];
    subst.emplace(
        types_.type_param(param.name, param.higher_kinded_arity, &param),
        chosen.fact.args[i]);
  }
  auto param_types = std::vector<type_id>{};
  for (auto &param : params) {
    param.type = substitute_solved(param.type, subst);
    param_types.push_back(param.type);
  }
  result = substitute_solved(result, subst);
  record_expr_type(field, types_.fn_of(std::move(param_types), result));
  if (!check_method_accepts_generic_args(field, *chosen.method,
                                         explicit_args)) {
    infer_call_args_loosely(call);
    return k_error_type;
  }
  auto solved = value_bindings{};
  const auto generic = generic_call_context{.decl = chosen.method,
                                            .owner = owner,
                                            .explicit_args = &explicit_args,
                                            .ufcs_receiver = nullptr};
  check_call_args_against(
      call, params, chosen.method->name,
      source_location{.file_id = owner_file, .span = chosen.method->span},
      &solved, is_generic_template(*chosen.method) ? &generic : nullptr);
  // Which function this is depends on what the parameter turns out to be,
  // so each instance decides for itself.
  defer_to_instances(call,
                     [this, &call, &field, receiver,
                      static_call](instance_subst &subst) -> void {
                       const auto &clone = *clone_of(subst, &call);
                       auto clone_args = explicit_generic_args{};
                       (void)method_field_of(clone, clone_args);
                       replay_method_call(clone, *clone_of(subst, &field),
                                          substitute_type(receiver, subst),
                                          static_call, clone_args);
                     });
  return result;
}

/// The method `name` a call on a concrete `target` reaches, through the
/// same tables an ordinary call consults: the type's own impls and
/// extensions, then a builtin's constructor-keyed ones.
auto checker::find_method_on(type_id target, std::string_view name)
    -> const method_entry * {
  const auto entry = types_.entry(target); // copy: lookup can intern
  const auto *method = find_method(entry, name, target);
  if (method == nullptr && entry.decl == nullptr && !entry.name.empty()) {
    method = find_builtin_impl_method(entry.name, name);
  }
  if (method == nullptr && entry.decl == nullptr && !entry.name.empty()) {
    method = find_extend_method_for_builtin(entry, name);
  }
  return method;
}

/// Pins each value parameter the arguments' recorded types determine.
auto checker::solve_values_from_argument_types(
    const ast::call_expr &call, const std::vector<fn_param_info> &params,
    value_bindings &solved) -> void {
  const auto mapping = call_argument_mappings_.find(&call);
  if (mapping == call_argument_mappings_.end()) {
    return;
  }
  const auto &args_by_param = mapping->second.args_by_param;
  for (size_t i = 0; i < params.size() && i < args_by_param.size(); ++i) {
    if (args_by_param[i] == nullptr) {
      continue;
    }
    const auto found = node_types_.find(args_by_param[i]);
    if (found != node_types_.end()) {
      match_value_params(params[i].type, settle(found->second), solved);
    }
  }
}

/// An instance's call to a generic block's method with an unannotated
/// parameter: queued like the template's own call would have been, with
/// the types the instance recorded for its arguments, so it names the
/// instance for this receiver and these arguments.
auto checker::replay_open_method_call(const ast::call_expr &call,
                                      const method_entry &method,
                                      std::string_view receiver_name,
                                      const ast::expr &receiver, type_id target)
    -> void {
  const auto params = signature_params(*method.decl, method.owner,
                                       /*skip_self=*/true);
  auto call_params = std::vector<type_id>{k_unknown_type};
  const auto mapping = call_argument_mappings_.find(&call);
  for (size_t i = 0; i < params.size(); ++i) {
    auto type = k_unknown_type;
    if (mapping != call_argument_mappings_.end() &&
        i < mapping->second.args_by_param.size() &&
        mapping->second.args_by_param[i] != nullptr) {
      if (const auto found = node_types_.find(mapping->second.args_by_param[i]);
          found != node_types_.end()) {
        type = settle(found->second);
      }
    }
    call_params.push_back(type);
  }
  pending_open_param_calls_.push_back(pending_open_param_call{
      .call = &call,
      .decl = method.decl,
      .owner = method.owner,
      .decl_file = method.file_id,
      .call_params = std::move(call_params),
      .file = file_id_,
      .module = module_,
      .receiver = &receiver,
      .method = method,
      .target_type_name = std::string(receiver_name),
      .self_type = target,
      .block_generic = true,
      // The template already stated the call's types; this names only.
      .typed = true});
}

/// Resolves a method call whose receiver was a type parameter in the
/// template and is `target` in this instance, from the types the template
/// recorded for its arguments — the elaboration half of a call, without
/// checking anything again.
auto checker::replay_method_call(const ast::call_expr &call,
                                 const ast::field_expr &field, type_id target,
                                 bool static_call,
                                 const explicit_generic_args &explicit_args)
    -> void {
  target = strip_refs(target);
  const auto *method = find_method_on(target, field.field_name);
  if (method == nullptr || mentions_type_param(target)) {
    return;
  }
  const auto entry = types_.entry(target);
  const auto generic_method = !method->decl->type_params.empty();
  if (!static_call) {
    if (field.object == nullptr) {
      return;
    }
    const auto &receiver = *field.object;
    if (generic_method) {
      const auto params = signature_params(*method->decl, method->owner,
                                           /*skip_self=*/true);
      auto solved = value_bindings{};
      solve_values_from_argument_types(call, params, solved);
      auto bindings = param_subst{};
      solve_from_argument_types(call, params, bindings);
      auto impl_bindings = param_subst{};
      if (method->block_type_params != nullptr) {
        match_params(method->impl_target_pattern, target, impl_bindings);
        solve_impl_value_params(*method, target, impl_bindings);
      }
      const auto impl_scope = bindings_by_name(impl_bindings);
      (void)instantiate_hk_method(
          call, *method, entry.name, bindings,
          resolved_callee{.owner_module = method->owner->module_name,
                          .impl_target_type = "",
                          .receiver = &receiver,
                          .trait_name = method->trait_name},
          explicit_args, solved, target, &impl_scope);
      return;
    }
    if (impl_needs_instance(*method, entry) &&
        has_unannotated_params(*method->decl)) {
      replay_open_method_call(call, *method, entry.name, receiver, target);
      return;
    }
    if (impl_needs_instance(*method, entry)) {
      auto bindings = param_subst{};
      match_params(method->impl_target_pattern, target, bindings);
      solve_impl_value_params(*method, target, bindings);
      auto scoped = method->fixed_type_params;
      scoped.merge(bindings_by_name(bindings));
      auto solution = generic_solution{};
      carry_impl_value_slots(*method, bindings, solution);
      solution.suffix = std::format("${}", mangle_type_for_instance(target));
      (void)finish_impl_generic_method_call(call, *method, entry.name, receiver,
                                            target, bindings, scoped, solution);
      return;
    }
    record_instance_method_callee(call, *method, entry.name, receiver);
    return;
  }

  const auto target_name = types_.display(target);
  if (!generic_method && impl_needs_instance(*method, entry)) {
    auto bindings = param_subst{};
    if (const auto *instance =
            check_impl_generic_static_call(call, *method, target, bindings)) {
      resolved_callees_[&call] =
          resolved_callee{.decl = instance,
                          .owner_module = method->owner->module_name,
                          .impl_target_type = ""};
    }
    return;
  }
  if (!generic_method) {
    resolved_callees_[&call] =
        resolved_callee{.decl = method->decl,
                        .owner_module = method->owner->module_name,
                        .impl_target_type = target_name};
    return;
  }
  const auto params = signature_params(*method->decl, method->owner,
                                       /*skip_self=*/false);
  auto solved = value_bindings{};
  solve_values_from_argument_types(call, params, solved);
  auto bindings = param_subst{};
  solve_from_argument_types(call, params, bindings);
  auto impl_bindings = param_subst{};
  if (method->block_type_params != nullptr) {
    match_params(method->impl_target_pattern, target, impl_bindings);
    bindings.insert(impl_bindings.begin(), impl_bindings.end());
  }
  const auto impl_scope = bindings_by_name(impl_bindings);
  (void)instantiate_hk_method(
      call, *method, target_name, bindings,
      resolved_callee{.owner_module = method->owner->module_name,
                      .impl_target_type = ""},
      explicit_args, solved, k_unknown_type, &impl_scope);
}

/// "no method `m` on `T`", with what *is* known about `T` and — when some
/// trait in reach declares `m` — the bound that would make the call legal.
auto checker::report_unbound_method(const ast::field_expr &field,
                                    const std::string &receiver_name,
                                    const std::vector<type_fact> &facts,
                                    bool static_call) -> void {
  auto diag = diagnostic(diagnostic_level::error,
                         std::format("no {} `{}` on `{}`",
                                     static_call ? "static member" : "method",
                                     field.field_name, receiver_name),
                         file_id_);
  diag.with_label(field.span, "not provided by any bound");
  if (facts.empty()) {
    diag.with_note(std::format(
        "`{}` is a type parameter with no bounds, so nothing is known about "
        "it: a body can only call methods its bounds promise",
        receiver_name));
  } else {
    auto listed = std::string{};
    for (const auto &fact : facts) {
      if (!listed.empty()) {
        listed += ", ";
      }
      listed += std::format("`{}`", display_fact(fact));
    }
    diag.with_note(std::format("`{}` is only known to implement {}",
                               receiver_name, listed));
  }
  // Spelled with the trait's own parameter names, so a generic trait is
  // suggested in a form that can be written — `iterator[T]`, not the
  // `iterator` that would then fail for missing its argument — and with a
  // word on what those names stand for, since they are the trait's, not
  // this body's.
  struct offer {
    std::string spelled;
    std::string params;
  };
  auto offering = std::vector<offer>{};
  for (const auto &[module_name, members] : index_.modules) {
    for (const auto &[trait_name, trait] : members.traits) {
      if (trait.decl == nullptr ||
          !trait_declares_method(*trait.decl, field.field_name)) {
        continue;
      }
      auto candidate = offer{.spelled = trait_name, .params = {}};
      if (!trait.decl->type_params.empty()) {
        candidate.spelled += '[';
        for (size_t i = 0; i < trait.decl->type_params.size(); ++i) {
          const auto sep = i == 0 ? "" : ", ";
          candidate.spelled += sep + trait.decl->type_params[i].name;
          candidate.params +=
              std::format("{}`{}`", sep, trait.decl->type_params[i].name);
        }
        candidate.spelled += ']';
      }
      if (std::ranges::none_of(offering, [&](const offer &known) -> bool {
            return known.spelled == candidate.spelled;
          })) {
        offering.push_back(std::move(candidate));
      }
    }
  }
  std::ranges::sort(offering, {}, &offer::spelled);
  if (offering.size() == 1) {
    const auto &only = offering.front();
    const auto replace =
        only.params.empty()
            ? std::string{}
            : std::format(", with {} replaced by the type this code means",
                          only.params);
    diag.with_help(std::format(
        "`{}` is a method of the trait `{}`. Add the bound — `where {}: "
        "{}`{} — and every caller must then pass a type that implements it.",
        field.field_name, only.spelled, receiver_name, only.spelled, replace));
  } else if (!offering.empty()) {
    auto listed = std::string{};
    for (const auto &candidate : offering) {
      if (!listed.empty()) {
        listed += ", ";
      }
      listed += std::format("`{}`", candidate.spelled);
    }
    diag.with_help(std::format(
        "`{}` is declared by the traits {}. Bound `{}` by the one this code "
        "means, as in `where {}: {}`.",
        field.field_name, listed, receiver_name, receiver_name,
        offering.front().spelled));
  } else {
    diag.with_help(std::format(
        "No trait declares a method `{}`. Declare one that does, bound "
        "`{}` by it, and implement it for the types you pass.",
        field.field_name, receiver_name));
  }
  emit_diag(diag);
  mark_error();
}

/// What is known about `subject`, as a note: its bounds, or that it has
/// none.
auto checker::facts_note(const std::string &subject_name,
                         const std::vector<type_fact> &facts) -> std::string {
  if (facts.empty()) {
    return std::format("`{}` is a type parameter with no bounds, so nothing "
                       "is known about it: a body can only use what its "
                       "bounds promise",
                       subject_name);
  }
  auto listed = std::string{};
  for (const auto &fact : facts) {
    if (!listed.empty()) {
      listed += ", ";
    }
    listed += std::format("`{}`", display_fact(fact));
  }
  return std::format("`{}` is only known to implement {}", subject_name,
                     listed);
}

/// An operator on a value of type `T` needs the operator's trait among
/// `T`'s facts (ch. 19, Bounded Generics, rule 2). Returns the
/// fact that allows it, or reports at the operator and returns `nullopt`.
auto checker::require_bound_operator(source_span span, type_id subject,
                                     std::string_view trait_name,
                                     std::string_view op_name)
    -> std::optional<type_fact> {
  const auto facts = facts_of(subject);
  for (const auto &fact : facts) {
    if (fact.trait->name == trait_name) {
      return fact;
    }
  }
  const auto subject_name = types_.display(subject);
  auto diag =
      diagnostic(diagnostic_level::error,
                 std::format("`{}` on `{}` needs `{}` to implement `{}`",
                             op_name, subject_name, subject_name, trait_name),
                 file_id_);
  diag.with_label(span, "not provided by any bound");
  diag.with_note(facts_note(subject_name, facts));
  const auto builtin_has_it = trait_name != "eq";
  diag.with_help(std::format(
      "An operator on a type parameter is a call to its trait's method, so "
      "the trait has to be among the parameter's bounds. Add `where {}: "
      "{}`, and every caller must then pass a type that implements it{}.",
      subject_name, trait_name,
      builtin_has_it
          ? std::format(" — or, if `{}` is only ever a builtin number, "
                        "`where {}: numeric`",
                        subject_name, subject_name)
          : std::string{}));
  emit_diag(diag);
  mark_error();
  return std::nullopt;
}

/// What a bound's associated type `name` stands for on `subject`.
///
/// A bound fixes an associated type that has a default to that default:
/// `T: add` means `T + T` is a `T`, because `add` declares `type output =
/// self`. That is the reading almost every generic body wants, and the
/// alternative — an opaque `T.output` that nothing can be done with — makes
/// the common case unwritable. The call site holds up its end by checking
/// the same default against the type it passes. `k_unknown_type` when the
/// trait gives no default, which the caller reports.
auto checker::bound_assoc_type(const type_fact &fact, type_id subject,
                               std::string_view name) -> type_id {
  for (const auto &item : fact.trait->items) {
    if (item == nullptr ||
        item->kind != ast::node_kind::associated_type_decl_node) {
      continue;
    }
    const auto &assoc =
        dynamic_cast<const ast::associated_type_decl_node &>(*item).value;
    if (assoc.name != name || assoc.default_type == nullptr) {
      continue;
    }
    auto bindings = type_scope{};
    for (size_t i = 0;
         i < fact.trait->type_params.size() && i < fact.args.size(); ++i) {
      bindings.emplace(fact.trait->type_params[i].name, fact.args[i]);
    }
    const auto [home, file] = trait_home(*fact.trait);
    const auto ctx = resolve_ctx{.module = home,
                                 .param_bindings = &bindings,
                                 .use_type_param_stack = false,
                                 .quiet = true};
    const auto saved_self = self_type_;
    self_type_ = subject;
    const auto resolved = resolve_type(*assoc.default_type, ctx);
    self_type_ = saved_self;
    return resolved;
  }
  return k_unknown_type;
}

/// `a + b` (and the other arithmetic operators) where an operand's type is
/// a type parameter. The operator is its trait's method, `def add(self,
/// other: self) -> self.output`, so the parameter needs the trait, the two
/// operands must be the same type, and the result is the bound's `output`.
auto checker::check_bound_arithmetic(const ast::binary_expr &binary,
                                     type_id lhs, type_id rhs) -> type_id {
  const auto rigid_lhs = types_.entry(lhs).kind == type_kind::type_param_kind;
  const auto subject = rigid_lhs ? lhs : rhs;
  const auto other = rigid_lhs ? rhs : lhs;
  const auto op_name = ast::binary_op_name(binary.op);
  const auto trait_name = operator_trait_for(binary.op);
  const auto subject_name = types_.display(subject);
  // Every type a numeric domain allows has the builtin operator, and its
  // result is the operand type — no trait needed.
  if (domain_all(subject,
                 [&](type_id m) -> bool { return types_.is_numeric(m); })) {
    if (!types_.is_unknown(other) && other != subject) {
      error(binary.span,
            std::format("`{}` on `{}` needs another `{}`, found `{}`", op_name,
                        subject_name, subject_name, types_.display(other)),
            "operands have different types");
      return k_error_type;
    }
    return subject;
  }
  const auto fact =
      require_bound_operator(binary.span, subject, trait_name, op_name);
  if (!fact.has_value()) {
    return k_error_type;
  }
  if (!types_.is_unknown(other) && other != subject) {
    error_with_help(
        binary.span,
        std::format("`{}` on `{}` needs another `{}`, found `{}`", op_name,
                    subject_name, subject_name, types_.display(other)),
        "operands have different types",
        std::format("`{}` is `{}`'s method `{}(self, other: self)`, so both "
                    "operands have to be the same type.",
                    op_name, trait_name, trait_name));
    return k_error_type;
  }
  const auto output = bound_assoc_type(*fact, subject, "output");
  if (output == k_unknown_type) {
    error_with_help(
        binary.span,
        std::format("the bound `{}: {}` does not say what `{}` produces",
                    subject_name, display_fact(*fact), op_name),
        "result type unknown",
        std::format("`{}` declares `type output` without a default, so "
                    "nothing fixes the result of `{}` on `{}`. Give the "
                    "trait a default (`type output = self`).",
                    trait_name, op_name, subject_name));
    return k_error_type;
  }
  return output;
}

} // namespace cinder::semantic::detail
