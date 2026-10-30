#include "checker.h"
#include "src/parser/ast_traversal.h"

namespace cinder::semantic::detail {

// ==========================================================================
//  Field access and indexing
// ==========================================================================

/// `int32.max` / `int32.min` — constants on a numeric type name.
auto checker::infer_type_name_member(const ast::field_expr &field)
    -> std::optional<type_id> {
  if (field.object == nullptr ||
      field.object->kind != ast::node_kind::ident_expr) {
    return std::nullopt;
  }
  const auto &ident = ast::cast<const ast::ident_expr>(*field.object);
  if (lookup_value(ident.name) != nullptr ||
      !is_builtin_scalar_name(ident.name)) {
    return std::nullopt;
  }
  const auto type = session_.types_.builtin(ident.name);
  if ((field.field_name == "max" || field.field_name == "min") &&
      session_.types_.is_numeric(type)) {
    return type;
  }
  return k_unknown_type; // other type-level members (reflection, try_from)
}

/// Builds the comma-separated field-name list used in the "has the
/// fields ..." note on an unknown-field error.
auto checker::struct_field_names(const type_entry &instance) -> std::string {
  auto out = std::string{};
  if (const auto *fields = struct_fields_of(instance)) {
    for (const auto &field : *fields) {
      if (!out.empty()) {
        out += ", ";
      }
      out += std::format("`{}`", field.name);
    }
  }
  return out;
}

// ==========================================================================
//  Struct literals
// ==========================================================================

/// Finds a `type` declaration by name in the current module or
/// reachable through a non-wildcard import; returns the declaration paired
/// with its owning module's name.
/// The type a multi-segment path names, with the module that owns it.
/// Shared by qualified type annotations (`q.holder` in a signature) and
/// qualified struct-literal heads (`q.holder { value: 7 }`) so the two
/// spellings cannot drift apart — they used to, because the literal form
/// did not parse at all.
///
/// Multi-segment paths are validated by the qualified-path pass; this
/// only recovers the declaration when the path stays in this session.
auto checker::find_type_decl_by_path(const std::vector<std::string> &path)
    -> std::optional<std::pair<const ast::type_decl *, std::string>> {
  // A leading import alias (`use pkg.db as db` → `db.conn`): rewrite the
  // alias to the aliased module's absolute path and resolve there. This is
  // what makes a functor's `DB.conn` projection resolve, where `DB` is the
  // module parameter bound to the argument module as an import alias.
  if (const auto *binding = find_import(path.front());
      binding != nullptr && binding->leaf_name.empty()) {
    // The module the alias' head names. A whole-module import — a functor
    // parameter `DB` bound to `postgres`, or a plain `use pkg.sub` — names
    // a module directly (its `path`); a renamed member import falls back
    // to the member's owning module. `import_source_module` deliberately
    // returns null for a whole-module import (its members are reached by
    // path), so it cannot be the only source consulted here.
    const module_members *aliased =
        index_.find_module(join_strings(binding->path, "."));
    if (aliased == nullptr) {
      aliased = import_source_module(*binding);
    }
    if (aliased != nullptr) {
      auto absolute = split_module_name(aliased->module_name);
      absolute.insert(absolute.end(), path.begin() + 1, path.end());
      if (const auto *owner = find_session_module_of_path(absolute)) {
        if (const auto it = owner->types.find(absolute.back());
            it != owner->types.end()) {
          return std::pair{it->second.decl, owner->module_name};
        }
      }
    }
  }
  if (const auto *owner = find_visible_module_of_path(path)) {
    if (const auto it = owner->types.find(path.back());
        it != owner->types.end()) {
      return std::pair{it->second.decl, owner->module_name};
    }
  }
  // Last: relative to the module being checked, so a submodule declared in
  // this same file is reachable as `inner.holder` and not only as the
  // fully-qualified `main.inner.holder`.
  if (const auto *owner = find_submodule_of_current_module(path)) {
    if (const auto it = owner->types.find(path.back());
        it != owner->types.end()) {
      return std::pair{it->second.decl, owner->module_name};
    }
  }
  return std::nullopt;
}

auto checker::find_type_decl_by_name(std::string_view name)
    -> std::optional<std::pair<const ast::type_decl *, std::string>> {
  if (file_.module_ != nullptr) {
    if (const auto it = file_.module_->types.find(std::string(name));
        it != file_.module_->types.end()) {
      return std::pair{it->second.decl, file_.module_->module_name};
    }
  }
  if (const auto *binding = find_import(name)) {
    if (const auto *source = import_source_module(*binding)) {
      if (const auto it = source->types.find(imported_member_name(*binding));
          it != source->types.end()) {
        return std::pair{it->second.decl, source->module_name};
      }
    }
  }
  for (const auto *source : wildcard_import_sources()) {
    if (const auto it = source->types.find(std::string(name));
        it != source->types.end()) {
      return std::pair{it->second.decl, source->module_name};
    }
  }
  return std::nullopt;
}

/// Mirrors `find_type_decl_by_name` exactly, one map over (`.traits`
/// instead of `.types`) — used only by `infer_implements_call`'s `Trait`
/// argument, which names a trait rather than a type and so cannot go
/// through `explicit_type_argument`'s ordinary type resolution.
auto checker::find_trait_decl_by_name(std::string_view name)
    -> std::optional<const ast::trait_decl *> {
  if (file_.module_ != nullptr) {
    if (const auto it = file_.module_->traits.find(std::string(name));
        it != file_.module_->traits.end()) {
      return it->second.decl;
    }
  }
  if (const auto *binding = find_import(name)) {
    if (const auto *source = import_source_module(*binding)) {
      if (const auto it = source->traits.find(imported_member_name(*binding));
          it != source->traits.end()) {
        return it->second.decl;
      }
    }
  }
  for (const auto *source : wildcard_import_sources()) {
    if (const auto it = source->traits.find(std::string(name));
        it != source->traits.end()) {
      return it->second.decl;
    }
  }
  if (const auto found = find_prelude_trait(name)) {
    return found->decl;
  }
  return std::nullopt;
}

/// Resolves a struct-literal head's target type once the declaration is
/// known: `type_args` empty means the head named no explicit generic
/// arguments (`box { ... }`), in which case instantiation is left to the
/// field-driven inference below; otherwise resolves the explicit
/// arguments (`box[int32] { ... }`) exactly the way the same `[...]`
/// resolves in type position, arity-checked against the declaration.
auto checker::resolve_struct_literal_head(
    const ast::type_decl &decl, std::string_view owner_module,
    const std::vector<ast::type_arg> &type_args, source_span head_span)
    -> type_id {
  if (type_args.empty()) {
    return make_user_type(decl, owner_module, {});
  }
  const auto args =
      resolve_type_args(type_args, current_resolve_ctx(), &decl.type_params);
  if (type_args.size() != decl.type_params.size()) {
    auto diag = diagnostic(
        diagnostic_level::error,
        std::format("type `{}` expects {} type argument{}, found {}", decl.name,
                    decl.type_params.size(),
                    decl.type_params.size() == 1 ? "" : "s", type_args.size()),
        file_.file_id_);
    diag.with_label(head_span, "wrong number of type arguments");
    emit_diag(diag);
    mark_error();
    return k_error_type;
  }
  return make_user_type(decl, owner_module, args);
}

/// Types a struct literal `Type { field: value, ... }` (or bare `{ ... }`
/// against an expected struct type). Reports duplicate/unknown/missing
/// fields and checks each field value's type; when the target is a
/// generic struct instantiated with no explicit arguments, infers them
/// from field values whose declared type names a bare type parameter.
auto checker::check_struct_literal(const ast::struct_expr &expr,
                                   type_id expected) -> type_id {
  auto target = k_unknown_type;

  if (expr.type_name != nullptr) {
    if (expr.type_name->kind == ast::node_kind::ident_expr) {
      const auto &ident = ast::cast<const ast::ident_expr>(*expr.type_name);
      if (const auto found = find_type_decl_by_name(ident.name)) {
        target = resolve_struct_literal_head(
            *found->first, found->second, expr.type_args, expr.type_name->span);
      } else if (!file_.file_has_external_wildcard_ &&
                 !is_builtin_scalar_name(ident.name) &&
                 !builtin_generic_arity(ident.name).has_value() &&
                 find_import(ident.name) == nullptr) {
        if (function_.reported_undefined_
                .insert(std::format("type:{}", ident.name))
                .second) {
          error_with_help(
              expr.type_name->span,
              std::format("undefined type `{}`", ident.name),
              "no struct type with this name is in scope",
              std::format("Declare `type {} = {{ ... }}` or import it with "
                          "`use`.",
                          ident.name));
        }
        target = k_error_type;
      }
    } else if (expr.type_name->kind == ast::node_kind::module_path_expr) {
      // A module-qualified head, `q.holder { value: 7 }`. The path itself
      // is validated by the qualified-path pass; all that is needed here
      // is the declaration it names, looked up exactly the way the same
      // path resolves in type position.
      const auto &path =
          ast::cast<const ast::module_path_expr>(*expr.type_name);
      if (const auto found = find_type_decl_by_path(path.segments)) {
        target = resolve_struct_literal_head(
            *found->first, found->second, expr.type_args, expr.type_name->span);
      } else {
        // Reported here rather than left to the qualified-path pass,
        // which does not cover struct-literal heads: without this the
        // literal types as unknown and the failure surfaces from lowering
        // as "no concrete checked type is available for this node" — a
        // compiler-internals message for what is an ordinary typo.
        const auto spelling = join_strings(path.segments, ".");
        if (function_.reported_undefined_
                .insert(std::format("type:{}", spelling))
                .second) {
          error_with_help(
              expr.type_name->span,
              std::format("undefined type `{}`", spelling),
              std::format("module `{}` declares no type named `{}`",
                          join_strings(
                              std::vector<std::string>(path.segments.begin(),
                                                       path.segments.end() - 1),
                              "."),
                          path.segments.back()),
              "Check the spelling, or declare the type as `pub` in that "
              "module so this one can see it.");
        }
        target = k_error_type;
      }
    }
  } else if (session_.types_.entry(strip_refs(expected)).kind ==
             type_kind::struct_kind) {
    target = strip_refs(expected);
  }

  const auto &entry = session_.types_.entry(target);
  const auto *fields = struct_fields_of(entry);
  if (fields == nullptr) {
    for (const auto &field : expr.fields) {
      if (field.value != nullptr) {
        infer_expr(*field.value, k_unknown_type);
      }
    }
    return target == k_error_type ? k_error_type : target;
  }

  const auto bindings = param_bindings_for_instance(entry);
  const auto member_ctx = member_resolve_ctx(entry, bindings);

  auto initialized = std::unordered_set<std::string>{};
  auto inferred_by_field = type_scope{};

  for (const auto &field : expr.fields) {
    if (!initialized.insert(field.name).second) {
      error(field.span,
            std::format("field `{}` is initialized more than once", field.name),
            "duplicate field");
      continue;
    }
    const ast::struct_field *declared = nullptr;
    for (const auto &candidate : *fields) {
      if (candidate.name == field.name) {
        declared = &candidate;
        break;
      }
    }
    if (declared == nullptr) {
      auto diag = diagnostic(
          diagnostic_level::error,
          std::format("unknown field `{}` in struct literal for `{}`",
                      field.name, entry.name),
          file_.file_id_);
      diag.with_label(field.span, "no such field");
      if (const auto names = struct_field_names(entry); !names.empty()) {
        diag.with_note(
            std::format("`{}` has the fields {}", entry.name, names));
      }
      emit_diag(diag);
      mark_error();
      if (field.value != nullptr) {
        infer_expr(*field.value, k_unknown_type);
      }
      continue;
    }
    const auto field_expected = declared->type != nullptr
                                    ? resolve_type(*declared->type, member_ctx)
                                    : k_unknown_type;
    auto found = k_unknown_type;
    if (field.value != nullptr) {
      found = infer_expr(*field.value, field_expected);
      type_mismatch(field.value->span, field_expected, found, "for this field",
                    field.value.get());
    } else if (const auto *binding = lookup_value(field.name)) {
      // Shorthand `{name}` binds the in-scope value of the same name.
      found =
          binding->generic_lambda != nullptr
              ? generic_lambda_value(
                    {.node = &field, .name = field.name, .span = field.span},
                    *binding, field_expected)
              : binding->type;
      type_mismatch(field.span, field_expected, found, "for this field");
    }
    // Recorded unconditionally (even when neither branch above ran, in
    // which case `found` is still `k_unknown_type`) so lowering's lookup
    // is a plain `.find` — see `session_.struct_literal_field_types_`'s doc
    // comment.
    session_.struct_literal_field_types_[&field] = found;
    session_.struct_literal_field_expected_[&field] = field_expected;
    inferred_by_field.emplace(field.name, found);
  }

  auto missing = std::string{};
  for (const auto &field : *fields) {
    if (!initialized.contains(field.name)) {
      if (!missing.empty()) {
        missing += ", ";
      }
      missing += std::format("`{}`", field.name);
    }
  }
  if (!missing.empty()) {
    error_with_help(
        expr.span,
        std::format("missing field{} {} in struct literal for `{}`",
                    missing.find(',') != std::string::npos ? "s" : "", missing,
                    entry.name),
        "struct literal is incomplete",
        "Every struct field must be initialized; Cinder structs have no "
        "default field values.");
  }

  // Instantiate generic structs from the field values. A field declared as
  // a bare type parameter binds it directly; any other field type that
  // mentions a parameter (`w: wrap[T]`) is matched against the value's
  // type with the struct's own parameters as the pattern's unknowns.
  if (entry.decl != nullptr && !entry.decl->type_params.empty() &&
      entry.args.empty()) {
    auto pattern_bindings = type_scope{};
    for (const auto &param : entry.decl->type_params) {
      if (!param.name.empty() && !param.is_value_param) {
        pattern_bindings.emplace(param.name, param_id(param));
      }
    }
    const auto pattern_ctx = member_resolve_ctx(entry, pattern_bindings);
    auto args = std::vector<type_id>{};
    for (const auto &param : entry.decl->type_params) {
      auto bound = k_unknown_type;
      for (const auto &field : *fields) {
        const auto it = inferred_by_field.find(field.name);
        if (field.type == nullptr || it == inferred_by_field.end()) {
          continue;
        }
        if (field.type->kind == ast::node_kind::named_type) {
          const auto &named = ast::cast<const ast::named_type>(*field.type);
          if (named.path.size() == 1 && named.path.front() == param.name) {
            bound = it->second;
            break;
          }
        }
        if (param.is_value_param || it->second == k_unknown_type ||
            it->second == k_error_type) {
          continue;
        }
        const auto pattern = resolve_type(*field.type, pattern_ctx);
        const auto matched =
            infer::match_pattern(session_.types_, pattern, it->second);
        if (const auto solved = matched.bindings.find(param_id(param));
            solved != matched.bindings.end()) {
          bound = solved->second;
          break;
        }
      }
      args.push_back(bound);
    }
    return make_user_type(*entry.decl, entry.module_name, std::move(args));
  }

  return target;
}

// ==========================================================================
//  Blocks and body values
// ==========================================================================

/// Checks a list of body nodes in a fresh scope and returns the value type
/// of the final expression statement (`unit` otherwise).
auto checker::check_body_nodes(const std::vector<ast::ptr<ast::node>> &items,
                               type_id expected_tail) -> type_id {
  push_scope();
  auto last = session_.types_.builtin("unit");
  // Every statement is still visited below regardless of reachability —
  // `hir::lower` walks this same list unconditionally and needs every
  // node's type/resolution recorded, so this never skips a `check_body_
  // node` call. It only tracks, for `check_static_decl`'s `assertion`
  // case, whether a *preceding sibling* in this same block is already
  // known (for this concrete instantiation) to have returned — see
  // `stmt_definitely_returns`'s doc comment for why a `static assert`
  // reached only past one of those must not be judged reachable.
  const auto saved_unreachable = function_.in_known_unreachable_code_;
  function_.in_known_unreachable_code_ = false;
  // Narrowings past a returning `static if` hold for the rest of this block
  // and no further (phase 9, rule 5).
  auto narrowed_past = size_t{0};
  for (size_t i = 0; i < items.size(); ++i) {
    if (items[i] == nullptr) {
      continue;
    }
    const auto is_last = i + 1 == items.size();
    if (function_.in_comptime_only_function_) {
      // The checker owns the rewrite of the body it is checking.
      rewrite_call_time_static(const_cast<ast::ptr<ast::node> &>(items[i]));
    }
    last = check_body_node(*items[i], is_last ? expected_tail : k_unknown_type);
    if (!is_last) {
      last = function_.in_known_unreachable_code_
                 ? session_.types_.builtin("never")
                 : session_.types_.builtin("unit");
      if (stmt_definitely_returns(*items[i])) {
        function_.in_known_unreachable_code_ = true;
      }
      if (const auto past = narrowing_past(*items[i])) {
        scoped_domains_.emplace_back(past->param, *past->else_domain);
        ++narrowed_past;
        if (past->else_domain->empty()) {
          function_.in_known_unreachable_code_ = true;
        }
      }
    }
  }
  scoped_domains_.resize(scoped_domains_.size() - narrowed_past);
  function_.in_known_unreachable_code_ = saved_unreachable;
  pop_scope();
  return last;
}

/// Types a lambda expression. Parameter types come from an explicit
/// annotation, or (failing that) from the corresponding slot of an
/// expected `fn(...)` type; the declared/expected return type is checked
/// against the body's inferred type. Always yields a concrete `fn(...)`
/// type — Cinder monomorphizes closures, so there is no separate closure
/// type distinct from the function-value type it's assigned/passed as.
/// The set of names a lambda's body may reach across the lambda boundary,
/// or `nullopt` when it declared no capture list (capture implicitly, the
/// pre-existing behavior). Note that `[]` yields an *empty set*, not
/// `nullopt`: "capture nothing" and "capture everything used" are opposite
/// answers and must not collapse.
auto checker::capture_allow_set(const ast::lambda_expr &lambda)
    -> std::optional<std::unordered_map<std::string, ast::capture_mode>> {
  if (!lambda.captures.has_value()) {
    return std::nullopt;
  }
  auto allowed = std::unordered_map<std::string, ast::capture_mode>{};
  for (const auto &entry : *lambda.captures) {
    allowed.insert_or_assign(entry.name, entry.mode);
  }
  return allowed;
}

/// Whether `name` names a function or `static` this file can already see
/// without capturing it — a module member, an import, or a prelude name.
auto checker::is_module_level_value(std::string_view name) -> bool {
  const auto key = std::string(name);
  if (file_.module_ != nullptr && (file_.module_->functions.contains(key) ||
                                   file_.module_->statics.contains(key))) {
    return true;
  }
  if (find_import(name) != nullptr) {
    return true;
  }
  for (const auto *source : wildcard_import_sources()) {
    if (source->functions.contains(key) || source->statics.contains(key)) {
      return true;
    }
  }
  return is_prelude_value_name(name);
}

/// Warns about a listed name the body never read. Runs after the body has
/// been checked, while the barrier is still on the stack.
auto checker::warn_unused_captures(const ast::lambda_expr &lambda,
                                   const capture_barrier &barrier) -> void {
  if (!lambda.captures.has_value()) {
    return;
  }
  for (const auto &entry : *lambda.captures) {
    if (barrier.used.contains(entry.name) ||
        barrier.invalid.contains(entry.name)) {
      continue;
    }
    auto diag =
        diagnostic(diagnostic_level::warning,
                   std::format("`{}` is captured but never used", entry.name),
                   file_.file_id_);
    diag.with_label(entry.span, "this capture has no effect");
    diag.with_help(std::format(
        "The lambda's body never reads `{}`, so listing it only widens "
        "what the lambda can reach. Remove it from the capture list.",
        entry.name));
    emit_diag(diag);
  }
}

/// Validates a lambda's explicit capture list against the enclosing scope.
/// Must run before the lambda's own scope and barrier are pushed, so each
/// entry resolves where the programmer wrote it.
auto checker::check_capture_list(const ast::lambda_expr &lambda)
    -> std::unordered_set<std::string> {
  auto invalid = std::unordered_set<std::string>{};
  if (!lambda.captures.has_value()) {
    return invalid;
  }

  auto seen = std::unordered_map<std::string, source_span>{};
  for (const auto &entry : *lambda.captures) {
    if (const auto it = seen.find(entry.name); it != seen.end()) {
      auto diag = diagnostic(
          diagnostic_level::error,
          std::format("`{}` is captured twice in this list", entry.name),
          file_.file_id_);
      diag.with_label(entry.span, "captured again here");
      diag.with_secondary_label(it->second, "first captured here");
      diag.with_help("Each variable may appear at most once in a capture "
                     "list; a second entry cannot mean anything the first "
                     "one didn't already say.");
      emit_diag(diag);
      mark_error();
      invalid.insert(entry.name);
      continue;
    }
    seen.emplace(entry.name, entry.span);

    const auto found = find_value_in_scopes(entry.name);
    if (found.binding == nullptr) {
      // A module-level name in a capture list is a misunderstanding of
      // what a list is for, not a typo — say which one it is.
      if (is_module_level_value(entry.name)) {
        auto diag = diagnostic(
            diagnostic_level::error,
            std::format("`{}` does not need to be captured", entry.name),
            file_.file_id_);
        diag.with_label(entry.span, "this is a module-level item");
        diag.with_help(std::format(
            "A capture list controls which *local* variables cross into "
            "the lambda. `{}` is declared at module level, so it is in "
            "scope inside the lambda whether it is listed or not — remove "
            "it from the list.",
            entry.name));
        emit_diag(diag);
        mark_error();
        invalid.insert(entry.name);
        continue;
      }
      auto diag = diagnostic(diagnostic_level::error,
                             std::format("cannot capture `{}`", entry.name),
                             file_.file_id_);
      diag.with_label(entry.span,
                      "no local variable with this name is in scope here");
      if (const auto suggestion =
              best_suggestion(entry.name, value_name_candidates())) {
        diag.with_help(std::format("did you mean `{}`?", *suggestion));
      } else {
        diag.with_help(
            "A capture list names local variables from the enclosing "
            "scope. Module-level functions and `static` bindings are "
            "always in scope and are never captured.");
      }
      emit_diag(diag);
      mark_error();
      invalid.insert(entry.name);
      continue;
    }
    if (found.blocked_by != nullptr) {
      // The name exists, but an enclosing lambda already put it out of
      // reach — capturing it here would reach through a boundary that
      // refused it.
      emit_capture_not_listed(entry.span, entry.name, *found.blocked_by);
      invalid.insert(entry.name);
      continue;
    }

    // Naming an outer local in *this* list is itself a use of it as far as
    // any enclosing capture list is concerned: the value has to reach here
    // before it can reach further in.
    note_capture_use(entry.name, found.depth);

    // Capturing by reference through an enclosing lambda that captured the
    // same name *by value* would write into that intermediate copy, not
    // into the original. The by-value copy is where the chain breaks, so
    // point at it rather than letting the write silently go nowhere.
    if (ast::is_reference_capture(entry.mode)) {
      for (const auto &barrier : function_.capture_barriers_) {
        if (barrier.scope_floor <= found.depth ||
            !barrier.allowed.has_value()) {
          continue;
        }
        const auto outer = barrier.allowed->find(entry.name);
        if (outer == barrier.allowed->end() ||
            ast::is_reference_capture(outer->second)) {
          continue;
        }
        auto diag = diagnostic(
            diagnostic_level::error,
            std::format("cannot capture `{}` by reference through an "
                        "enclosing by-value capture",
                        entry.name),
            file_.file_id_);
        diag.with_label(
            entry.span,
            std::format("this reaches `{}` by reference", entry.name));
        if (barrier.lambda != nullptr) {
          diag.with_secondary_label(
              barrier.lambda->captures_span,
              std::format("but this enclosing lambda captured `{}` by "
                          "value, so only its own copy is reachable here",
                          entry.name));
        }
        diag.with_help(std::format(
            "Write `&{0}` (or `&mut {0}`) in the enclosing lambda's capture "
            "list too, so the reference reaches the original binding rather "
            "than a copy of it.",
            entry.name));
        emit_diag(diag);
        mark_error();
        invalid.insert(entry.name);
        break;
      }
    }

    if (entry.mode == ast::capture_mode::by_mut_ref &&
        found.binding->origin != binding_origin::var_binding &&
        found.binding->origin != binding_origin::mut_binding) {
      auto diag = diagnostic(
          diagnostic_level::error,
          std::format("cannot capture `{}` by mutable reference", entry.name),
          file_.file_id_);
      diag.with_label(entry.span,
                      std::format("`{}` is not declared mutable", entry.name));
      diag.with_secondary_label(found.binding->span, "declared here");
      diag.with_help(std::format(
          "`&mut {0}` lets the lambda write through to the original "
          "binding, so `{0}` has to be mutable. Declare it with `var` "
          "instead of `let`, or capture it by value as `{0}` or by shared "
          "reference as `&{0}`.",
          entry.name));
      emit_diag(diag);
      mark_error();
      invalid.insert(entry.name);
    }
  }
  return invalid;
}

auto checker::infer_lambda(const ast::lambda_expr &lambda, type_id expected)
    -> type_id {
  const auto &expected_entry = session_.types_.entry(strip_refs(expected));
  const auto expected_is_fn = expected_entry.kind == type_kind::fn_kind;

  // A lambda body is a separate function: a loop outside it is not one its
  // `break` could reach.
  const auto saved_loop_depth = std::exchange(function_.loop_depth_, 0U);

  // Validated *before* the barrier and the body scope go up, so each entry
  // resolves against the enclosing scope the way the programmer wrote it.
  auto invalid_captures = check_capture_list(lambda);

  push_scope();
  function_.capture_barriers_.push_back(capture_barrier{
      .scope_floor = function_.scopes_.size() - 1,
      .lambda = &lambda,
      .allowed = capture_allow_set(lambda),
      .invalid = std::move(invalid_captures),
  });
  auto param_types = std::vector<type_id>{};
  for (size_t i = 0; i < lambda.params.size(); ++i) {
    const auto &param = lambda.params[i];
    auto type = k_unknown_type;
    if (param.type_annotation != nullptr) {
      type = resolve_type(*param.type_annotation, current_resolve_ctx());
    } else if (expected_is_fn && i < expected_entry.args.size()) {
      type = expected_entry.args[i];
    }
    param_types.push_back(type);
    if (param.pattern != nullptr &&
        param.pattern->kind == ast::node_kind::binding_pattern) {
      const auto &binding =
          ast::cast<const ast::binding_pattern>(*param.pattern);
      bind_value(binding.name, type, binding_origin::parameter, param.span);
    } else if (param.pattern != nullptr) {
      check_pattern(ast::cast<const ast::pattern>(*param.pattern), type);
    }
  }

  auto declared_result = k_unknown_type;
  if (lambda.return_type != nullptr) {
    declared_result = resolve_type(*lambda.return_type, current_resolve_ctx());
  } else if (expected_is_fn) {
    declared_result = expected_entry.result;
  }

  // An inferred expectation that still mentions an unsolved type parameter
  // is withheld from the body. Propagating it makes the body infer *as*
  // that parameter — a `p => if ...: @some(x) else: @none` checked against
  // `option[U]` comes back `option[U]`, not `option[int32]` — and a body
  // type that mentions `U` tells the call-site solver nothing, so `U` is
  // then reported unsolved on a call that plainly determines it. Checked
  // against no expectation the body answers concretely and `U` falls out.
  //
  // Only for an expectation the *call site* supplied: a `return_type` the
  // user wrote is theirs to be checked against, abstract or not.
  const auto body_expectation =
      lambda.return_type == nullptr && mentions_abstract_type(declared_result)
          ? k_unknown_type
          : declared_result;

  // A `return` inside a lambda body returns from the *lambda*, so it must
  // be checked against the lambda's own result, not the enclosing
  // function's. Without this a block-bodied lambda in an `int64`-returning
  // function could not `return` an `int32` even when its own declared
  // result said `int32`. (`lower_lambda` already scopes the enclosing
  // function's postconditions out for the same reason.)
  const auto saved_return =
      std::exchange(function_.return_type_, declared_result);
  const auto saved_annotated = std::exchange(function_.return_annotated_,
                                             declared_result != k_unknown_type);
  const auto saved_inferring =
      std::exchange(function_.inferring_return_, false);
  const auto restore_return = [&] -> void {
    function_.return_type_ = saved_return;
    function_.return_annotated_ = saved_annotated;
    function_.inferring_return_ = saved_inferring;
  };

  auto body_result = k_unknown_type;
  if (lambda.body_expr != nullptr) {
    body_result = infer_expr(*lambda.body_expr, body_expectation);
    if (body_expectation != k_unknown_type) {
      type_mismatch(lambda.body_expr->span, body_expectation, body_result,
                    "as the lambda result");
    }
  } else {
    body_result = check_body_nodes(lambda.body_stmts, body_expectation);
  }
  restore_return();
  warn_unused_captures(lambda, function_.capture_barriers_.back());
  function_.capture_barriers_.pop_back();
  pop_scope();
  function_.loop_depth_ = saved_loop_depth;

  auto result =
      declared_result != k_unknown_type ? declared_result : body_result;
  // An expected result that still mentions an unsolved type parameter
  // (the `M[B]` in `bind`'s function argument) yields to a concrete body
  // type: reporting `option[B]` back would tell rigid inference nothing
  // at a call site that plainly determines `B`.
  if (result != body_result && body_result != k_unknown_type &&
      mentions_abstract_type(result) &&
      !mentions_abstract_type(body_result, /*leaves_are_answers=*/true)) {
    result = body_result;
  }
  return session_.types_.fn_of(std::move(param_types), result);
}

/// Checks an `if`/`elif` branch's condition (requiring `bool`) or, for an
/// `if let` branch, infers the scrutinee and checks the pattern against it.
/// Both callers run this with the branch's own scope already open, so the
/// names an `if let` pattern binds are visible in that branch's body and
/// nowhere else — matching how lowering scopes them (`lower_if_let_chain`,
/// `hir/lower.cpp`) and how `while let` already behaves.
auto checker::check_if_branch_header(const ast::if_branch &branch) -> void {
  if (branch.condition != nullptr) {
    require_bool(*branch.condition, "an `if` condition");
  }
  if (branch.let_expr != nullptr && branch.let_pattern != nullptr) {
    const auto subject = infer_expr(*branch.let_expr, k_unknown_type);
    check_pattern(ast::cast<const ast::pattern>(*branch.let_pattern),
                  strip_refs(subject));
  } else if (branch.let_expr != nullptr) {
    infer_expr(*branch.let_expr, k_unknown_type);
  }
}

/// Joins branch value types, diagnosing genuinely conflicting branches.
auto checker::join_branch_type(type_id current, type_id found, source_span span,
                               std::string_view construct) -> type_id {
  // `never` is the bottom: a branch that jumps away contributes nothing,
  // whichever position it is in.
  if (session_.types_.entry(found).name == "never") {
    return session_.types_.is_unknown(current) ? found : current;
  }
  // The branches are one value, so their leaves are tied now. Without
  // this, an open leaf counts as unknown below and is simply dropped for
  // the other branch's type: `@some(v) => v` joined with `@none => 0`
  // answered with the `0`'s leaf, and a later `let w: int64 = r` never
  // reached `v`'s literal, which defaulted to `int32`.
  if (leaf_ctxt_.meta_count() != 0 &&
      session_.types_.entry(current).name != "never") {
    solve_leaves(current, found);
    current = leaf_ctxt_.zonk(current);
    found = leaf_ctxt_.zonk(found);
  }
  if (session_.types_.is_unknown(current) ||
      session_.types_.entry(current).name == "never") {
    return found;
  }
  if (session_.types_.is_unknown(found)) {
    return current;
  }
  if (!session_.types_.compatible(current, found) &&
      !session_.types_.compatible(found, current)) {
    error_with_help(
        span,
        std::format("{} branches have mismatched types: `{}` vs `{}`",
                    construct, session_.types_.display(current),
                    session_.types_.display(found)),
        std::format("this branch produces `{}`",
                    session_.types_.display(found)),
        "Every branch of a value-producing conditional must produce the "
        "same type.");
    return current;
  }
  return current;
}

/// Types an `if`/`elif`/`else` expression: each branch's tail value is
/// joined via `join_branch_type`, reporting a mismatch if any two
/// branches produce genuinely incompatible types. An `if` with no `else`
/// still yields the joined branch type, since exhaustiveness for boolean
/// conditions is not enforced the way `match` exhaustiveness is.
auto checker::infer_if_expr(const ast::if_expr &expr, type_id expected)
    -> type_id {
  auto result = expected;
  for (const auto &branch : expr.branches) {
    push_scope();
    check_if_branch_header(branch);
    // Inside a branch, its condition holds — and so does the refutation of
    // every branch before it, since reaching this one means those failed.
    auto assumed = fact_scope(function_.facts_);
    for (const auto &earlier : expr.branches) {
      if (&earlier == &branch) {
        break;
      }
      assume_condition(earlier.condition.get(), /*negated=*/true);
    }
    assume_condition(branch.condition.get(), /*negated=*/false);
    if (branch.let_pattern != nullptr && branch.let_expr == nullptr) {
      check_pattern(ast::cast<const ast::pattern>(*branch.let_pattern),
                    k_unknown_type);
    }
    const auto branch_type = check_body_nodes(branch.body, expected);
    pop_scope();
    result =
        join_branch_type(result, branch_type,
                         branch.body.empty() || branch.body.back() == nullptr
                             ? branch.span
                             : branch.body.back()->span,
                         "`if`");
  }
  if (!expr.else_body.empty()) {
    // Reaching the `else` means every branch condition was false.
    auto refuted = fact_scope(function_.facts_);
    for (const auto &branch : expr.branches) {
      assume_condition(branch.condition.get(), /*negated=*/true);
    }
    const auto else_type = check_body_nodes(expr.else_body, expected);
    result = join_branch_type(result, else_type,
                              expr.else_body.back() != nullptr
                                  ? expr.else_body.back()->span
                                  : expr.span,
                              "`if`");
  }
  return result;
}

/// Types a loop head's iterable and, when it is a user type that
/// implements `std.iter`'s protocol, records the `next`-method dispatch
/// lowering needs to desugar the loop into
/// `while let @some(x) = it.next(): ...`. Returns the element type one
/// pass of the loop yields.
///
/// Shared by the statement `for` and by each clause of a `for ... =>
/// yield` comprehension: the two reach the identical loop lowerers, so
/// resolving the iterable differently for them is how a comprehension
/// over a `list` ended up with no route at all once `list` stopped being
/// a builtin.
///
/// `site` is the node instantiation diagnostics point at; `out` is filled
/// in only when a dispatch was found, and left untouched otherwise.
auto checker::resolve_loop_iterable(const ast::expr &iterable_expr,
                                    const ast::node &site,
                                    iterator_loop_dispatch &out) -> type_id {
  // The route (`into_iter`, `iter`, `next`) is chosen from the iterable's
  // head alone, so only the head is owed a default here. The *instances*
  // it names are another matter: they are named after the whole type, and
  // `list::into_iter$list___` is a function nothing ever compiles. So a
  // loop over `[1, 2]` — elements still open until the body's `t + x` with
  // `t: int64` says what they are — is routed now without naming anything
  // (a `site` of null is the resolvers' probe mode), and re-routed against
  // the settled type once the leaves are known: the deferral an ordinary
  // method call on an open receiver gets.
  const auto iterable = demand_shape(infer_expr(iterable_expr, k_unknown_type));
  if (!mentions_type_var(iterable)) {
    defer_loop_route(iterable_expr, site, iterable);
    return resolve_loop_route(iterable_expr, iterable, &site, out);
  }
  const auto element =
      resolve_loop_route(iterable_expr, iterable, nullptr, out);
  if (out.decl != nullptr) {
    defer_method_call(
        "a `for` loop's iterator", iterable, std::vector<type_id>{},
        [this, &iterable_expr, &site, &out](type_id settled) -> void {
          defer_loop_route(iterable_expr, site, settled);
          auto routed = iterator_loop_dispatch{};
          (void)resolve_loop_route(iterable_expr, settled, &site, routed);
          if (routed.decl != nullptr) {
            out = std::move(routed);
          }
        });
  }
  return element;
}

/// A template's loop over something written in its parameters, routed by
/// each instance: `for x in xs` on a `list[T]` calls that instance's own
/// `list::into_iter`. The dispatch lives in `session_.for_iterator_dispatches_`
/// for a `for` statement and `session_.comprehension_iterator_dispatches_` for
/// a comprehension clause, keyed on `site` either way.
auto checker::defer_loop_route(const ast::expr &iterable_expr,
                               const ast::node &site, type_id iterable)
    -> void {
  if (function_.current_template_ == nullptr ||
      !mentions_template_param(iterable)) {
    return;
  }
  defer_to_instances(
      site,
      [this, &iterable_expr, &site, iterable](instance_subst &subst) -> void {
        const auto concrete = substitute_type(iterable, subst);
        if (mentions_template_param(concrete)) {
          return;
        }
        const auto &clone_site = *clone_of(subst, &site);
        auto routed = iterator_loop_dispatch{};
        (void)resolve_loop_route(*clone_of(subst, &iterable_expr), concrete,
                                 &clone_site, routed);
        if (routed.decl == nullptr) {
          return;
        }
        if (site.kind == ast::node_kind::for_stmt) {
          session_.for_iterator_dispatches_[&ast::cast<const ast::for_stmt>(
              clone_site)] = std::move(routed);
        } else {
          session_.comprehension_iterator_dispatches_[&clone_site] =
              std::move(routed);
        }
      });
}

/// `resolve_loop_iterable`'s routing half, for an iterable already typed.
/// `site` null routes without requesting any instance.
auto checker::resolve_loop_route(const ast::expr &iterable_expr,
                                 type_id iterable, const ast::node *site,
                                 iterator_loop_dispatch &out) -> type_id {
  auto element = element_type_of(iterable, iterable_expr.span);

  // A bare `&`/`&mut` as the *whole* iterable (`for x in &v`) asks for
  // the non-consuming `iter`/`iter_mut` route, tried before anything
  // else so it never falls back to `into_iterator`'s always-consuming
  // `into_iter` for a collection the caller explicitly did not want
  // moved (spec/todo.md #20).
  const auto *borrow = ast::as<const ast::unary_expr>(&iterable_expr);
  const auto is_top_level_borrow = borrow != nullptr &&
                                   borrow->operand != nullptr &&
                                   (borrow->op == ast::unary_op::addr_of ||
                                    borrow->op == ast::unary_op::addr_of_mut);
  auto borrowed = is_top_level_borrow
                      ? try_resolve_iter_borrow(
                            strip_refs(iterable),
                            borrow->op == ast::unary_op::addr_of_mut, site)
                      : std::nullopt;

  // `into_iterator` next: a type that both *is* an iterator and can
  // *hand back* one should hand it back, since being consumed by a
  // loop is the more surprising of the two readings. A collection
  // reaches a loop only through this path — it has no `next` of its
  // own.
  if (borrowed) {
    element = borrowed->element_type;
    out = std::move(*borrowed);
  } else if (auto into = try_resolve_into_iterator(iterable, site)) {
    element = into->element_type;
    out = std::move(*into);
  } else if (auto iter = try_resolve_iterator(iterable, site)) {
    out = std::move(*iter);
  }
  return element;
}

/// Types a `for ... => yield` comprehension expression: each clause's
/// iterable is unwrapped to its element type via `element_type_of` and
/// bound to that clause's pattern(s), the optional guard must be `bool`,
/// and the result is always `list[yield_type]`.
auto checker::infer_for_expr(const ast::for_expr &expr) -> type_id {
  push_scope();
  for (const auto &clause : expr.clauses) {
    auto element = k_unknown_type;
    if (clause.iterable != nullptr) {
      const auto *key = static_cast<const ast::node *>(clause.iterable.get());
      element = resolve_loop_iterable(
          *clause.iterable, *clause.iterable,
          session_.comprehension_iterator_dispatches_[key]);
      if (session_.comprehension_iterator_dispatches_[key].decl == nullptr) {
        session_.comprehension_iterator_dispatches_.erase(key);
      }
    }
    auto patterns = std::vector<const ast::pattern *>{};
    patterns.reserve(clause.patterns.size());
    for (const auto &pattern : clause.patterns) {
      patterns.push_back(ast::as<const ast::pattern>(pattern.get()));
    }
    check_loop_head_patterns(patterns, element);
  }
  if (expr.guard != nullptr) {
    require_bool(*expr.guard, "a `for` guard");
  }
  auto yield_type = k_unknown_type;
  if (expr.yield_expr != nullptr) {
    yield_type = infer_expr(*expr.yield_expr, k_unknown_type);
  }
  pop_scope();
  const auto list_type = resolve_list_type(yield_type);
  resolve_comprehension_dispatch(expr, list_type);
  return list_type;
}

/// When `operand_err` (the `?` operand's `result[_, E]` error type)
/// differs from `fn_err` (the enclosing function's declared error type),
/// requires an `impl from[operand_err] for fn_err` and records the
/// resolved conversion in
/// `session_.try_conversions_`/`session_.try_conversion_types_` for
/// `hir::lower_try` to apply — mirroring how `require_operand_trait`
/// records `session_.operator_dispatches_` for operator overloading. Identical
/// error types (including two unknowns) need no conversion and are a
/// silent no-op, matching `?`'s current same-type-only behavior.
auto checker::maybe_wire_try_conversion(const ast::try_expr &expr,
                                        type_id operand_err, type_id fn_err,
                                        type_id fn_return_type) -> void {
  // The conversion a template's `?` calls is each instance's to name: its
  // error types, or the `from` impl's instance, depend on the parameters.
  if (function_.current_template_ != nullptr &&
      (mentions_template_param(operand_err) ||
       mentions_template_param(fn_err))) {
    defer_to_instances(expr,
                       [this, &expr, operand_err, fn_err,
                        fn_return_type](instance_subst &subst) -> void {
                         maybe_wire_try_conversion(
                             *clone_of(subst, &expr),
                             substitute_type(operand_err, subst),
                             substitute_type(fn_err, subst),
                             substitute_type(fn_return_type, subst));
                       });
  }
  const auto stripped_operand_err = strip_refs(operand_err);
  const auto stripped_fn_err = strip_refs(fn_err);
  if (stripped_operand_err == stripped_fn_err ||
      session_.types_.is_unknown(stripped_operand_err) ||
      session_.types_.is_unknown(stripped_fn_err)) {
    return;
  }
  const auto trait_args = trait_args_of_impl_for(stripped_fn_err, "from");
  const auto convertible = trait_args.has_value() && !trait_args->empty() &&
                           strip_refs((*trait_args)[0]) == stripped_operand_err;
  if (!convertible) {
    error_with_help(
        expr.span,
        std::format("cannot propagate `{}` with `?` in a function that returns "
                    "`result[_, {}]`",
                    session_.types_.display(stripped_operand_err),
                    session_.types_.display(stripped_fn_err)),
        "no conversion from this error type exists",
        std::format("Add `impl from[{}] for {}` with a `from` method to "
                    "convert between error types.",
                    session_.types_.display(stripped_operand_err),
                    session_.types_.display(stripped_fn_err)));
    return;
  }
  const auto &fn_err_entry = session_.types_.entry(stripped_fn_err);
  const auto *method = find_method(fn_err_entry, "from", stripped_fn_err);
  if (method == nullptr) {
    return;
  }
  const auto *callee =
      instantiate_impl_method_for(expr, *method, fn_err_entry, stripped_fn_err);
  session_.try_conversions_[&expr] = resolved_callee{
      .decl = callee != nullptr ? callee : method->decl,
      .owner_module = method->owner->module_name,
      .impl_target_type = callee != nullptr ? "" : fn_err_entry.name,
      .receiver = nullptr};
  session_.try_conversion_types_[&expr] = fn_return_type;
}

/// Types `expr?`: the operand must be `result`/`option`, and (when the
/// enclosing function's return type is known) that return type must also
/// be `result`/`option` for the early-return side of `?` to make sense.
/// When the operand's error type differs from the enclosing function's,
/// a `from`-conversion is required and applied automatically (see
/// `maybe_wire_try_conversion`). Yields the wrapped success type.
auto checker::infer_try(const ast::try_expr &expr) -> type_id {
  if (expr.operand == nullptr) {
    return k_unknown_type;
  }
  const auto operand = strip_refs(infer_expr(*expr.operand, k_unknown_type));
  const auto &entry = session_.types_.entry(operand);

  const auto is_wrapper = entry.kind == type_kind::builtin_generic_kind &&
                          (entry.name == "result" || entry.name == "option");
  if (!is_wrapper && !session_.types_.is_unknown(operand)) {
    error_with_help(
        expr.span,
        std::format("`?` requires a `result` or `option` value, found `{}`",
                    session_.types_.display(operand)),
        "cannot propagate from this value",
        "`?` unwraps `@ok`/`@some` and returns `@err`/`@none` early; only "
        "`result` and `option` values support it.");
    return k_error_type;
  }

  if (function_.return_annotated_) {
    const auto return_type = strip_refs(function_.return_type_);
    const auto &return_entry = session_.types_.entry(return_type);
    const auto returns_wrapper =
        return_entry.kind == type_kind::builtin_generic_kind &&
        (return_entry.name == "result" || return_entry.name == "option");
    if (!returns_wrapper &&
        !session_.types_.is_unknown(function_.return_type_)) {
      error_with_help(
          expr.span,
          std::format("cannot use `?` in a function that returns `{}`",
                      session_.types_.display(function_.return_type_)),
          "`?` needs a `result` or `option` return type to propagate into",
          "Change the function to return `result[T, E]` (or `option[T]`), "
          "or handle the failure here with `match`.");
    } else if (is_wrapper && entry.name == "result" &&
               return_entry.name == "result" && entry.args.size() > 1 &&
               return_entry.args.size() > 1) {
      maybe_wire_try_conversion(expr, entry.args[1], return_entry.args[1],
                                return_type);
    }
  }

  return is_wrapper && !entry.args.empty() ? entry.args[0] : k_unknown_type;
}

/// The built-in context an `on(target)` block runs on: `pool` and `cpu` name
/// the thread pool's `cpu` context, `io` names `io`. Any other target is a
/// user-defined context, which this check does not model, so it is unknown.
auto checker::context_of_on_target(const ast::on_expr &expr) -> type_id {
  const auto *named = ast::as<const ast::named_type>(expr.context_type.get());
  if (named == nullptr || named->path.size() != 1) {
    return k_unknown_type;
  }
  const auto &name = named->path.front();
  if (name == "pool" || name == "cpu") {
    return session_.types_.builtin("cpu");
  }
  return name == "io" ? session_.types_.builtin("io") : k_unknown_type;
}

/// Types `await expr` (or bare `await yield`, which yields `unit`).
/// Unwraps a `task[T, E, C]` operand to `result[T, E]` (or plain `T` when
/// the task cannot fail); any other operand type passes through
/// unchanged, since full task/context typing is not yet modeled.
auto checker::infer_await(const ast::await_expr &expr) -> type_id {
  if (expr.operand == nullptr) {
    return session_.types_.builtin("unit"); // `await yield`
  }
  if (!function_.in_async_) {
    error_with_help(expr.span, "`await` outside an `async` function",
                    "not inside an `async` function",
                    "Mark the enclosing function `async def`, or run the "
                    "task from a `crew`.");
  }
  const auto operand = strip_refs(infer_expr(*expr.operand, k_unknown_type));
  // `await par:` waits for every branch and gives a tuple of their results.
  if (expr.operand->kind == ast::node_kind::par_expr &&
      session_.types_.entry(operand).kind == type_kind::tuple_kind) {
    auto results = std::vector<type_id>{};
    for (const auto branch : session_.types_.entry(operand).args) {
      results.push_back(awaited_type(branch, expr.span));
    }
    return session_.types_.tuple_of(std::move(results));
  }
  return awaited_type(operand, expr.span);
}

/// Checks a `crew` block. The one option is `on_error: collect`, which lets
/// the body read the errors its tasks finished with through `name.errors()`.
auto checker::check_crew(const std::string &name,
                         const std::vector<ast::crew_option> &options,
                         const std::vector<ast::ptr<ast::node>> &body,
                         source_span span) -> type_id {
  auto scope = session_state::crew_scope{};
  for (const auto &option : options) {
    if (option.name == "on_error" && option.value == "collect") {
      scope.collects = true;
    } else {
      error_with_help(option.span,
                      std::format("`crew` has no option `{}: {}`", option.name,
                                  option.value),
                      "unknown crew option",
                      "The only option is `on_error: collect`.");
    }
  }
  const auto saved = session_.crews_.find(name) != session_.crews_.end()
                         ? std::optional{session_.crews_.at(name)}
                         : std::nullopt;
  session_.crews_[name] = scope;
  push_scope();
  bind_value(name, session_.types_.builtin_generic("crew", {}),
             binding_origin::synthetic, span);
  const auto result = check_body_nodes(body, k_unknown_type);
  pop_scope();
  if (saved.has_value()) {
    session_.crews_[name] = *saved;
  } else {
    session_.crews_.erase(name);
  }
  return result;
}

auto checker::record_crew_spawn(const ast::expr &crew, type_id task,
                                source_span span) -> void {
  const auto *name = ast::as<const ast::ident_expr>(&crew);
  if (name == nullptr) {
    return;
  }
  const auto found = session_.crews_.find(name->name);
  if (found == session_.crews_.end() || !found->second.collects ||
      session_.types_.is_unknown(task)) {
    return;
  }
  if (found->second.task_type == k_unknown_type) {
    found->second.task_type = task;
  } else if (found->second.task_type != task) {
    error_with_help(
        span,
        std::format("`{}` collects errors, but this task is a `{}` and the "
                    "crew's earlier tasks are `{}`",
                    name->name, session_.types_.display(task),
                    session_.types_.display(found->second.task_type)),
        "different task type",
        "Every task in a crew that uses `on_error: collect` must return the "
        "same type, so `errors()` has one error type.");
  }
}

auto checker::check_task_body(source_span span,
                              const std::function<type_id()> &check)
    -> type_id {
  function_.task_boundaries_.push_back(
      {.scope_floor = function_.scopes_.size(), .captured = {}});
  const auto result = check();
  const auto captured = std::move(function_.task_boundaries_.back().captured);
  function_.task_boundaries_.pop_back();
  auto reported = std::unordered_set<std::string>{};
  for (const auto &[name, type] : captured) {
    if (reported.insert(name).second) {
      require_sendable(type, span,
                       std::format("`{}` is captured by this task", name));
    }
  }
  return result;
}

auto checker::require_task_args_sendable(const ast::expr &task) -> void {
  const auto *call = ast::as<const ast::call_expr>(&task);
  if (call == nullptr) {
    return;
  }
  for (const auto &arg : call->args) {
    if (arg.value == nullptr) {
      continue;
    }
    if (const auto it = session_.node_types_.find(arg.value.get());
        it != session_.node_types_.end()) {
      require_sendable(it->second, arg.value->span,
                       "this argument moves into the task");
    }
  }
}

auto checker::infer_crew_errors(const ast::call_expr &call,
                                const ast::expr &crew, source_span span)
    -> type_id {
  const auto *name = ast::as<const ast::ident_expr>(&crew);
  const auto found = name != nullptr ? session_.crews_.find(name->name)
                                     : session_.crews_.end();
  if (found == session_.crews_.end() || !found->second.collects) {
    error_with_help(span, "`errors()` needs a `crew` that collects errors",
                    "this crew does not collect errors",
                    "Declare it as `crew c(on_error: collect):`.");
    return k_error_type;
  }
  if (found->second.task_type == k_unknown_type) {
    error_with_help(span,
                    std::format("`{}.errors()` comes before any task is "
                                "spawned into `{}`",
                                name->name, name->name),
                    "no task spawned yet",
                    "Spawn the tasks first; `errors()` waits for them and "
                    "collects what they failed with.");
    return k_error_type;
  }
  const auto &task = session_.types_.entry(found->second.task_type);
  if (task.args.size() < 2 || session_.types_.entry(task.args[1]).name == "never") {
    error_with_help(span, "the tasks in this crew cannot fail",
                    "no error type",
                    "`errors()` collects the `err` side of a task that "
                    "returns `result[T, E]`.");
    return k_error_type;
  }
  const auto list_type = resolve_list_type(task.args[1]);
  record_new_push_dispatch(call, list_type, session_.crew_error_dispatches_);
  session_.crew_error_task_types_[&call] = found->second.task_type;
  return list_type;
}

/// What awaiting a value of type `operand` produces: for a `task[T, E, C]`,
/// `result[T, E]` (or plain `T` when the task cannot fail), after checking
/// that the awaiting code runs on the task's context. Any other operand type
/// passes through unchanged, since full task/context typing is not yet
/// modeled.
auto checker::awaited_type(type_id operand, source_span span) -> type_id {
  const auto &entry = session_.types_.entry(strip_refs(operand));
  if (entry.kind == type_kind::builtin_generic_kind && entry.name == "task") {
    if (entry.args.size() == 3 && function_.async_context_ != k_unknown_type &&
        !session_.types_.is_unknown(entry.args[2]) &&
        entry.args[2] != function_.async_context_) {
      error_with_help(
          span,
          std::format("this task needs the `{}` context, but the code "
                      "awaiting it runs on `{}`",
                      session_.types_.display(entry.args[2]),
                      session_.types_.display(function_.async_context_)),
          "awaited from the wrong context",
          "Wrap the `await` in `on(...)` to switch to the task's context.");
    }
    if (entry.args.size() >= 2 && !session_.types_.is_unknown(entry.args[1]) &&
        session_.types_.entry(entry.args[1]).name != "never") {
      return session_.types_.builtin_generic("result",
                                             {entry.args[0], entry.args[1]});
    }
    return entry.args.empty() ? k_unknown_type : entry.args[0];
  }
  return operand; // awaiting a non-task is validated once tasks are typed
}

/// Types `yield expr` — only legal inside a `generator def` body. Checks
/// the yielded value against the enclosing generator's item type
/// (`function_.generator_item_type_`), the same way a `return` statement checks
/// against `function_.return_type_`. A `yield` expression itself always has
/// type `unit` — it's a statement-shaped suspension point, not a
/// value-producing expression, even though it's grammatically an `expr`
/// so it can appear as an expression-statement without extra plumbing.
auto checker::infer_yield(const ast::yield_expr &expr) -> type_id {
  if (!function_.in_generator_) {
    error(expr.span, "`yield` outside a `generator def` body",
          "not inside a generator");
    if (expr.value != nullptr) {
      infer_expr(*expr.value, k_unknown_type);
    }
    return session_.types_.builtin("unit");
  }
  const auto found =
      expr.value != nullptr
          ? infer_expr(*expr.value, function_.generator_item_type_)
          : k_unknown_type;
  if (!session_.types_.is_unknown(function_.generator_item_type_)) {
    type_mismatch(expr.span, function_.generator_item_type_, found,
                  "as the yielded value");
  }
  return session_.types_.builtin("unit");
}

/// Returns the element type yielded by iterating `iterable` (array/list/
/// slice/range/option element, or `char` for `str`), used by `for`
/// loops/comprehensions. Reports a not-iterable error for a known
/// non-iterable scalar type.
/// Resolves a user type's `std.iter.iterator[T]` conformance for a `for`
/// loop: finds a `next(mut self) -> option[T]` method on `iterable` and,
/// when present, returns the dispatch a `for x in it: ...` loop needs to
/// desugar into `while let @some(x) = it.next(): ...` (`element_type` is the
/// `T`, the rest is what `hir::lower_iterator_loop` rebuilds the `it.next()`
/// call from). `nullopt` for any type that isn't shaped like an iterator —
/// including the builtin iterables (`list`/`slice`/`range`/...), which have
/// their own dedicated loop shapes and never reach this path.
/// `site`, when given, is the syntax whose iteration needs a *compiled*
/// `next` — the `for` statement. An adapter declared
/// `impl[I, T, U] iterator[U] for map_iter[I, T, U]` has no single compiled
/// `next`, exactly as a written `it.next()` call would not
/// (`check_impl_generic_method_call`); the difference is only that nothing
/// in the source names the call, so the instance has to be requested from
/// here or lowering finds no such function. Passing `nullptr` answers the
/// element type without compiling anything, which is what `element_type_of`
/// wants — it is asked about types in places that are not loops at all.
/// Resolves the *borrowing* iteration route for `for x in &e` / `for x in
/// &mut e`: a UFCS free function named `iter`/`iter_mut` (the
/// `xs: &list[T]` / `xs: &mut list[T]` shape `std.iter` already declares
/// for the builtin `list`) whose first parameter accepts `operand`'s type
/// by the matching reference kind. Tried only when the loop's whole
/// iterable is a bare `&`/`&mut` borrow, and tried *before*
/// `try_resolve_into_iterator` — that adapter's `into_iter(self)` always
/// consumes, which is exactly what a written `&`/`&mut` says not to do
/// (spec/todo.md #20).
///
/// `nullopt` when no visible `iter`/`iter_mut` accepts `operand`, or what
/// it returns is not itself iterable; the caller then falls through to
/// `try_resolve_into_iterator`/`try_resolve_iterator` over the whole
/// (still-referenced) iterable, exactly as before this route existed.
auto checker::try_resolve_iter_borrow(type_id operand, bool is_mut,
                                      const ast::node *site)
    -> std::optional<iterator_loop_dispatch> {
  const auto name = std::string(is_mut ? "iter_mut" : "iter");
  for (const auto &candidate : collect_ufcs_candidates(name)) {
    const auto params = signature_params(*candidate.decl, candidate.owner,
                                         /*skip_self=*/false);
    if (params.empty()) {
      continue;
    }
    const auto &param_entry = session_.types_.entry(params.front().type);
    if (param_entry.kind != type_kind::ref_kind ||
        param_entry.is_mut != is_mut) {
      continue;
    }
    const auto bare_param = strip_refs(params.front().type);
    // The candidate's own parameters (`container[T]`'s `T`) are unsolved.
    if (!session_.types_.compatible(open_foreign_params(bare_param), operand)) {
      continue;
    }

    auto bindings = param_subst{};
    match_params(bare_param, operand, bindings);
    const auto ret_type = substitute_solved(
        signature_return_type(*candidate.decl, candidate.owner), bindings);
    if (session_.types_.is_unknown(ret_type) || ret_type == k_error_type) {
      continue;
    }

    auto inner = try_resolve_iterator(ret_type, site);
    if (!inner.has_value()) {
      continue;
    }

    inner->adapter_decl = candidate.decl;
    inner->adapter_owner_module = candidate.owner->module_name;
    inner->adapter_impl_target_type = "";
    inner->adapter_result_type = ret_type;

    // A generic `iter[T]`/`iter_mut[T]` needs its own compiled instance for
    // the reason `try_resolve_into_iterator` documents: nothing in the
    // source names this call.
    if (site != nullptr && is_generic_template(*candidate.decl) &&
        !mentions_template_param(operand)) {
      auto solution = generic_solution{};
      solution.type_slots = bindings_by_name(bindings);
      solution.suffix = std::format("${}", mangle_type_for_instance(operand));
      const auto instance_name =
          std::format("{}{}", candidate.decl->name, solution.suffix);
      if (const auto *instance = find_or_check_generic_instance(
              *site, *candidate.decl, candidate.owner, candidate.file_id,
              solution, instance_name, &solution.type_slots)) {
        inner->adapter_decl = instance;
      }
    }
    return inner;
  }
  return std::nullopt;
}

/// Resolves a *collection*'s `std.iter.into_iterator[T]` conformance for a
/// `for` loop: finds `into_iter(self) -> I`, resolves `I`, and returns the
/// dispatch for iterating that `I` with the adapter call recorded on it.
///
/// Tried before `try_resolve_iterator`, so a type that is both a
/// collection and an iterator hands back its iterator rather than being
/// consumed — "give me your iterator" is the more specific answer.
///
/// `nullopt` when the type has no usable `into_iter`, or when what it
/// returns is not itself iterable; the caller then falls through to the
/// structural `next` path exactly as before.
auto checker::try_resolve_into_iterator(type_id iterable, const ast::node *site)
    -> std::optional<iterator_loop_dispatch> {
  const auto stripped = strip_refs(iterable);
  const auto &entry = session_.types_.entry(stripped);
  if (entry.decl == nullptr) {
    return std::nullopt;
  }
  const auto *method = find_method(entry, "into_iter", stripped);
  if (method == nullptr || method->decl->params.empty() ||
      param_name_of(method->decl->params.front()) != "self") {
    return std::nullopt;
  }

  // The iterator type, with the impl's parameters solved from the receiver
  // — the same two-step every other generic-impl return type needs.
  auto bindings = param_subst{};
  if (method->block_type_params != nullptr) {
    match_params(method->impl_target_pattern, stripped, bindings);
  }
  const auto iter_type =
      substitute_solved(signature_return_type(*method->decl, method->owner,
                                              method->block_type_params),
                        bindings);
  if (session_.types_.is_unknown(iter_type) || iter_type == k_error_type) {
    return std::nullopt;
  }

  // What `into_iter` hands back has to be iterable in its own right, or
  // there is no loop to build. Resolved with the same `site`, so the
  // iterator's `next` instance is compiled here too.
  auto inner = try_resolve_iterator(iter_type, site);
  if (!inner.has_value()) {
    return std::nullopt;
  }

  inner->adapter_decl = method->decl;
  inner->adapter_owner_module = method->owner->module_name;
  inner->adapter_impl_target_type = entry.name;
  inner->adapter_result_type = iter_type;

  // An `into_iter` on a generic collection needs its own compiled
  // instance, for the reason `check_impl_generic_method_call` documents:
  // nothing in the source names this call, so if the instance is not
  // requested here, lowering finds no such function.
  if (site != nullptr && impl_needs_instance(*method, entry) &&
      !mentions_template_param(stripped)) {
    auto scoped_params = method->fixed_type_params;
    scoped_params.merge(bindings_by_name(bindings));
    auto solution = generic_solution{};
    solution.suffix = std::format("${}", mangle_type_for_instance(stripped));
    const auto name =
        method_instance_name(entry.name, method->decl->name, solution.suffix);
    if (const auto *instance = find_or_check_generic_instance(
            *site, *method->decl, method->owner, method->file_id, solution,
            name, &scoped_params, stripped)) {
      // The instance's name already carries the receiver, so it is a plain
      // function name rather than a `Type::method` pair to be joined.
      inner->adapter_decl = instance;
      inner->adapter_impl_target_type = "";
    }
  }
  return inner;
}

auto checker::try_resolve_iterator(type_id iterable, const ast::node *site)
    -> std::optional<iterator_loop_dispatch> {
  const auto stripped = strip_refs(iterable);
  const auto &entry = session_.types_.entry(stripped);
  if (entry.decl == nullptr) {
    return std::nullopt;
  }
  const auto *method = find_method(entry, "next");
  if (method == nullptr || method->decl->params.empty() ||
      param_name_of(method->decl->params.front()) != "self") {
    return std::nullopt;
  }

  // Solved unconditionally, not just when an instance is wanted: an impl
  // parameter reaching the *element* type (`-> option[U]`) leaves the
  // return type written in `U` until the receiver answers it, and an
  // unsubstituted `U` is what `hir::lower_iterator_loop` rejects as "the
  // iterator's item type did not resolve to a concrete type".
  auto bindings = param_subst{};
  if (method->block_type_params != nullptr) {
    match_params(method->impl_target_pattern, stripped, bindings);
  }
  const auto ret =
      substitute_solved(signature_return_type(*method->decl, method->owner,
                                              method->block_type_params),
                        bindings);
  const auto &ret_entry = session_.types_.entry(strip_refs(ret));
  if (ret_entry.kind != type_kind::builtin_generic_kind ||
      ret_entry.name != "option" || ret_entry.args.empty()) {
    return std::nullopt;
  }

  auto dispatch =
      iterator_loop_dispatch{.decl = method->decl,
                             .owner_module = method->owner->module_name,
                             .impl_target_type = entry.name,
                             .element_type = ret_entry.args.front()};

  if (site == nullptr || !impl_needs_instance(*method, entry) ||
      mentions_template_param(stripped)) {
    return dispatch;
  }

  // Keyed on the receiver, and named after it, for the reason
  // `check_impl_generic_method_call` documents: two impls on two
  // instantiations of one target both claim `next`, and only the
  // receiver's arguments tell them apart.
  auto scoped_params = method->fixed_type_params;
  scoped_params.merge(bindings_by_name(bindings));
  auto solution = generic_solution{};
  solution.suffix = std::format("${}", mangle_type_for_instance(stripped));
  const auto name =
      method_instance_name(entry.name, method->decl->name, solution.suffix);
  const auto *instance = find_or_check_generic_instance(
      *site, *method->decl, method->owner, method->file_id, solution, name,
      &scoped_params, stripped);
  if (instance == nullptr) {
    return dispatch;
  }
  // The instance's name already carries the receiver, so it is a plain
  // function name rather than a `Type::method` pair to be joined later.
  dispatch.decl = instance;
  dispatch.impl_target_type = "";
  return dispatch;
}

auto checker::element_type_of(type_id iterable, source_span span) -> type_id {
  const auto stripped = strip_refs(iterable);
  // `for i in a..b` yields the bound type. Checked before the kind
  // switch because `range[T]` is an ordinary struct now, and the
  // `struct_kind` arm below would otherwise send it looking for an
  // `into_iterator` impl it does not have.
  if (const auto bound = range_element_type(stripped)) {
    return *bound;
  }
  const auto &entry = session_.types_.entry(stripped);
  switch (entry.kind) {
  case type_kind::array_kind:
    return entry.result;
  case type_kind::builtin_generic_kind:
    if (entry.name == "slice" || entry.name == "slice_mut" ||
        entry.name == "option" || entry.name == "generator") {
      return entry.args.empty() ? k_unknown_type : entry.args[0];
    }
    return k_unknown_type;
  case type_kind::builtin_kind:
    if (entry.name == "str") {
      return session_.types_.builtin("char");
    }
    if (session_.types_.is_numeric(stripped) ||
        session_.types_.is_boolean(stripped)) {
      error_with_help(span,
                      std::format("cannot iterate over a value of type `{}`",
                                  session_.types_.display(stripped)),
                      "not an iterable value",
                      "Iterate over a collection or a range such as `0..10`.");
      return k_error_type;
    }
    return k_unknown_type;
  default:
    // A user type that implements `std.iter.iterator[T]` iterates to `T`
    // (via `it.next() -> option[T]`); anything else has no element type.
    if (const auto iter = try_resolve_iterator(stripped)) {
      return iter->element_type;
    }
    return k_unknown_type;
  }
}

// ==========================================================================
//  Expression dispatch
// ==========================================================================

/// Records `type` as `node`'s resolved type in `session_.node_types_` and
/// returns it unchanged, so a call site can wrap its result in one expression.
auto checker::record_expr_type(const ast::node &node, type_id type) -> type_id {
  session_.node_types_[&node] = type;
  // Where this node was checked, for `semantic::render_snapshot` — see
  // `checked_types::node_files`. `file_.file_id_` is the file the checker is
  // standing in, which for a per-call method instance is the method's own
  // file rather than the caller's (`method_entry::file_id`), and that is
  // the answer a reader of the snapshot wants.
  session_.node_files_[&node] = file_.file_id_;
  return type;
}

/// The central expression-typing dispatcher: infers `expr`'s type given an
/// `expected` type (used to disambiguate literals, propagate generic
/// arguments into a lambda, and check tail-expression compatibility),
/// dispatching to the dedicated `infer_*`/`check_*` function for its
/// concrete node kind. Thin wrapper around `infer_expr_impl` that records
/// every visited expression's resolved type into `session_.node_types_`, so
/// this is the one place that needs to — every `infer_*` helper is reached only
/// through here (recursively, for nested expressions too).
auto checker::infer_expr(const ast::expr &expr, type_id expected) -> type_id {
  // A deferred call being re-run (`defer_open_receiver_call`): its
  // receiver and arguments were checked where they were written, in
  // scopes that are gone now, so they answer with what they were found
  // to be then.
  if (const auto replayed = replayed_exprs_.find(&expr);
      replayed != replayed_exprs_.end()) {
    if (!session_.types_.is_unknown(expected)) {
      solve_leaves(session_.types_.strip_refinement(expected),
                   replayed->second);
    }
    return record_expr_type(expr, settle(replayed->second));
  }
  // A refined `expected` is an *obligation*, not a shape hint. Inference
  // gets the base type, so an expression only ever comes back refined by
  // actually being one (a `positive` parameter read back, a `try_from`
  // unwrapped) — never by having been asked for one. Without this,
  // `needs_positive(0 - 3)` would infer its own argument *as* a `positive`
  // and the narrowing check would find nothing left to prove.
  if (!cache_.static_for_yields_.empty()) {
    note_static_for_loop_read(expr);
  }
  return record_expr_type(
      expr, infer_expr_impl(expr, session_.types_.strip_refinement(expected)));
}

/// Records `expr` against every inline `static for` being checked whose
/// loop binds the name it is rooted at (`static_for_reads::loop_reads`).
auto checker::note_static_for_loop_read(const ast::expr &expr) -> void {
  const auto *root = static_for_read_root(expr);
  if (root == nullptr) {
    return;
  }
  const auto found = find_value_in_scopes(*root);
  if (found.binding == nullptr) {
    return;
  }
  for (const auto &[decl, floor] : cache_.static_for_yields_) {
    if (found.depth == floor) {
      cache_.static_for_reads_[decl].loop_reads.push_back(&expr);
    }
  }
}

/// The name `expr` reads through, when it is a name or a chain of field
/// reads on one (`f`, `f.name`, `p.x.y`); `nullptr` otherwise.
auto checker::static_for_read_root(const ast::expr &expr)
    -> const std::string * {
  const auto *current = &expr;
  while (current->kind == ast::node_kind::field_expr) {
    const auto &field = ast::cast<const ast::field_expr>(*current);
    if (field.object == nullptr) {
      return nullptr;
    }
    current = field.object.get();
  }
  if (current->kind == ast::node_kind::ident_expr) {
    return &ast::cast<const ast::ident_expr>(*current).name;
  }
  if (current->kind == ast::node_kind::module_path_expr) {
    const auto &path = ast::cast<const ast::module_path_expr>(*current);
    return path.segments.empty() ? nullptr : &path.segments.front();
  }
  return nullptr;
}

auto checker::infer_expr_impl(const ast::expr &expr, type_id expected)
    -> type_id {
  if (expr.has_error) {
    return k_unknown_type;
  }

  switch (expr.kind) {
  case ast::node_kind::ident_expr:
    return resolve_ident(ast::cast<const ast::ident_expr>(expr), expected);
  case ast::node_kind::literal_expr:
    return infer_literal(ast::cast<const ast::literal_expr>(expr), expected);
  case ast::node_kind::binary_expr:
    return infer_binary(ast::cast<const ast::binary_expr>(expr), expected);
  case ast::node_kind::unary_expr:
    return infer_unary(ast::cast<const ast::unary_expr>(expr), expected);
  case ast::node_kind::call_expr:
    return infer_call(ast::cast<const ast::call_expr>(expr), expected);
  case ast::node_kind::index_expr:
    return infer_index(ast::cast<const ast::index_expr>(expr));
  case ast::node_kind::field_expr:
    return infer_field(ast::cast<const ast::field_expr>(expr));
  case ast::node_kind::cast_expr: {
    const auto &cast = ast::cast<const ast::cast_expr>(expr);
    const auto target =
        cast.target_type != nullptr
            ? resolve_type(*cast.target_type, current_resolve_ctx())
            : k_unknown_type;
    if (cast.operand != nullptr) {
      // A literal operand adopts the cast's own target as its expected
      // type (e.g. `300000000000 as int64` should size the literal
      // against `int64`, not silently default it to `int32` first and
      // reject it before the cast ever runs). Non-literal operands keep
      // `k_unknown_type`: the cast is an explicit conversion, not a
      // narrowing obligation on an already-typed expression.
      const auto *operand_lit =
          ast::as<const ast::literal_expr>(cast.operand.get());
      const auto operand = strip_refs(infer_expr(
          *cast.operand, operand_lit != nullptr ? target : k_unknown_type));
      if (is_rigid_param(operand) || is_rigid_param(target)) {
        check_cast_with_param(cast, operand, target);
      }
    }
    return target;
  }
  case ast::node_kind::try_expr:
    return infer_try(ast::cast<const ast::try_expr>(expr));
  case ast::node_kind::tuple_expr: {
    const auto &tuple = ast::cast<const ast::tuple_expr>(expr);
    const auto &expected_entry = session_.types_.entry(strip_refs(expected));
    auto elements = std::vector<type_id>{};
    elements.reserve(tuple.elements.size());
    for (size_t i = 0; i < tuple.elements.size(); ++i) {
      const auto element_expected =
          expected_entry.kind == type_kind::tuple_kind &&
                  i < expected_entry.args.size()
              ? expected_entry.args[i]
              : k_unknown_type;
      elements.push_back(tuple.elements[i] != nullptr
                             ? infer_expr(*tuple.elements[i], element_expected)
                             : k_unknown_type);
    }
    return session_.types_.tuple_of(std::move(elements));
  }
  case ast::node_kind::array_expr:
    return infer_array(ast::cast<const ast::array_expr>(expr), expected);
  case ast::node_kind::struct_expr:
    return check_struct_literal(ast::cast<const ast::struct_expr>(expr),
                                expected);
  case ast::node_kind::lambda_expr:
    return infer_lambda(ast::cast<const ast::lambda_expr>(expr), expected);
  case ast::node_kind::match_expr: {
    const auto &match = ast::cast<const ast::match_expr>(expr);
    return check_match(match.subject.get(), match.arms, expected, match.span);
  }
  case ast::node_kind::if_expr:
    return infer_if_expr(ast::cast<const ast::if_expr>(expr), expected);
  case ast::node_kind::for_expr:
    return infer_for_expr(ast::cast<const ast::for_expr>(expr));
  case ast::node_kind::await_expr:
    return infer_await(ast::cast<const ast::await_expr>(expr));
  case ast::node_kind::yield_expr:
    return infer_yield(ast::cast<const ast::yield_expr>(expr));
  case ast::node_kind::async_expr: {
    const auto saved_async = std::exchange(function_.in_async_, true);
    check_task_body(expr.span, [&] -> type_id {
      return check_body_nodes(ast::cast<const ast::async_expr>(expr).body,
                              k_unknown_type);
    });
    function_.in_async_ = saved_async;
    return k_unknown_type;
  }
  case ast::node_kind::par_expr: {
    const auto &par = ast::cast<const ast::par_expr>(expr);
    auto elements = std::vector<type_id>{};
    for (const auto &branch : par.branches) {
      elements.push_back(branch != nullptr ? infer_expr(*branch, k_unknown_type)
                                           : k_unknown_type);
      if (branch != nullptr) {
        require_task_args_sendable(*branch);
      }
    }
    return session_.types_.tuple_of(std::move(elements));
  }
  case ast::node_kind::race_expr: {
    const auto &race = ast::cast<const ast::race_expr>(expr);
    auto result = k_unknown_type;
    for (const auto &branch : race.branches) {
      if (branch != nullptr) {
        result = infer_expr(*branch, k_unknown_type);
        require_task_args_sendable(*branch);
      }
    }
    return result;
  }
  case ast::node_kind::crew_expr: {
    const auto &crew = ast::cast<const ast::crew_expr>(expr);
    return check_crew(crew.name, crew.options, crew.body, crew.span);
  }
  case ast::node_kind::on_expr: {
    const auto &on = ast::cast<const ast::on_expr>(expr);
    if (on.sender != nullptr) {
      infer_expr(*on.sender, k_unknown_type);
    }
    const auto saved_context = function_.async_context_;
    function_.async_context_ = context_of_on_target(on);
    // A single-threaded context moves nothing between threads.
    const auto result =
        function_.async_context_ == session_.types_.builtin("io")
            ? check_body_nodes(on.body, expected)
            : check_task_body(on.span, [&] -> type_id {
                return check_body_nodes(on.body, expected);
              });
    function_.async_context_ = saved_context;
    return result;
  }
  case ast::node_kind::block_expr:
    return check_body_nodes(ast::cast<const ast::block_expr>(expr).stmts,
                            expected);
  case ast::node_kind::quote_expr: {
    const auto &quote = ast::cast<const ast::quote_expr>(expr);
    // The splices evaluating the quote resolves are code that runs then,
    // here, so they are checked here. A bare type parameter spliced into a
    // type position (`~T`) names a type; it is not a value to check.
    for (const auto *splice :
         comptime::fragment_builder::eager_splices(quote)) {
      const auto *operand =
          splice->kind == ast::node_kind::splice_expr
              ? ast::cast<const ast::splice_expr>(*splice).operand.get()
              : ast::cast<const ast::splice_type>(*splice).operand.get();
      if (operand == nullptr) {
        continue;
      }
      if (const auto *ident = ast::as<const ast::ident_expr>(operand);
          ident != nullptr && splice->kind == ast::node_kind::splice_type &&
          lookup_type_param(ident->name).has_value()) {
        continue;
      }
      infer_expr(*operand, k_unknown_type);
    }
    switch (quote.fragment_kind) {
    case ast::quote_fragment_kind::stmt:
      return session_.types_.builtin("stmt");
    case ast::quote_fragment_kind::def_expr:
      return session_.types_.builtin("def_expr");
    case ast::quote_fragment_kind::type_expr:
      return session_.types_.builtin("type_expr");
    case ast::quote_fragment_kind::expr:
      // A bare name or dotted path (`` `(int32)` ``, `` `(std.foo.bar)` ``)
      // is lexically ambiguous between an expression and a type — the
      // parser classifies it as `expr` since it has no annotation context
      // (see `parser::classify_and_parse_quote_fragment`'s doc comment).
      // When the surrounding context expects `type_expr` and the content
      // reinterprets as a named type, honor that instead of reporting a
      // spurious mismatch — this is the "deferred to the semantic layer"
      // disambiguation the parser punted on.
      if (!session_.types_.is_unknown(expected) &&
          session_.types_.entry(expected).kind == type_kind::builtin_kind &&
          session_.types_.entry(expected).name == "type_expr" &&
          quote.parsed_body != nullptr &&
          reinterpret_as_named_type(*quote.parsed_body) != nullptr) {
        return session_.types_.builtin("type_expr");
      }
      return session_.types_.builtin("expr");
    case ast::quote_fragment_kind::none:
      // `none` means the parser couldn't classify the quoted content
      // (already diagnosed at parse time, or genuinely empty) — fall back
      // to `expr` rather than `unknown` so a downstream annotation
      // mismatch still gets a concrete, if possibly wrong, type to report
      // against instead of silently unifying with everything.
      return session_.types_.builtin("expr");
    }
    return session_.types_.builtin("expr");
  }
  case ast::node_kind::splice_expr: {
    const auto &splice = ast::cast<const ast::splice_expr>(expr);
    if (splice.operand == nullptr) {
      return k_unknown_type;
    }
    const auto operand_type = infer_expr(*splice.operand, k_unknown_type);
    require_quote_value(operand_type, splice.operand->span,
                        "a spliced expression");
    // Reification: evaluate the operand at compile time and, if it
    // resolves to a quoted expression, graft that fragment in as this
    // splice's real content — type-checking it here (so `hir::lower` has
    // a `session_.node_types_` entry to look up) and recording the splice ->
    // fragment mapping (`session_.spliced_fragments_`) so lowering can find it.
    // No hygiene pass yet (M5): the fragment is checked directly against
    // whatever scope is active at the splice site.
    const auto fragment_value = comptime_eval_.evaluate(*splice.operand);
    if (fragment_value.is_error()) {
      return k_error_type;
    }
    if (fragment_value.kind != comptime::value_kind::expr_fragment ||
        fragment_value.fragment == nullptr) {
      error_with_help(
          splice.operand->span,
          std::format("a spliced expression must resolve to a quoted "
                      "expression, found {}",
                      quote_fragment_kind_name(fragment_value.kind)),
          "this splice is used in expression position",
          "Only a value quoted as `` `(...)` `` with expression content "
          "can be spliced here; a quoted statement or definition belongs "
          "in statement or item position instead.");
      return k_error_type;
    }
    const auto *fragment_expr =
        ast::as<const ast::expr>(fragment_value.fragment);
    if (fragment_expr == nullptr) {
      return k_error_type;
    }
    const auto fragment_type = infer_expr(*fragment_expr, expected);
    session_.spliced_fragments_[&expr] = fragment_expr;
    return fragment_type;
  }
  case ast::node_kind::static_expr: {
    const auto &inner = ast::cast<const ast::static_expr>(expr);
    return inner.operand != nullptr ? infer_expr(*inner.operand, expected)
                                    : k_unknown_type;
  }
  case ast::node_kind::module_path_expr:
    return infer_module_path(ast::cast<const ast::module_path_expr>(expr));
  case ast::node_kind::group_expr: {
    const auto &group = ast::cast<const ast::group_expr>(expr);
    return group.inner != nullptr ? infer_expr(*group.inner, expected)
                                  : k_unknown_type;
  }
  case ast::node_kind::interpolated_string_expr:
    return check_interpolated_string(
        ast::cast<const ast::interpolated_string_expr>(expr));
  case ast::node_kind::where_expr: {
    const auto &where = ast::cast<const ast::where_expr>(expr);
    push_scope();
    for (const auto &binding : where.bindings) {
      auto type = k_unknown_type;
      if (binding.value != nullptr) {
        type = infer_expr(*binding.value, k_unknown_type);
      }
      bind_value(binding.name, type, binding_origin::let_binding, binding.span);
    }
    const auto result = where.inner != nullptr
                            ? infer_expr(*where.inner, expected)
                            : k_unknown_type;
    pop_scope();
    return result;
  }
  default:
    return k_unknown_type;
  }
}

/// Types an interpolated string literal (`"{expr :spec}"`,
/// `spec/string-formatting-design.md`): infers each embedded expression's
/// type, resolves the format spec's type char to a required capability
/// (a builtin primitive or a trait), checks the `.precision`-on-integer-
/// style and dynamic-width/precision-must-be-`usize` rules, and records
/// how `hir::lower` should render each segment in
/// `session_.interp_dispatches_`. The whole node always types as `str`.
/// Resolves one interpolation segment's format dispatch once its value
/// type is a concrete, settled type — either immediately (the common
/// case) or later, once a deferred call-result leaf (see
/// `pending_interp_segments_`) has been pinned by `finish_open_results`.
auto checker::finish_interp_segment(const ast::interp_segment &seg,
                                    type_id value_type,
                                    const ast::expr *value_key) -> void {
  if (value_key == nullptr) {
    value_key = seg.value.get();
    // How a `T` formats is each instance's choice — the builtin way for
    // `int32`, `show` for a user type.
    if (mentions_template_param(value_type)) {
      defer_to_instances(
          *seg.value, [this, &seg, value_type](instance_subst &subst) -> void {
            const auto concrete = substitute_type(value_type, subst);
            if (!mentions_type_param(concrete)) {
              finish_interp_segment(seg, concrete,
                                    clone_of(subst, seg.value.get()));
            }
          });
    }
  }
  const char type_char = (seg.has_spec && seg.spec.type_char.has_value())
                             ? *seg.spec.type_char
                             : '\0';

  const bool has_precision =
      seg.has_spec &&
      !std::holds_alternative<std::monostate>(seg.spec.precision);
  const bool is_integer_style = type_char == 'd' || type_char == 'x' ||
                                type_char == 'X' || type_char == 'o' ||
                                type_char == 'b';
  if (has_precision && is_integer_style) {
    error_with_help(
        seg.spec.span,
        std::format("`.precision` is not allowed with `{}`", type_char),
        "precision is only meaningful for `s`, `?`, `f`, `e`/`E`, `g`/`G`",
        "for integer styles, use width and zero-padding instead, e.g. "
        "\"{value :04x}\"");
  }

  const auto &value_entry = session_.types_.entry(value_type);
  const bool is_builtin_int = session_.types_.is_integer(value_type);
  const bool is_builtin_float = session_.types_.is_float(value_type);
  const bool is_builtin_str =
      value_entry.kind == type_kind::builtin_kind && value_entry.name == "str";
  const bool is_builtin_bool = session_.types_.is_boolean(value_type);
  const bool is_builtin_char =
      value_entry.kind == type_kind::builtin_kind && value_entry.name == "char";
  const bool is_any_builtin = is_builtin_int || is_builtin_float ||
                              is_builtin_str || is_builtin_bool ||
                              is_builtin_char;

  auto dispatch = interp_dispatch{};
  dispatch.type_char = type_char;
  dispatch.value_type = value_type;

  const auto try_trait_dispatch =
      [&](std::string_view trait_name,
          interp_dispatch::kind_t builtin_kind_fallback,
          bool builtin_allowed) -> bool {
    if (builtin_allowed) {
      dispatch.kind = builtin_kind_fallback;
      return true;
    }
    // A value whose type is the body's own parameter formats as its bounds
    // allow (phase 9): through the trait if `T` has it, or the builtin way
    // if every type `T` can be here is a builtin that formats so.
    if (is_rigid_param(value_type)) {
      const auto facts = facts_of(value_type);
      if (std::ranges::any_of(facts, [&](const type_fact &fact) -> bool {
            return fact.trait->name == trait_name;
          })) {
        dispatch.kind = interp_dispatch::kind_t::trait_method;
        return true;
      }
      const auto radix =
          builtin_kind_fallback == interp_dispatch::kind_t::builtin_radix;
      if (domain_all(value_type, [&](type_id m) -> bool {
            return radix ? session_.types_.is_integer(m)
                         : session_.types_.entry(m).kind ==
                               type_kind::builtin_kind;
          })) {
        dispatch.kind = builtin_kind_fallback;
        return true;
      }
      return false;
    }
    if (!type_has_trait(value_entry, trait_name)) {
      return false;
    }
    dispatch.kind = interp_dispatch::kind_t::trait_method;
    dispatch.impl_target_type = value_entry.name;
    // `value_type` is passed, not left to default: it is what lets a
    // lookup on a concrete instantiation of a generic `deriving` type
    // derive that instantiation's method (`ensure_derived_instance_
    // impls`), and what lets `find_method` tell `wrap[int32]`'s impl
    // from `wrap[str]`'s.
    if (const auto *method = find_method(value_entry, trait_name, value_type);
        method != nullptr) {
      dispatch.decl = method->decl;
      dispatch.owner_module = method->owner->module_name;
      // A method reached through an impl over a generic target only
      // exists once compiled for this receiver. Record the instance, and
      // clear `impl_target_type` so `hir::lower` uses the instance's own
      // mangled name rather than composing `wrap::show`, which is the
      // template and is never emitted.
      if (const auto *instance = instantiate_impl_method_for(
              *seg.value, *method, value_entry, value_type)) {
        dispatch.decl = instance;
        dispatch.impl_target_type.clear();
      }
    }
    return true;
  };

  // What `value_type` actually supports — used only for the diagnostic
  // below when the requested style isn't one of them, so the message
  // never has to guess: builtins get a fixed description per category,
  // a user type gets a list of the capability traits it really
  // implements (empty if none, meaning only `impl show for T` etc. can
  // fix this).
  auto describe_supported = [&]() -> std::string {
    if (is_builtin_int) {
      return "the default/`s` style (`show`), `?` (`debug`), `d`, "
             "`x`/`X`, `o`, `b`, and `c`";
    }
    if (is_builtin_float) {
      return "the default/`s` style (`show`), `?` (`debug`), `e`/`E`, "
             "`f`, and `g`/`G`";
    }
    if (is_builtin_char) {
      return "the default/`s` style (`show`), `?` (`debug`), and `c`";
    }
    if (is_builtin_str || is_builtin_bool) {
      return "the default/`s` style (`show`) and `?` (`debug`)";
    }
    auto styles = std::vector<std::string>{};
    for (const auto &[trait_name, label] :
         {std::pair{"show", "the default/`s` style (`show`)"},
          std::pair{"debug", "`?` (`debug`)"},
          std::pair{"hex", "`x`/`X` (`hex`)"},
          std::pair{"octal", "`o` (`octal`)"},
          std::pair{"binary", "`b` (`binary`)"}}) {
      if (type_has_trait(value_entry, trait_name)) {
        styles.emplace_back(label);
      }
    }
    if (styles.empty()) {
      return "no format styles yet";
    }
    auto out = std::string{};
    for (size_t i = 0; i < styles.size(); ++i) {
      if (i > 0) {
        out += ", ";
      }
      out += styles[i];
    }
    return out;
  };

  auto ok = false;
  std::string trait_hint;
  switch (type_char) {
  case '\0':
  case 's':
    ok = try_trait_dispatch("show", interp_dispatch::kind_t::builtin_show,
                            is_any_builtin);
    trait_hint = "show";
    break;
  case '?':
    ok = try_trait_dispatch("debug", interp_dispatch::kind_t::builtin_debug,
                            is_any_builtin);
    trait_hint = "debug";
    break;
  case 'x':
  case 'X':
    ok = try_trait_dispatch("hex", interp_dispatch::kind_t::builtin_radix,
                            is_builtin_int);
    trait_hint = "hex";
    break;
  case 'o':
    ok = try_trait_dispatch("octal", interp_dispatch::kind_t::builtin_radix,
                            is_builtin_int);
    trait_hint = "octal";
    break;
  case 'b':
    ok = try_trait_dispatch("binary", interp_dispatch::kind_t::builtin_radix,
                            is_builtin_int);
    trait_hint = "binary";
    break;
  case 'd':
    ok = is_builtin_int;
    dispatch.kind = interp_dispatch::kind_t::builtin_radix;
    break;
  case 'e':
  case 'E':
  case 'f':
  case 'g':
  case 'G':
    ok = is_builtin_float;
    dispatch.kind = interp_dispatch::kind_t::builtin_float;
    break;
  case 'c':
    ok = is_builtin_int;
    dispatch.kind = interp_dispatch::kind_t::builtin_char;
    break;
  default:
    break;
  }

  if (!ok) {
    error_with_help(
        seg.value->span,
        std::format("`{}` does not support {} formatting",
                    session_.types_.display(value_type),
                    type_char == '\0' ? "the requested"
                                      : std::string(1, type_char)),
        std::format("`{}` does not implement a required trait",
                    session_.types_.display(value_type)),
        std::format("`{}` supports {} — implement `{} for {}` yourself "
                    "if you want this style to mean something specific",
                    session_.types_.display(value_type), describe_supported(),
                    trait_hint.empty() ? "show" : trait_hint,
                    session_.types_.display(value_type)));
    return;
  }

  session_.interp_dispatches_[value_key] = dispatch;
}

/// Retries dispatch for every segment `check_interpolated_string` deferred.
/// Must run after `finish_open_results` has had a chance to pin each
/// segment's leaf — called right after it, both places `flush_deferred`
/// calls it. A leaf still open here (nothing ever solved it) is left
/// queued rather than dropped, so a later `flush_deferred` pass — e.g. the
/// whole-run one after every file's own — still gets a chance at it.
auto checker::finish_pending_interp_segments() -> void {
  auto pending = std::vector<pending_interp_segment>{};
  pending.swap(pending_interp_segments_);
  for (const auto &item : pending) {
    const auto value_type = strip_refs(settle(item.leaf));
    if (mentions_type_var(value_type)) {
      pending_interp_segments_.push_back(item);
      continue;
    }
    if (session_.types_.is_unknown(value_type)) {
      continue;
    }
    finish_interp_segment(*item.seg, value_type);
  }
}

/// Rejects a `"{x}"` segment whose value is a reference type
/// (spec/todo.md item 28): formatting `&T` would print the reference's
/// address, not the value it points to, which is never what a user wants
/// out of string interpolation. Suggests `*x` instead.
auto checker::reject_interpolated_reference(const ast::expr &value,
                                            type_id ref_type) -> void {
  auto diag = diagnostic(diagnostic_level::error,
                         std::format("cannot interpolate a reference (`{}`)",
                                     session_.types_.display(ref_type)),
                         file_.file_id_);
  diag.with_label(value.span,
                  "this is a reference, not the value it points to");
  diag.with_help(
      "interpolating a reference would print its address, not the value "
      "it lends — dereference it first with `*`.");
  diag.with_fix("dereference with `*`",
                source_span{.start = value.span.start, .end = value.span.start},
                "*");
  emit_diag(diag);
  mark_error();
}

auto checker::check_interpolated_string(
    const ast::interpolated_string_expr &node) -> type_id {
  const auto usize_type = session_.types_.builtin("usize");

  const auto check_dynamic_size =
      [&](const std::variant<std::monostate, size_t, ast::ptr<ast::expr>> &slot)
      -> void {
    const auto *dynamic_expr = std::get_if<ast::ptr<ast::expr>>(&slot);
    if (dynamic_expr == nullptr || *dynamic_expr == nullptr) {
      return;
    }
    const auto found = infer_expr(**dynamic_expr, usize_type);
    if (!session_.types_.is_unknown(found) &&
        !session_.types_.compatible(usize_type, found)) {
      error((*dynamic_expr)->span,
            std::format("format width must be `usize`, found `{}`",
                        session_.types_.display(found)),
            "expected usize here");
    }
  };

  for (const auto &seg : node.segments) {
    if (seg.is_literal || seg.value == nullptr) {
      continue;
    }
    // Not a point of demand. The capability check needs a type, but it
    // can wait for one: an open segment is queued below and retried once
    // the leaves settle, so `println("{a}")` followed by
    // `let c: int64 = a` formats an `int64` (phase 11, defect 2).
    const auto settled_type = settle(infer_expr(*seg.value, k_unknown_type));

    if (session_.types_.entry(settled_type).kind == type_kind::ref_kind) {
      // A reference's runtime representation is an address; interpolating
      // it directly would format that address instead of the value it
      // points to (spec/todo.md item 28). Reject rather than silently
      // stripping the reference and formatting the referent's type as if
      // it were the value in hand.
      reject_interpolated_reference(*seg.value, settled_type);
      continue;
    }

    const auto value_type = strip_refs(settled_type);

    if (seg.has_spec) {
      check_dynamic_size(seg.spec.width);
      check_dynamic_size(seg.spec.precision);
    }

    if (mentions_type_var(value_type)) {
      // Real, but not solvable yet — e.g. an implicit/explicit generic
      // whose instance body hasn't been checked. `finish_open_results`
      // (inside `flush_deferred`) pins leaves like this later; come back
      // then instead of reporting "unknown" against a type that is really
      // just not ready.
      pending_interp_segments_.push_back(
          pending_interp_segment{.seg = &seg, .leaf = value_type});
      continue;
    }

    if (session_.types_.is_unknown(value_type)) {
      continue;
    }

    finish_interp_segment(seg, value_type);
  }

  return session_.types_.builtin("str");
}

/// Types an array literal, either the explicit-list form `[a, b, c]` or
/// the fill form `[val; count]`. The expected type (an `array[T, n]` or a
/// `list[T]`/`slice[T]`) determines the element type and result container;
/// with no useful expectation, an explicit list defaults to `list[T]` with
/// `T` inferred from its first element, checking the rest against it.
/// The constant length of a `[value; n]` fill literal, when `n` is one.
/// `nullopt` inside a template, where `n` is still a symbol — the same
/// answer `infer_array`'s own fill branch arrives at, and for the same
/// reason.
auto checker::fill_count_of(const ast::array_expr &array)
    -> std::optional<uint64_t> {
  if (array.fill_count == nullptr) {
    return std::nullopt;
  }
  if (array.fill_count->kind == ast::node_kind::literal_expr) {
    const auto &lit = ast::cast<const ast::literal_expr>(*array.fill_count);
    if (lit.lit_kind == token_kind::int_lit) {
      return parse_integer_literal(lit.value);
    }
  }
  if (const auto folded = explicit_value_argument(*array.fill_count);
      folded.has_value() && *folded >= 0) {
    return static_cast<uint64_t>(*folded);
  }
  return std::nullopt;
}

/// Compiles the `from_array` instance this literal needs and points the
/// recorded conversion at it.
///
/// Nothing in the source names this call — the literal is the whole
/// syntax — so if the instance is not requested here, lowering finds only
/// the uncompiled template and both backends report "could not be resolved
/// to a function in this compiled module". The same reason
/// `try_resolve_iterator` requests its `next` instance from the loop.
auto checker::instantiate_from_array_for(const ast::array_expr &array,
                                         type_id target, uint64_t length)
    -> void {
  // A target still holding a leaf — the second `[2]` in `[[1], [2]]`, whose
  // expected type is the first element's `list[?a]` — would name an
  // instance of `list[?a]`, compiled with no concrete element type. Wait
  // for the leaf, the same way an open `new`/`push` dispatch does.
  if (mentions_type_var(target)) {
    defer_method_call("from_array", target, std::vector<type_id>{},
                      [this, &array, length](type_id settled) -> void {
                        instantiate_from_array_for(array, settled, length);
                      });
    return;
  }
  if (mentions_template_param(target)) {
    defer_to_instances(
        array, [this, &array, target, length](instance_subst &subst) -> void {
          instantiate_from_array_for(*clone_of(subst, &array),
                                     substitute_type(target, subst), length);
        });
    return;
  }
  const auto conversion = session_.array_literal_conversions_.find(&array);
  if (conversion == session_.array_literal_conversions_.end()) {
    return;
  }
  const auto &entry = session_.types_.entry(target);
  const auto *method = find_method(entry, "from_array", target);
  if (method == nullptr || method->decl->type_params.empty()) {
    return;
  }
  const auto &length_param = method->decl->type_params.front();
  if (!length_param.is_value_param) {
    return;
  }

  auto solution = generic_solution{};
  bind_generic_constant(solution, length_param, session_.types_.usize_type(),
                        static_cast<int64_t>(length));
  auto bindings = param_subst{};
  if (method->block_type_params != nullptr) {
    match_params(method->impl_target_pattern, target, bindings);
    solve_impl_value_params(*method, target, bindings);
  }
  auto scoped_params = method->fixed_type_params;
  scoped_params.merge(bindings_by_name(bindings));
  carry_impl_value_slots(*method, bindings, solution);
  // The receiver goes in the name as well as the length. Without it
  // `vector[int32]` and `vector[int64]` both want `vector::from_array$3`
  // and share one instance — whichever was compiled first, read at the
  // other's element width. That is a silent wrong-value bug, not a link
  // failure, which is why this name is the same shape every other
  // impl-method instance uses.
  solution.suffix += std::format("${}", mangle_type_for_instance(target));
  const auto name =
      method_instance_name(entry.name, method->decl->name, solution.suffix);
  const auto *instance = find_or_check_generic_instance(
      array, *method->decl, method->owner, method->file_id, solution, name,
      &scoped_params, target, method->block_type_params);
  if (instance == nullptr) {
    return;
  }
  // The instance's name already carries its length, so it is a plain
  // function name rather than a `Type::method` pair to be joined.
  conversion->second.callee.decl = instance;
  conversion->second.callee.impl_target_type = "";
}

/// If `target` is a user type implementing `std.traits.from_array`,
/// records the constructor this literal should be handed to
/// (`session_.array_literal_conversions_[&array].callee`, `.array_type` left
/// for the caller to fill in once the literal's own length is known) and
/// answers the element type the literal's elements must have. Pure
/// resolution — does not itself infer the elements, so a caller that has
/// already inferred them (the no-expectation default-`list` path,
/// `infer_array` below) can call this without re-inferring.
auto checker::resolve_array_literal_conversion(const ast::array_expr &array,
                                               type_id &target)
    -> std::optional<type_id> {
  const auto entry =
      session_.types_.entry(target); // copy: leaves are minted below
  if (entry.kind != type_kind::struct_kind &&
      entry.kind != type_kind::sum_kind &&
      entry.kind != type_kind::opaque_kind) {
    return std::nullopt;
  }
  if (!type_has_trait(entry, "from_array")) {
    return std::nullopt;
  }
  const auto *method = find_method(entry, "from_array", target);
  if (method == nullptr) {
    return std::nullopt;
  }
  // The element type is the impl's `T`, recovered from its target pattern
  // the same way every other generic-impl question is answered here.
  auto bindings = param_subst{};
  if (method->block_type_params != nullptr) {
    match_params(method->impl_target_pattern, target, bindings);
    fill_target_holes(array, *method, bindings, target);
  }
  // `block_type_params` passed so the impl block's `T` resolves to the
  // parameter rather than to `unknown` — `substitute_solved` can bind a
  // type parameter but has nothing to bind an unknown to. Without it the
  // element type comes back abstract and the *caller* builds the literal
  // at an 8-byte stride while the callee reads it at the real width: a
  // silent wrong-value bug, which is exactly why this path is covered by
  // an exact-output test at two element widths.
  const auto params = signature_params(*method->decl, method->owner, false,
                                       method->block_type_params);
  if (params.empty()) {
    return std::nullopt;
  }
  const auto &array_param = session_.types_.entry(
      strip_refs(substitute_solved(params.front().type, bindings)));
  if (array_param.kind != type_kind::array_kind) {
    return std::nullopt;
  }
  session_.array_literal_conversions_[&array] = array_literal_conversion{
      .callee = resolved_callee{.decl = method->decl,
                                .owner_module = method->owner->module_name,
                                .impl_target_type = std::string(entry.name)},
      // Filled in by the caller once the literal's own length is known.
      .array_type = k_unknown_type};
  return array_param.result;
}

/// Gives each impl parameter the target left open its own leaf, and
/// rewrites `target` to carry them.
///
/// A target with a hole is a generic callee's `xs: list[T]` with `T` not
/// yet solved: the argument check opens it to `list[?]`. Read as it was,
/// the element type came back as the impl's own `T`, and every element of
/// `first([5, 6])` was refused as "expected `T`, found `int32`". With a
/// leaf, the elements say what the list holds, and the call solves its
/// `T` from the `list[?a]` the literal answers with — the same path
/// `let xs = [5, 6]; first(xs)` already took.
auto checker::fill_target_holes(const ast::array_expr &array,
                                const method_entry &method,
                                param_subst &bindings, type_id &target)
    -> void {
  auto filled = false;
  for (const auto &param : *method.block_type_params) {
    if (param.is_value_param || param.name.empty()) {
      continue;
    }
    const auto id = param_id(param);
    const auto it = bindings.find(id);
    if (it != bindings.end() && it->second != k_unknown_type) {
      continue;
    }
    const auto leaf = leaf_ctxt_.fresh_type(
        "the element type of this list literal",
        source_location{.file_id = file_.file_id_, .span = array.span});
    bindings.insert_or_assign(id, leaf);
    filled = true;
    // Elements always solve the leaf; an empty `[]` (`count([])`) may
    // leave it open, and the literal that minted it is where to say so.
    if (array.elements.empty() && array.fill_value == nullptr) {
      pending_leaf_literals_.push_back(
          pending_leaf_literal{.literal = &array,
                               .element = leaf,
                               .span = array.span,
                               .file = file_.file_id_,
                               .module = file_.module_,
                               .current_template = function_.current_template_,
                               .report_only = true});
    }
  }
  if (filled) {
    target = substitute_solved(method.impl_target_pattern, bindings);
  }
}

/// If `expected` is a user type implementing `std.traits.from_array`,
/// infers the literal's elements against it and records the conversion.
///
/// This is what makes `[1, 2, 3]` reach a collection other than `list[T]`.
/// The literal keeps its
/// `array[T, n]` shape all the way through lowering; the only change is
/// the call wrapped around it.
///
/// `target` is the collection the literal answers with: `expected` without
/// its references, and with any hole filled (`fill_target_holes`).
auto checker::try_wire_from_array(const ast::array_expr &array,
                                  type_id expected, type_id &target)
    -> std::optional<type_id> {
  target = strip_refs(expected);
  return resolve_array_literal_conversion(array, target);
}

/// The real `list[T]` (`std.list`, prelude-re-exported) instantiated at
/// `element` — what an unannotated `[1, 2, 3]` or `[0; 4]` literal answers
/// with (the "one rule": no expectation means `list`, for every literal
/// spelling). `list` is an ordinary generic user type, so this is exactly
/// `find_prelude_type`/`instantiate_user_type`'s path, just with the
/// element type already in hand as a `type_id` rather than an AST node —
/// `make_user_type` is the one piece of that path that takes arguments
/// directly.
auto checker::resolve_list_type(type_id element) -> type_id {
  const auto found = find_prelude_type("list");
  if (!found.has_value()) {
    return k_unknown_type;
  }
  return make_user_type(*found->first, found->second, {element});
}

/// The real `range[T]` (`std.traits`, prelude-re-exported) instantiated at
/// `element` — what `a..b` evaluates to. An ordinary struct for the same
/// reason `list` is: `impl index[range[usize]]` receives one as a value
/// and reads `.start`/`.end`, which a desugared-away builtin could not
/// provide.
///
/// A session with no stdlib (most unit tests) falls back to the interned
/// builtin shape. Nothing there can index by range or read `.start` — the
/// two things that need a real value — but `for i in a..b` and
/// range-indexing an `array`/`slice`/`str` lower the bounds directly and
/// never touch the type, so those keep working exactly as they did.
auto checker::resolve_range_type(type_id element) -> type_id {
  const auto found = find_prelude_type("range");
  if (!found.has_value()) {
    return session_.types_.builtin_generic("range", {element});
  }
  return make_user_type(*found->first, found->second, {element});
}

/// Whether `type` is a `range[T]`, and its `T` if so. The one place that
/// knows how a range is spelled, so the checks scattered across indexing,
/// iteration and `in` cannot drift from what `resolve_range_type` builds.
auto checker::range_element_type(type_id type) -> std::optional<type_id> {
  const auto &entry = session_.types_.entry(strip_refs(type));
  if (entry.name != "range" || entry.args.size() != 1) {
    return std::nullopt;
  }
  if (entry.kind != type_kind::struct_kind &&
      entry.kind != type_kind::builtin_generic_kind) {
    return std::nullopt;
  }
  return entry.args.front();
}

auto checker::is_range_type(type_id type) -> bool {
  return range_element_type(type).has_value();
}

/// Resolves a `for ... => yield` comprehension's `list[T]::new`/
/// `list[T]::push` calls into `session_.comprehension_dispatches_`
/// (`hir::lower_for_expr` builds the actual calls from it). Mirrors
/// `resolve_own_drop`'s `find_method`/`instantiate_impl_method_for`
/// pattern — the same shape whether the method takes `self` (`push`) or
/// not (the static `new`).
auto checker::resolve_comprehension_dispatch(const ast::for_expr &expr,
                                             type_id list_type) -> void {
  record_new_push_dispatch(expr, list_type, session_.comprehension_dispatches_);
}

/// Resolves (and instantiates) the `new`/`push` pair for `list_type`,
/// attributing the instantiation to `site`. Shared by the two desugarings
/// that build a list by repeated appending with nothing in the source
/// naming either call: a `for ... => yield` comprehension, and a `[v; n]`
/// fill whose count is only known at runtime.
auto checker::resolve_new_push_dispatch(const ast::node &site,
                                        type_id list_type)
    -> std::optional<comprehension_dispatch> {
  if (mentions_template_param(list_type)) {
    return std::nullopt;
  }
  const auto &entry = session_.types_.entry(list_type);
  const auto *new_method = find_method(entry, "new", list_type);
  const auto *push_method = find_method(entry, "push", list_type);
  if (new_method == nullptr || push_method == nullptr) {
    return std::nullopt;
  }
  const auto build_callee = [&](const method_entry &method) -> resolved_callee {
    const auto *instance =
        instantiate_impl_method_for(site, method, entry, list_type);
    return resolved_callee{.decl = instance != nullptr ? instance : method.decl,
                           .owner_module = method.owner->module_name,
                           .impl_target_type =
                               instance != nullptr ? "" : entry.name};
  };
  return comprehension_dispatch{.new_callee = build_callee(*new_method),
                                .push_callee = build_callee(*push_method),
                                .list_type = list_type};
}

/// Records the `from_array` conversion for a literal whose elements have
/// already been inferred (the no-expectation default-`list` path below) —
/// `resolve_array_literal_conversion` only resolves the constructor, so
/// this fills in `.array_type`/instantiates the same way the explicit-
/// annotation path at the top of `infer_array` does, without re-inferring
/// the elements a second time.
auto checker::wire_default_list(const ast::array_expr &array, type_id element,
                                std::optional<uint64_t> count) -> type_id {
  auto target = resolve_list_type(element);
  // `[v; n]` with a count only known at runtime. `from_array` takes an
  // `array[T, n]`, which has no meaning without a compile-time `n` — so
  // there is nothing for the constructor to be handed, and wiring one
  // anyway left lowering naming an instance that was never compiled
  // ("call to `list::from_array` could not be resolved to a function in
  // this compiled module"). Fill it the way a comprehension builds its
  // result instead: `new()`, then `push(v)` `n` times.
  if (array.fill_value != nullptr && !count.has_value()) {
    record_new_push_dispatch(array, target, session_.runtime_fill_dispatches_);
    return target;
  }
  if (!resolve_array_literal_conversion(array, target).has_value()) {
    return target; // `list[T]` always has `from_array` — see std.list.
  }
  session_.array_literal_conversions_[&array].array_type = array_with_length(
      element, count.has_value() ? session_.types_.const_value(
                                       session_.types_.usize_type(), *count)
                                 : k_unknown_type);
  if (count.has_value()) {
    instantiate_from_array_for(array, target, *count);
  }
  return target;
}

/// Checks `[v; n]`'s repeat count, which must be a `usize`.
///
/// The expected type alone does not enforce this: it is a hint a literal
/// adopts, and a binding that already has a type simply ignores it. Before
/// this check, `[7; n]` for an `n: int32` type-checked silently and then
/// lowered to a counting loop whose index is a real `usize`, so the LLVM
/// tier built an `icmp` over an `i64` and an `i32` and tripped an assertion
/// inside LLVM itself — while the bytecode VM, whose values are all tagged
/// 64-bit words, ran it and produced the right answer. Cross-tier agreement
/// could not have caught it; only one tier was ever wrong.
///
/// Phrased to match the diagnostic a non-`usize` *index* already gets
/// (`cannot index ... expected an index of type `usize``), because a repeat
/// count and a subscript are the same requirement wearing different syntax.
auto checker::check_fill_count(const ast::expr &count) -> void {
  const auto found =
      strip_refs(infer_expr(count, session_.types_.builtin("usize")));
  const auto usize = session_.types_.usize_type();
  if (found == usize || session_.types_.is_unknown(found) ||
      found == k_error_type) {
    return;
  }
  error_with_help(
      count.span,
      std::format("a repeat count must be a `usize`, but this one is `{}`",
                  session_.types_.display(found)),
      "expected a count of type `usize`",
      std::format("`[v; n]` repeats `v` exactly `n` times, and a count of "
                  "things is a `usize` here the same way a subscript is. "
                  "Give `n` type `usize`, or convert it at the literal."));
}

/// Defers a literal whose element type still carries an open leaf.
///
/// `[1, 2, 3]` and `[7; n]` alike: wiring one now names
/// `list::from_array$3$list___` or `list::new$list___`, an instance of a
/// type no value has. The empty `[]` is the same situation with the leaf
/// minted here rather than inherited from an element, so one deferral
/// covers all three. `true` when it was deferred and the caller should
/// answer with the list type and stop.
auto checker::defer_open_literal(const ast::array_expr &array, type_id element,
                                 std::optional<uint64_t> count) -> bool {
  auto seen = std::unordered_set<type_id>{};
  if (!mentions_type_var(element, seen)) {
    return false;
  }
  pending_leaf_literals_.push_back(
      pending_leaf_literal{.literal = &array,
                           .element = element,
                           .count = count,
                           .span = array.span,
                           .file = file_.file_id_,
                           .module = file_.module_,
                           .current_template = function_.current_template_});
  return true;
}

auto checker::infer_array(const ast::array_expr &array, type_id expected)
    -> type_id {
  // A user collection that says what a literal of it means: check the
  // elements against its element type, then answer with the collection.
  // Done before the builtin shapes below, because `expected` being a user
  // type rules every one of them out anyway.
  auto target = k_unknown_type;
  if (const auto from_array_element =
          try_wire_from_array(array, expected, target)) {
    // The element type is a requirement, not a hint: without the check an
    // `int64` element of a `list[int32]` was accepted and truncated, and an
    // open `list[?a]` (the second `[x]` in `[[1], [x]]`) never learned
    // `?a` from its elements.
    if (array.fill_value != nullptr) {
      const auto found = infer_expr(*array.fill_value, *from_array_element);
      type_mismatch(array.fill_value->span, *from_array_element, found,
                    "for this element", array.fill_value.get());
      if (array.fill_count != nullptr) {
        check_fill_count(*array.fill_count);
      }
    }
    for (const auto &item : array.elements) {
      if (item != nullptr) {
        const auto found = infer_expr(*item, *from_array_element);
        type_mismatch(item->span, *from_array_element, found,
                      "for this element", item.get());
      }
    }
    // The literal still *is* an `array[T, n]` underneath; record which one
    // so lowering can build it before wrapping it in the constructor call.
    const auto count = array.fill_value != nullptr
                           ? fill_count_of(array)
                           : std::optional<uint64_t>{array.elements.size()};
    // `[v; n]` for a runtime `n` has no `array[T, n]` to be: `from_array`
    // takes one, and there is no compile-time length to give it. Fill by
    // repeated appending instead, the same way a comprehension builds its
    // result — see `checked_types::runtime_fill_dispatches`.
    if (array.fill_value != nullptr && !count.has_value()) {
      session_.array_literal_conversions_.erase(&array);
      // An open target (`[[1], [0; n]]`) cannot name its `new`/`push` yet;
      // `record_new_push_dispatch` waits for the leaf.
      if (mentions_type_var(target)) {
        record_new_push_dispatch(array, target,
                                 session_.runtime_fill_dispatches_);
        return target;
      }
      if (const auto dispatch = resolve_new_push_dispatch(array, target)) {
        session_.runtime_fill_dispatches_[&array] = *dispatch;
      } else if (!mentions_template_param(target)) {
        error_with_help(
            array.span,
            std::format("`{}` cannot be built from a fill whose count is "
                        "only known at runtime",
                        session_.types_.display(target)),
            "no way to fill this collection",
            "A `[value; count]` literal with a constant count becomes an "
            "`array`, which `from_array` accepts. With a runtime count "
            "there is no sized array to hand it, so the collection is "
            "filled by appending instead — which needs a `new()` and a "
            "`push(value)`. Add those, or use a constant count.");
        return k_error_type;
      }
      return target;
    }
    session_.array_literal_conversions_[&array].array_type = array_with_length(
        *from_array_element,
        count.has_value()
            ? session_.types_.const_value(session_.types_.usize_type(), *count)
            : k_unknown_type);
    if (count.has_value()) {
      instantiate_from_array_for(array, target, *count);
    }
    return target;
  }
  const auto &expected_entry = session_.types_.entry(strip_refs(expected));
  auto element_expected = k_unknown_type;
  auto expected_is_array = false;
  if (expected_entry.kind == type_kind::array_kind) {
    element_expected = expected_entry.result;
    expected_is_array = true;
  } else if (expected_entry.kind == type_kind::builtin_generic_kind &&
             expected_entry.name == "slice") {
    element_expected =
        expected_entry.args.empty() ? k_unknown_type : expected_entry.args[0];
  }

  if (array.fill_value != nullptr) {
    const auto element = infer_expr(*array.fill_value, element_expected);
    auto count = std::optional<uint64_t>{};
    if (array.fill_count != nullptr) {
      check_fill_count(*array.fill_count);
      if (array.fill_count->kind == ast::node_kind::literal_expr) {
        const auto &lit = ast::cast<const ast::literal_expr>(*array.fill_count);
        if (lit.lit_kind == token_kind::int_lit) {
          count = parse_integer_literal(lit.value);
        }
      } else if (const auto folded = explicit_value_argument(*array.fill_count);
                 folded.has_value() && *folded >= 0) {
        // Inside a monomorphized instance the count may be a value
        // parameter (`[0; n]` in a `def zeros[n: usize]`), which is a
        // definite number here even though it isn't spelled as a literal —
        // fold it, so the array gets a known length and the backends have
        // an allocation size. In the *template* there is no constant to
        // fold to and this correctly finds none.
        count = static_cast<uint64_t>(*folded);
      }
    }
    // No expected type: a sequence literal is a `list`, exactly as the
    // element form a few lines below already was. One rule for both
    // spellings — "a literal with a constant repeat count is an array,
    // otherwise a list" would make `[0; 4]` and `[0, 0, 0, 0]` different
    // types, a distinction nothing else in the language draws
    // (`01-core/06-collections-list-array.md`: "For general-purpose code,
    // `list` is the default choice"). An `array` is spelled by asking for
    // one, which is what `expected_is_array` below serves.
    if (!expected_is_array &&
        session_.types_.is_unknown(strip_refs(expected))) {
      if (defer_open_literal(array, element, count)) {
        return resolve_list_type(element);
      }
      return wire_default_list(array, element, count);
    }
    auto length =
        count.has_value()
            ? session_.types_.const_value(session_.types_.usize_type(), *count)
            : k_unknown_type;
    // A template's `[0; n]` is an `array[int32, n]`: the count is a value
    // parameter, which is a length in its own right.
    if (!count.has_value() && array.fill_count != nullptr) {
      if (const auto poly =
              value_poly(*array.fill_count, current_resolve_ctx())) {
        length =
            session_.types_.symbolic_value(session_.types_.usize_type(), *poly);
      }
    }
    return array_with_length(element, length);
  }

  auto element = element_expected;
  for (const auto &item : array.elements) {
    if (item == nullptr) {
      continue;
    }
    const auto found = infer_expr(*item, element);
    // A leaf element is not "nothing known yet": it is a variable the
    // remaining elements must agree with, so it is kept and unified with
    // rather than replaced by the last element's own.
    if (session_.types_.is_unknown(element) &&
        session_.types_.entry(element).kind != type_kind::type_var_kind) {
      element = found;
    } else {
      type_mismatch(item->span, element, found, "for this element", item.get());
    }
  }

  if (expected_is_array) {
    if (expected_entry.array_size.has_value() &&
        *expected_entry.array_size != array.elements.size()) {
      error(array.span,
            std::format("array literal has {} element{}, but `{}` holds "
                        "exactly {}",
                        array.elements.size(),
                        array.elements.size() == 1 ? "" : "s",
                        session_.types_.display(strip_refs(expected)),
                        *expected_entry.array_size),
            "wrong number of elements");
    }
    return array_with_length(
        element, session_.types_.const_value(session_.types_.usize_type(),
                                             array.elements.size()));
  }
  // `[]` with nothing to infer from. There is no element to type it by and
  // no expected type to adopt, and inference does not run backwards from a
  // later `xs.push(v)` — so without this the literal would build a
  // `list[?]`, instantiate `list::from_array` with an unsolved `T`, and
  // surface as "cannot tell which `T` this call to `push` means" pointing
  // into `src/std/list.cn`: a diagnostic about the standard library's
  // internals for a mistake in the user's own line.
  // A literal whose element type still carries an open leaf — `[1, 2, 3]`
  // with nothing yet saying what the `1` is. Wiring it now names
  // `list::from_array$3$list___`, an instance of a type no value has; the
  // empty `[]` below is the same situation with the leaf minted here
  // rather than inherited from an element. One deferral covers both.
  if (defer_open_literal(array, element,
                         std::optional<uint64_t>{array.elements.size()})) {
    return resolve_list_type(element);
  }
  if (array.elements.empty() && session_.types_.is_unknown(element)) {
    // Phase 8: the literal stands for `list[?a]` and waits. Whatever the
    // binding later flows into — a `push`, a return, an argument — meets
    // its declared type in `type_mismatch`, which is where `?a` is solved.
    // If nothing ever does, `flush_leaf_literals` reports it against *this*
    // line rather than letting a `list[?]` reach the standard library's
    // internals and be reported there.
    const auto leaf = leaf_ctxt_.fresh_type(
        "the element type of this `[]`",
        source_location{.file_id = file_.file_id_, .span = array.span});
    pending_leaf_literals_.push_back(
        pending_leaf_literal{.literal = &array,
                             .element = leaf,
                             .span = array.span,
                             .file = file_.file_id_,
                             .module = file_.module_,
                             .current_template = function_.current_template_});
    return resolve_list_type(leaf);
  }
  return wire_default_list(array, element, array.elements.size());
}

} // namespace cinder::semantic::detail
